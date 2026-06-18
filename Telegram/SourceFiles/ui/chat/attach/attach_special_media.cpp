/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/chat/attach/attach_special_media.h"

#include "base/random.h"
#include "ffmpeg/ffmpeg_bytes_io_wrap.h"
#include "ffmpeg/ffmpeg_utility.h"
#include "logs.h"
#include "media/audio/media_audio.h"
#include "ui/controls/round_video_recorder.h"

#include <QtCore/QDir>
#include <QtCore/QFile>

#include <array>
#include <vector>

namespace Ui {
namespace {

using namespace FFmpeg;

constexpr auto kVoiceSampleRate = Media::Player::kDefaultFrequency;
constexpr auto kMaxVoiceDuration = 60 * 60 * crl::time(1000);
constexpr auto kRoundSide = 400;
constexpr auto kRoundMaxDuration = 60 * crl::time(1000);
constexpr auto kRoundMinDuration = crl::time(200);
constexpr auto kRoundAudioRate = 48'000;
constexpr auto kRoundVideoBitRate = 2 * 1024 * 1024;

struct BytesInput {
	QByteArray bytes;
	ReadBytesWrap wrap;
	FormatPointer input;
};

[[nodiscard]] FormatPointer OpenInputFromBytes(
		not_null<ReadBytesWrap*> wrap) {
	return MakeFormatPointer(
		wrap.get(),
		&ReadBytesWrap::Read,
		nullptr,
		&ReadBytesWrap::Seek);
}

[[nodiscard]] std::optional<BytesInput> OpenInputFromFileBytes(
		QByteArray fileBytes) {
	if (fileBytes.isEmpty()) {
		return std::nullopt;
	}
	auto result = BytesInput{
		.bytes = std::move(fileBytes),
	};
	result.wrap = ReadBytesWrap{
		.size = result.bytes.size(),
		.data = reinterpret_cast<const uchar*>(result.bytes.constData()),
	};
	result.input = OpenInputFromBytes(&result.wrap);
	if (!result.input) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] bool WritePacket(
		not_null<AVFormatContext*> output,
		AVPacket *packet,
		AVStream *outStream,
		AVStream *inStream) {
	av_packet_rescale_ts(packet, inStream->time_base, outStream->time_base);
	packet->stream_index = outStream->index;
	packet->pos = -1;
	const auto error = AvErrorWrap(av_interleaved_write_frame(output, packet));
	return !error;
}

[[nodiscard]] crl::time DurationFromFormat(not_null<AVFormatContext*> format) {
	if (format->duration > 0) {
		return PtsToTimeCeil(
			format->duration,
			AVRational{ 1, AV_TIME_BASE });
	}
	return 0;
}

[[nodiscard]] bool SeekInputToStart(
		not_null<AVFormatContext*> format,
		int streamId) {
	avformat_flush(format);
	if (av_seek_frame(format, -1, 0, AVSEEK_FLAG_BACKWARD) >= 0) {
		return true;
	}
	if (avformat_seek_file(
			format,
			streamId,
			0,
			0,
			0,
			AVSEEK_FLAG_BACKWARD) >= 0) {
		return true;
	}
	if (avformat_seek_file(
			format,
			-1,
			0,
			0,
			0,
			AVSEEK_FLAG_BACKWARD) >= 0) {
		return true;
	}
	if (format->pb && avio_seek(format->pb, 0, SEEK_SET) >= 0) {
		avformat_flush(format);
		return true;
	}
	return false;
}

void FlushInputDecoders(
		const CodecPointer &video,
		const CodecPointer &audio) {
	if (video) {
		avcodec_flush_buffers(video.get());
	}
	if (audio) {
		avcodec_flush_buffers(audio.get());
	}
}

void CloseFileInput(FormatPointer &input) {
	if (!input) {
		return;
	}
	auto raw = input.release();
	avformat_close_input(&raw);
}

void CloseOutputFormat(FormatPointer &output) {
	if (!output) {
		return;
	}
	auto raw = output.release();
	avformat_free_context(raw);
}

[[nodiscard]] std::optional<RoundFilePrepareResult> TryCopyCompatibleRound(
		const QString &path,
		not_null<AVFormatContext*> format) {
	const auto videoStreamId = av_find_best_stream(
		format,
		AVMEDIA_TYPE_VIDEO,
		-1,
		-1,
		nullptr,
		0);
	if (videoStreamId < 0) {
		return std::nullopt;
	}
	const auto videoStream = format->streams[videoStreamId];
	const auto videoCodec = videoStream->codecpar;
	if (videoCodec->codec_id != AV_CODEC_ID_H264) {
		return std::nullopt;
	}
	const auto width = videoCodec->width;
	const auto height = videoCodec->height;
	if (!width || !height || width != height) {
		return std::nullopt;
	}
	const auto audioStreamId = av_find_best_stream(
		format,
		AVMEDIA_TYPE_AUDIO,
		-1,
		-1,
		nullptr,
		0);
	if (audioStreamId >= 0) {
		const auto audioCodec = format->streams[audioStreamId]->codecpar;
		if (audioCodec->codec_id != AV_CODEC_ID_AAC) {
			return std::nullopt;
		}
	}
	const auto duration = DurationFromFormat(format);
	if (duration < kRoundMinDuration || duration > kRoundMaxDuration) {
		return std::nullopt;
	}
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return std::nullopt;
	}
	const auto content = file.readAll();
	if (content.isEmpty()) {
		return std::nullopt;
	}
	return RoundFilePrepareResult{
		.content = content,
		.duration = duration,
	};
}

void RewindBytesInput(BytesInput &input) {
	input.wrap.offset = 0;
	if (input.input->pb) {
		avio_seek(input.input->pb, 0, SEEK_SET);
	}
}

[[nodiscard]] FormatPointer OpenInputFromPath(const QString &path) {
	AVFormatContext *context = nullptr;
	const auto error = AvErrorWrap(avformat_open_input(
		&context,
		QFile::encodeName(path).constData(),
		nullptr,
		nullptr));
	if (error) {
		LogError(u"avformat_open_input"_q, error);
		return {};
	}
	return FormatPointer(context);
}

[[nodiscard]] std::optional<Media::AudioEditResult> CopyOpusStream(
		not_null<AVFormatContext*> input,
		int streamId) {
	const auto inStream = input->streams[streamId];
	auto outputWrap = WriteBytesWrap();
	auto output = MakeWriteFormatPointer(
		static_cast<void*>(&outputWrap),
		nullptr,
		&WriteBytesWrap::Write,
		&WriteBytesWrap::Seek,
		"opus"_q);
	if (!output) {
		return std::nullopt;
	}
	const auto outputGuard = gsl::finally([&] {
		CloseOutputFormat(output);
	});
	const auto outStream = avformat_new_stream(output.get(), nullptr);
	if (!outStream) {
		return std::nullopt;
	}
	auto error = AvErrorWrap(avcodec_parameters_copy(
		outStream->codecpar,
		inStream->codecpar));
	if (error) {
		return std::nullopt;
	}
	outStream->codecpar->codec_tag = 0;
	outStream->time_base = inStream->time_base;

	error = AvErrorWrap(avformat_write_header(output.get(), nullptr));
	if (error) {
		return std::nullopt;
	}

	if (!SeekInputToStart(input, streamId)) {
		return std::nullopt;
	}

	auto durationPts = int64(0);
	const auto maxDurationPts = TimeToPts(
		kMaxVoiceDuration,
		inStream->time_base);
	auto packet = AVPacket();
	av_init_packet(&packet);
	while (true) {
		error = AvErrorWrap(av_read_frame(input, &packet));
		if (error.code() == AVERROR_EOF) {
			break;
		} else if (error) {
			return std::nullopt;
		}
		const auto guard = gsl::finally([&] {
			av_packet_unref(&packet);
		});
		if (packet.stream_index != streamId) {
			continue;
		}
		const auto packetPosition = (packet.pts != AV_NOPTS_VALUE)
			? packet.pts
			: packet.dts;
		if (packetPosition != AV_NOPTS_VALUE) {
			durationPts = std::max(
				durationPts,
				packetPosition + std::max(int64(packet.duration), int64()));
			if (durationPts >= maxDurationPts) {
				break;
			}
		}
		if (!WritePacket(output.get(), &packet, outStream, inStream)) {
			return std::nullopt;
		}
	}

	error = AvErrorWrap(av_write_trailer(output.get()));
	if (error) {
		return std::nullopt;
	}

	auto result = Media::AudioEditResult();
	result.content = std::move(outputWrap.content);
	result.duration = durationPts
		? PtsToTimeCeil(durationPts, outStream->time_base)
		: crl::time(0);
	if (result.duration <= 0 || result.duration > kMaxVoiceDuration) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] std::optional<Media::AudioEditResult> TranscodeToOpus(
		not_null<AVFormatContext*> input,
		int streamId) {
	const auto inStream = input->streams[streamId];
	auto inCodecContext = MakeCodecPointer({ .stream = inStream });
	if (!inCodecContext) {
		return std::nullopt;
	}

	auto outputWrap = WriteBytesWrap();
	auto output = MakeWriteFormatPointer(
		static_cast<void*>(&outputWrap),
		nullptr,
		&WriteBytesWrap::Write,
		&WriteBytesWrap::Seek,
		"opus"_q);
	if (!output) {
		return std::nullopt;
	}
	const auto outputGuard = gsl::finally([&] {
		CloseOutputFormat(output);
	});

	const auto outCodec = avcodec_find_encoder_by_name("libopus");
	if (!outCodec) {
		return std::nullopt;
	}
	const auto outStream = avformat_new_stream(output.get(), nullptr);
	if (!outStream) {
		return std::nullopt;
	}
	auto outCodecContext = CodecPointer(avcodec_alloc_context3(outCodec));
	if (!outCodecContext) {
		return std::nullopt;
	}
	outCodecContext->bit_rate = 32000;
	outCodecContext->ch_layout = AV_CHANNEL_LAYOUT_MONO;
	outCodecContext->sample_rate = kVoiceSampleRate;
	outCodecContext->time_base = AVRational{ 1, kVoiceSampleRate };
	outCodecContext->sample_fmt = [&] {
		const auto preferred = {
			AV_SAMPLE_FMT_FLTP,
			AV_SAMPLE_FMT_S16,
		};
		for (const auto fmt : preferred) {
			for (auto p = outCodec->sample_fmts; p && *p != AV_SAMPLE_FMT_NONE; ++p) {
				if (*p == fmt) {
					return fmt;
				}
			}
		}
		return outCodec->sample_fmts
			? outCodec->sample_fmts[0]
			: AV_SAMPLE_FMT_S16;
	}();
	if (output->oformat->flags & AVFMT_GLOBALHEADER) {
		outCodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}
	auto error = AvErrorWrap(avcodec_open2(
		outCodecContext.get(),
		outCodec,
		nullptr));
	if (error) {
		LogError(u"voice out avcodec_open2"_q, error);
		return std::nullopt;
	}
	const auto sampleFormat = outCodecContext->sample_fmt;
	const auto channels = outCodecContext->ch_layout.nb_channels;
	error = AvErrorWrap(avcodec_parameters_from_context(
		outStream->codecpar,
		outCodecContext.get()));
	if (error) {
		LogError(u"voice avcodec_parameters_from_context"_q, error);
		return std::nullopt;
	}
	outStream->time_base = outCodecContext->time_base;

	error = AvErrorWrap(avformat_write_header(output.get(), nullptr));
	if (error) {
		LogError(u"voice avformat_write_header"_q, error);
		return std::nullopt;
	}

	avcodec_flush_buffers(inCodecContext.get());
	if (!SeekInputToStart(input, streamId)) {
		return std::nullopt;
	}
	avcodec_flush_buffers(inCodecContext.get());

	auto swrContext = MakeSwresamplePointer(
		&inCodecContext->ch_layout,
		inCodecContext->sample_fmt,
		inCodecContext->sample_rate,
		&outCodecContext->ch_layout,
		outCodecContext->sample_fmt,
		outCodecContext->sample_rate);
	if (!swrContext) {
		return std::nullopt;
	}

	const auto frameSize = (outCodecContext->frame_size > 0)
		? outCodecContext->frame_size
		: 960;
	const auto frameBytes = av_samples_get_buffer_size(
		nullptr,
		channels,
		frameSize,
		sampleFormat,
		1);
	if (frameBytes <= 0) {
		return std::nullopt;
	}
	const auto maxPts = int64(kMaxVoiceDuration)
		* kVoiceSampleRate
		/ 1000;
	constexpr auto kSwrChunkSamples = 4096;
	const auto swrChunkBytes = av_samples_get_buffer_size(
		nullptr,
		channels,
		kSwrChunkSamples,
		sampleFormat,
		1);
	if (swrChunkBytes <= 0) {
		return std::nullopt;
	}

	auto inFrame = MakeFramePointer();
	auto outFrame = MakeFramePointer();
	if (!inFrame || !outFrame) {
		return std::nullopt;
	}
	outFrame->format = outCodecContext->sample_fmt;
	outFrame->nb_samples = frameSize;
	av_channel_layout_copy(
		&outFrame->ch_layout,
		&outCodecContext->ch_layout);
	outFrame->sample_rate = outCodecContext->sample_rate;
	error = AvErrorWrap(av_frame_get_buffer(outFrame.get(), 0));
	if (error) {
		LogError(u"voice av_frame_get_buffer"_q, error);
		return std::nullopt;
	}

	std::vector<uint8_t> pending;
	std::vector<uint8_t> swrChunk(swrChunkBytes);
	pending.reserve(frameBytes * 4);

	const auto appendConverted = [&](int samples) {
		if (samples <= 0) {
			return;
		}
		const auto bytes = av_samples_get_buffer_size(
			nullptr,
			channels,
			samples,
			sampleFormat,
			1);
		if (bytes <= 0) {
			return;
		}
		pending.insert(
			pending.end(),
			swrChunk.begin(),
			swrChunk.begin() + bytes);
	};

	auto pts = int64(0);

	const auto drainEncoderPackets = [&]() -> AvErrorWrap {
		auto pkt = av_packet_alloc();
		const auto guard = gsl::finally([&] {
			av_packet_free(&pkt);
		});
		while (true) {
			error = AvErrorWrap(avcodec_receive_packet(
				outCodecContext.get(),
				pkt));
			if (error.code() == AVERROR(EAGAIN)
				|| error.code() == AVERROR_EOF) {
				return AvErrorWrap();
			} else if (error) {
				return error;
			}
			pkt->stream_index = outStream->index;
			av_packet_rescale_ts(
				pkt,
				outCodecContext->time_base,
				outStream->time_base);
			error = AvErrorWrap(av_interleaved_write_frame(output.get(), pkt));
			if (error) {
				return error;
			}
			av_packet_unref(pkt);
		}
	};

	const auto writeFrame = [&](AVFrame *frame) -> AvErrorWrap {
		if (frame) {
			while (true) {
				error = AvErrorWrap(avcodec_send_frame(
					outCodecContext.get(),
					frame));
				if (!error) {
					break;
				} else if (error.code() == AVERROR(EAGAIN)) {
					error = drainEncoderPackets();
					if (error) {
						return error;
					}
				} else if (error.code() != AVERROR_EOF) {
					return error;
				} else {
					break;
				}
			}
		} else {
			error = AvErrorWrap(avcodec_send_frame(
				outCodecContext.get(),
				nullptr));
			if (error && error.code() != AVERROR_EOF) {
				return error;
			}
		}
		return drainEncoderPackets();
	};

	const auto encodePending = [&](bool finalFlush) -> AvErrorWrap {
		while (pending.size() >= size_t(frameBytes) && pts < maxPts) {
			error = AvErrorWrap(av_frame_make_writable(outFrame.get()));
			if (error) {
				return error;
			}
			memcpy(
				outFrame->data[0],
				pending.data(),
				frameBytes);
			outFrame->nb_samples = frameSize;
			outFrame->pts = pts;
			pts += frameSize;
			pending.erase(
				pending.begin(),
				pending.begin() + frameBytes);
			error = writeFrame(outFrame.get());
			if (error) {
				return error;
			}
		}
		if (finalFlush && !pending.empty() && pts < maxPts) {
			const auto bytesPerSample = av_get_bytes_per_sample(sampleFormat);
			const auto tailSamples = int(pending.size()
				/ (bytesPerSample * channels));
			error = AvErrorWrap(av_frame_make_writable(outFrame.get()));
			if (error) {
				return error;
			}
			av_samples_set_silence(
				outFrame->data,
				0,
				frameSize,
				channels,
				sampleFormat);
			memcpy(
				outFrame->data[0],
				pending.data(),
				pending.size());
			outFrame->nb_samples = frameSize;
			outFrame->pts = pts;
			pts += tailSamples;
			pending.clear();
			error = writeFrame(outFrame.get());
			if (error) {
				return error;
			}
		}
		return AvErrorWrap();
	};

	auto packet = av_packet_alloc();
	const auto packetGuard = gsl::finally([&] {
		av_packet_free(&packet);
	});

	while (pts < maxPts) {
		error = AvErrorWrap(av_read_frame(input, packet));
		const auto finished = (error.code() == AVERROR_EOF);
		if (!finished) {
			if (error) {
				LogError(u"voice av_read_frame"_q, error);
				return std::nullopt;
			}
			const auto guard = gsl::finally([&] {
				av_packet_unref(packet);
			});
			if (packet->stream_index != streamId) {
				continue;
			}
			error = AvErrorWrap(avcodec_send_packet(
				inCodecContext.get(),
				packet));
			if (error) {
				LogError(u"voice avcodec_send_packet"_q, error);
				return std::nullopt;
			}
		} else {
			error = AvErrorWrap(avcodec_send_packet(
				inCodecContext.get(),
				nullptr));
			if (error && error.code() != AVERROR_EOF) {
				LogError(u"voice avcodec_send_packet flush"_q, error);
				return std::nullopt;
			}
		}

		while (true) {
			error = AvErrorWrap(avcodec_receive_frame(
				inCodecContext.get(),
				inFrame.get()));
			if (error.code() == AVERROR(EAGAIN)
				|| error.code() == AVERROR_EOF) {
				break;
			} else if (error) {
				LogError(u"voice avcodec_receive_frame"_q, error);
				return std::nullopt;
			}
			uint8_t *outData[1] = { swrChunk.data() };
			const auto converted = swr_convert(
				swrContext.get(),
				outData,
				kSwrChunkSamples,
				const_cast<const uint8_t**>(inFrame->data),
				inFrame->nb_samples);
			if (converted < 0) {
				return std::nullopt;
			} else if (converted > 0) {
				appendConverted(converted);
				error = encodePending(false);
				if (error) {
					LogError(u"voice writeFrame"_q, error);
					return std::nullopt;
				}
			}
			while (pts < maxPts) {
				const auto drained = swr_convert(
					swrContext.get(),
					outData,
					kSwrChunkSamples,
					nullptr,
					0);
				if (drained <= 0) {
					break;
				}
				appendConverted(drained);
				error = encodePending(false);
				if (error) {
					LogError(u"voice writeFrame"_q, error);
					return std::nullopt;
				}
			}
			if (pts >= maxPts) {
				break;
			}
		}

		if (finished || pts >= maxPts) {
			break;
		}
	}

	{
		uint8_t *outData[1] = { swrChunk.data() };
		while (pts < maxPts) {
			const auto drained = swr_convert(
				swrContext.get(),
				outData,
				kSwrChunkSamples,
				nullptr,
				0);
			if (drained <= 0) {
				break;
			}
			appendConverted(drained);
		}
	}

	error = encodePending(true);
	if (error) {
		LogError(u"voice writeFrame pending"_q, error);
		return std::nullopt;
	}
	error = writeFrame(nullptr);
	if (error) {
		LogError(u"voice writeFrame flush"_q, error);
		return std::nullopt;
	}
	error = AvErrorWrap(av_write_trailer(output.get()));
	if (error) {
		LogError(u"voice av_write_trailer"_q, error);
		return std::nullopt;
	}

	auto result = Media::AudioEditResult();
	result.content = std::move(outputWrap.content);
	result.duration = PtsToTimeCeil(pts, outCodecContext->time_base);
	if (result.duration < crl::time(200) || result.content.isEmpty()) {
		return std::nullopt;
	}
	return result;
}

class RoundFileEncoder final {
public:
	~RoundFileEncoder() {
		cleanupTempFile();
	}

	[[nodiscard]] std::optional<RoundFilePrepareResult> encode(
			not_null<AVFormatContext*> format) {
		const auto sourceDuration = DurationFromFormat(format);
		const auto videoStreamId = av_find_best_stream(
			format,
			AVMEDIA_TYPE_VIDEO,
			-1,
			-1,
			nullptr,
			0);
		if (videoStreamId < 0) {
			return std::nullopt;
		}
		if (!initOutput()) {
			return std::nullopt;
		}
		_videoInStream = format->streams[videoStreamId];
		_videoInContext = MakeCodecPointer({ .stream = _videoInStream });
		if (!_videoInContext) {
			return std::nullopt;
		}

		const auto audioStreamId = av_find_best_stream(
			format,
			AVMEDIA_TYPE_AUDIO,
			-1,
			-1,
			nullptr,
			0);
		if (audioStreamId >= 0) {
			_audioInStream = format->streams[audioStreamId];
			_audioInContext = MakeCodecPointer({ .stream = _audioInStream });
			if (!_audioInContext || !initAudio()) {
				_audioInContext = nullptr;
				_audioInStream = nullptr;
			}
		}

		auto error = AvErrorWrap(avformat_write_header(_output.get(), nullptr));
		if (error) {
			LogError(u"round avformat_write_header"_q, error);
			return std::nullopt;
		}

		FlushInputDecoders(_videoInContext, _audioInContext);
		if (!SeekInputToStart(format, videoStreamId)) {
			return std::nullopt;
		}
		FlushInputDecoders(_videoInContext, _audioInContext);

		auto packet = AVPacket();
		av_init_packet(&packet);
		while (true) {
			error = AvErrorWrap(av_read_frame(format, &packet));
			if (error.code() == AVERROR_EOF) {
				break;
			} else if (error) {
				LogError(u"round av_read_frame"_q, error);
				return std::nullopt;
			}
			const auto guard = gsl::finally([&] {
				av_packet_unref(&packet);
			});
			if (packet.stream_index == videoStreamId) {
				if (_videoFinished) {
					continue;
				}
				if (!processVideoPacket(&packet)) {
					return std::nullopt;
				}
			} else if (_audioInStream
				&& !_audioFinished
				&& packet.stream_index == _audioInStream->index) {
				if (!processAudioPacket(&packet)) {
					_audioInContext = nullptr;
					_audioInStream = nullptr;
					_audioFinished = true;
				}
			}
		}

		if (!flushVideo() || !flushAudio()) {
			return std::nullopt;
		}
		error = AvErrorWrap(av_write_trailer(_output.get()));
		if (error) {
			LogError(u"round av_write_trailer"_q, error);
			return std::nullopt;
		}
		if (_duration < kRoundMinDuration || _tempPath.isEmpty()) {
			return std::nullopt;
		}
		if (sourceDuration > kRoundMinDuration
			&& _duration + 1000 < sourceDuration) {
			return std::nullopt;
		}
		const auto duration = _duration;
		const auto tempPath = _tempPath;
		_tempPath.clear();
		deinit();
		return RoundFilePrepareResult{
			.tempPath = tempPath,
			.duration = duration,
		};
	}

private:
	void cleanupTempFile() {
		if (!_tempPath.isEmpty()) {
			QFile::remove(_tempPath);
			_tempPath = QString();
		}
	}

	void deinit() {
		_swsContext = nullptr;
		_swrContext = nullptr;
		_videoFrame = nullptr;
		_audioFrame = nullptr;
		_videoInContext = nullptr;
		_videoOutContext = nullptr;
		_audioInContext = nullptr;
		_audioOutContext = nullptr;
		_videoInStream = nullptr;
		_audioInStream = nullptr;
		_videoOutStream = nullptr;
		_audioOutStream = nullptr;
		if (_output) {
			const auto raw = _output.release();
			if (raw->pb && !(raw->oformat->flags & AVFMT_NOFILE)) {
				avio_closep(&raw->pb);
			}
			avformat_free_context(raw);
		}
	}

	[[nodiscard]] bool drainEncoderPackets(
			const CodecPointer &codec,
			AVStream *stream) {
		auto pkt = av_packet_alloc();
		const auto guard = gsl::finally([&] {
			av_packet_free(&pkt);
		});
		while (true) {
			auto error = AvErrorWrap(avcodec_receive_packet(codec.get(), pkt));
			if (error.code() == AVERROR(EAGAIN)
				|| error.code() == AVERROR_EOF) {
				return true;
			} else if (error) {
				return false;
			}
			pkt->stream_index = stream->index;
			av_packet_rescale_ts(pkt, codec->time_base, stream->time_base);
			error = AvErrorWrap(av_interleaved_write_frame(_output.get(), pkt));
			if (error) {
				return false;
			}
			av_packet_unref(pkt);
		}
	}

	[[nodiscard]] bool initOutput() {
		cleanupTempFile();
		_tempPath = QDir::temp().absoluteFilePath(
			u"ayu_round_"_q
			+ QString::number(base::RandomValue<uint64>(), 16)
			+ u".mp4"_q);
		const auto path = QFile::encodeName(_tempPath);
		AVFormatContext *context = nullptr;
		auto error = AvErrorWrap(avformat_alloc_output_context2(
			&context,
			nullptr,
			"mp4",
			path.constData()));
		if (error || !context) {
			LogError(u"round avformat_alloc_output_context2"_q, error);
			cleanupTempFile();
			return false;
		}
		if (!(context->oformat->flags & AVFMT_NOFILE)) {
			error = AvErrorWrap(avio_open(
				&context->pb,
				path.constData(),
				AVIO_FLAG_WRITE));
			if (error) {
				LogError(u"round avio_open"_q, error);
				avformat_free_context(context);
				cleanupTempFile();
				return false;
			}
		}
		_output = FormatPointer(context);
		if (!_output) {
			cleanupTempFile();
			return false;
		}
		auto videoCodec = avcodec_find_encoder_by_name("libopenh264");
		if (!videoCodec) {
			videoCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
		}
		if (!videoCodec) {
			return false;
		}
		_videoOutStream = avformat_new_stream(_output.get(), videoCodec);
		if (!_videoOutStream) {
			return false;
		}
		auto videoContext = CodecPointer(avcodec_alloc_context3(videoCodec));
		if (!videoContext) {
			return false;
		}
		videoContext->codec_id = videoCodec->id;
		videoContext->codec_type = AVMEDIA_TYPE_VIDEO;
		videoContext->width = kRoundSide;
		videoContext->height = kRoundSide;
		videoContext->time_base = AVRational{ 1, 1'000'000 };
		videoContext->framerate = AVRational{ 0, 1 };
		videoContext->pix_fmt = AV_PIX_FMT_YUV420P;
		videoContext->bit_rate = kRoundVideoBitRate;
		if (_output->oformat->flags & AVFMT_GLOBALHEADER) {
			videoContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		}
		error = AvErrorWrap(avcodec_open2(
			videoContext.get(),
			videoCodec,
			nullptr));
		if (error) {
			LogError(u"round video avcodec_open2"_q, error);
			return false;
		}
		_videoOutContext = std::move(videoContext);
		error = AvErrorWrap(avcodec_parameters_from_context(
			_videoOutStream->codecpar,
			_videoOutContext.get()));
		if (error) {
			LogError(u"round video avcodec_parameters_from_context"_q, error);
			return false;
		}
		_videoOutStream->time_base = _videoOutContext->time_base;
		_videoFrame = MakeFramePointer();
		if (!_videoFrame) {
			return false;
		}
		_videoFrame->format = _videoOutContext->pix_fmt;
		_videoFrame->width = kRoundSide;
		_videoFrame->height = kRoundSide;
		error = AvErrorWrap(av_frame_get_buffer(_videoFrame.get(), 0));
		return !error;
	}

	[[nodiscard]] bool initAudio() {
		const auto audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
		if (!audioCodec) {
			return false;
		}
		_audioOutContext = CodecPointer(avcodec_alloc_context3(audioCodec));
		if (!_audioOutContext) {
			return false;
		}
		_audioOutContext->sample_fmt = AV_SAMPLE_FMT_FLTP;
		_audioOutContext->bit_rate = 128000;
		_audioOutContext->ch_layout = AV_CHANNEL_LAYOUT_MONO;
		_audioOutContext->sample_rate = kRoundAudioRate;
		if (_output->oformat->flags & AVFMT_GLOBALHEADER) {
			_audioOutContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		}
		auto error = AvErrorWrap(avcodec_open2(
			_audioOutContext.get(),
			audioCodec,
			nullptr));
		if (error) {
			return false;
		}
		_swrContext = MakeSwresamplePointer(
			&_audioInContext->ch_layout,
			_audioInContext->sample_fmt,
			_audioInContext->sample_rate,
			&_audioOutContext->ch_layout,
			_audioOutContext->sample_fmt,
			_audioOutContext->sample_rate);
		if (!_swrContext) {
			return false;
		}
		_audioFrame = MakeFramePointer();
		if (!_audioFrame) {
			return false;
		}
		_audioFrame->format = _audioOutContext->sample_fmt;
		av_channel_layout_copy(
			&_audioFrame->ch_layout,
			&_audioOutContext->ch_layout);
		_audioFrame->sample_rate = _audioOutContext->sample_rate;
		const auto audioFrameSamples = std::max(_audioOutContext->frame_size, 1024);
		_audioFrame->nb_samples = audioFrameSamples;
		error = AvErrorWrap(av_frame_get_buffer(_audioFrame.get(), 0));
		if (error) {
			return false;
		}
		_audioOutStream = avformat_new_stream(_output.get(), audioCodec);
		if (!_audioOutStream) {
			return false;
		}
		error = AvErrorWrap(avcodec_parameters_from_context(
			_audioOutStream->codecpar,
			_audioOutContext.get()));
		if (error) {
			return false;
		}
		_audioOutStream->time_base = _audioOutContext->time_base;
		return true;
	}

	[[nodiscard]] bool processVideoPacket(AVPacket *packet) {
		auto error = AvErrorWrap(avcodec_send_packet(
			_videoInContext.get(),
			packet));
		if (error) {
			return false;
		}
		auto frame = MakeFramePointer();
		if (!frame) {
			return false;
		}
		while (true) {
			error = AvErrorWrap(avcodec_receive_frame(
				_videoInContext.get(),
				frame.get()));
			if (error.code() == AVERROR(EAGAIN)
				|| error.code() == AVERROR_EOF) {
				break;
			} else if (error) {
				return false;
			}
			const auto ptsMs = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
				? PtsToTimeCeil(
					frame->best_effort_timestamp,
					_videoInStream->time_base)
				: _duration;
			if (ptsMs >= kRoundMaxDuration) {
				_videoFinished = true;
				break;
			}
			if (!encodeVideoFrame(frame.get(), ptsMs)) {
				return false;
			}
			av_frame_unref(frame.get());
		}
		return true;
	}

	[[nodiscard]] bool encodeVideoFrame(AVFrame *frame, crl::time ptsMs) {
		const auto width = frame->width;
		const auto height = frame->height;
		if (!width || !height) {
			return true;
		}
		if (av_frame_make_writable(_videoFrame.get()) < 0) {
			return false;
		}
		const auto side = std::min(width, height) & ~1;
		if (!side) {
			return true;
		}
		const auto cropX = ((width - side) / 2) & ~1;
		const auto cropY = ((height - side) / 2) & ~1;

		const uint8_t *srcData[4] = {};
		int srcLinesize[4] = {};
		const auto isPlanarYuv = (frame->format == AV_PIX_FMT_YUV420P)
			|| (frame->format == AV_PIX_FMT_YUVJ420P)
			|| (frame->format == AV_PIX_FMT_NV12);
		if (isPlanarYuv) {
			_swsContext = MakeSwscalePointer(
				QSize(side, side),
				frame->format,
				QSize(kRoundSide, kRoundSide),
				AV_PIX_FMT_YUV420P,
				&_swsContext);
			if (!_swsContext) {
				return false;
			}
			srcData[0] = frame->data[0] + cropY * frame->linesize[0] + cropX;
			srcLinesize[0] = frame->linesize[0];
			if (frame->format == AV_PIX_FMT_NV12) {
				srcData[1] = frame->data[1]
					+ (cropY / 2) * frame->linesize[1]
					+ cropX;
				srcLinesize[1] = frame->linesize[1];
			} else {
				srcData[1] = frame->data[1]
					+ (cropY / 2) * frame->linesize[1]
					+ (cropX / 2);
				srcData[2] = frame->data[2]
					+ (cropY / 2) * frame->linesize[2]
					+ (cropX / 2);
				srcLinesize[1] = frame->linesize[1];
				srcLinesize[2] = frame->linesize[2];
			}
			sws_scale(
				_swsContext.get(),
				srcData,
				srcLinesize,
				0,
				side,
				_videoFrame->data,
				_videoFrame->linesize);
		} else {
			_swsContext = MakeSwscalePointer(
				QSize(width, height),
				frame->format,
				QSize(kRoundSide, kRoundSide),
				AV_PIX_FMT_YUV420P,
				&_swsContext);
			if (!_swsContext) {
				return false;
			}
			for (auto i = 0; i != 4; ++i) {
				srcData[i] = frame->data[i];
				srcLinesize[i] = frame->linesize[i];
			}
			sws_scale(
				_swsContext.get(),
				srcData,
				srcLinesize,
				0,
				height,
				_videoFrame->data,
				_videoFrame->linesize);
		}

		if (_videoFirstPtsMs < 0) {
			_videoFirstPtsMs = ptsMs;
		}
		_videoFrame->pts = (ptsMs - _videoFirstPtsMs) * 1000;
		_duration = std::max(_duration, ptsMs - _videoFirstPtsMs);
		return writeFrame(_videoFrame.get(), _videoOutContext, _videoOutStream);
	}

	[[nodiscard]] bool processAudioPacket(AVPacket *packet) {
		if (!_audioOutContext) {
			return true;
		}
		auto error = AvErrorWrap(avcodec_send_packet(
			_audioInContext.get(),
			packet));
		if (error) {
			return false;
		}
		auto frame = MakeFramePointer();
		if (!frame) {
			return false;
		}
		while (true) {
			error = AvErrorWrap(avcodec_receive_frame(
				_audioInContext.get(),
				frame.get()));
			if (error.code() == AVERROR(EAGAIN)
				|| error.code() == AVERROR_EOF) {
				break;
			} else if (error) {
				return false;
			}
			if (av_frame_make_writable(_audioFrame.get()) < 0) {
				return false;
			}
			const auto converted = swr_convert(
				_swrContext.get(),
				_audioFrame->data,
				_audioFrame->nb_samples,
				const_cast<const uint8_t**>(frame->data),
				frame->nb_samples);
			if (converted < 0) {
				return false;
			} else if (!converted) {
				av_frame_unref(frame.get());
				break;
			}
			_audioFrame->nb_samples = converted;
			_audioFrame->pts = _audioPts;
			_audioPts += _audioFrame->nb_samples;
			if (_audioPts >= kRoundMaxDuration * int64(kRoundAudioRate) / 1000) {
				_audioFinished = true;
				break;
			}
			if (!writeFrame(_audioFrame.get(), _audioOutContext, _audioOutStream)) {
				return false;
			}
			av_frame_unref(frame.get());
		}
		return true;
	}

	[[nodiscard]] bool writeFrame(
			AVFrame *frame,
			const CodecPointer &codec,
			AVStream *stream) {
		if (frame) {
			while (true) {
				auto error = AvErrorWrap(avcodec_send_frame(
					codec.get(),
					frame));
				if (!error) {
					break;
				} else if (error.code() == AVERROR(EAGAIN)) {
					if (!drainEncoderPackets(codec, stream)) {
						return false;
					}
				} else {
					return false;
				}
			}
		} else {
			auto error = AvErrorWrap(avcodec_send_frame(codec.get(), nullptr));
			if (error && error.code() != AVERROR_EOF) {
				return false;
			}
		}
		return drainEncoderPackets(codec, stream);
	}

	[[nodiscard]] bool flushVideo() {
		return writeFrame(nullptr, _videoOutContext, _videoOutStream);
	}

	[[nodiscard]] bool flushAudio() {
		if (!_audioOutContext) {
			return true;
		}
		return writeFrame(nullptr, _audioOutContext, _audioOutStream);
	}

	QString _tempPath;
	FormatPointer _output;
	CodecPointer _videoInContext;
	CodecPointer _videoOutContext;
	CodecPointer _audioInContext;
	CodecPointer _audioOutContext;
	FramePointer _videoFrame;
	FramePointer _audioFrame;
	SwscalePointer _swsContext;
	SwresamplePointer _swrContext;
	AVStream *_videoInStream = nullptr;
	AVStream *_audioInStream = nullptr;
	AVStream *_videoOutStream = nullptr;
	AVStream *_audioOutStream = nullptr;
	crl::time _duration = 0;
	crl::time _videoFirstPtsMs = -1;
	int64 _audioPts = 0;
	bool _videoFinished = false;
	bool _audioFinished = false;

};

} // namespace

std::optional<Media::AudioEditResult> PrepareVoiceFromFile(
		const QString &path) {
	auto input = OpenInputFromPath(path);
	if (!input) {
		return std::nullopt;
	}
	auto error = AvErrorWrap(avformat_find_stream_info(
		input.get(),
		nullptr));
	if (error) {
		LogError(u"voice avformat_find_stream_info"_q, error);
		return std::nullopt;
	}
	const auto streamId = av_find_best_stream(
		input.get(),
		AVMEDIA_TYPE_AUDIO,
		-1,
		-1,
		nullptr,
		0);
	if (streamId < 0) {
		return std::nullopt;
	}
	const auto stream = input->streams[streamId];
	const auto formatName = input->iformat ? input->iformat->name : "";
	const auto isOggOpus = (formatName == u"ogg"_q)
		&& stream->codecpar->codec_id == AV_CODEC_ID_OPUS;

	std::optional<Media::AudioEditResult> result;
	if (isOggOpus) {
		result = CopyOpusStream(input.get(), streamId);
	} else {
		result = TranscodeToOpus(input.get(), streamId);
	}
	CloseFileInput(input);
	if (!result) {
		return std::nullopt;
	}
	return result;
}

std::optional<RoundFilePrepareResult> PrepareRoundFromFile(const QString &path) {
	auto input = OpenInputFromPath(path);
	if (!input) {
		return std::nullopt;
	}
	auto error = AvErrorWrap(avformat_find_stream_info(
		input.get(),
		nullptr));
	if (error) {
		LogError(u"round avformat_find_stream_info"_q, error);
		return std::nullopt;
	}
	if (auto copied = TryCopyCompatibleRound(path, input.get())) {
		return copied;
	}
	std::optional<RoundFilePrepareResult> encoded;
	{
		RoundFileEncoder encoder;
		encoded = encoder.encode(input.get());
	}
	CloseFileInput(input);
	if (!encoded) {
		return std::nullopt;
	}
	return std::move(*encoded);
}

} // namespace Ui
