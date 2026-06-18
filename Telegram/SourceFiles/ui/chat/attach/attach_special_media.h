/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "media/audio/media_audio_edit.h"

#include <optional>

class QString;

namespace Ui {

struct RoundFilePrepareResult {
	QByteArray content;
	QString tempPath;
	crl::time duration = 0;
};

[[nodiscard]] std::optional<Media::AudioEditResult> PrepareVoiceFromFile(
	const QString &path);
[[nodiscard]] std::optional<RoundFilePrepareResult> PrepareRoundFromFile(
	const QString &path);

} // namespace Ui
