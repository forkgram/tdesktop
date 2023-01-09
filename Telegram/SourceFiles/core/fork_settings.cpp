/*
Author: 23rd.
*/
#include "core/fork_settings.h"

#include "storage/serialize_common.h"

namespace Core {

namespace {

constexpr auto kDefaultStickerSize = 256;

} // namespace

ForkSettings::ForkSettings() {
}

QByteArray ForkSettings::serialize() const {

	auto size = sizeof(qint32) * 4
		+ Serialize::stringSize(_uriScheme)
		+ Serialize::stringSize(_searchEngineUrl)
		+ sizeof(qint32) * 5;

	auto result = QByteArray();
	result.reserve(size);
	{
		QDataStream stream(&result, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		stream
			<< qint32(_squareUserpics ? 1 : 0)
			<< qint32(_audioFade ? 1 : 0)
			<< qint32(_askUriScheme ? 1 : 0)
			<< qint32(_lastSeenInDialogs ? 1 : 0)
			<< _uriScheme
			<< _searchEngineUrl
			<< qint32(_searchEngine ? 1 : 0)
			<< qint32(_allRecentStickers ? 1 : 0)
			<< qint32(_customStickerSize)
			<< qint32(_useBlackTrayIcon ? 1 : 0)
			<< qint32(_useOriginalTrayIcon ? 1 : 0)
			<< qint32(_autoSubmitPasscode ? 1 : 0)
			<< qint32(_emojiPopupOnClick ? 1 : 0)
			<< qint32(_blockUsersInGroups ? 1 : 0)
			;
	}
	return result;
}

void ForkSettings::addFromSerialized(const QByteArray &serialized) {
	if (serialized.isEmpty()) {
		return;
	}

	QDataStream stream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);

	qint32 squareUserpics = _squareUserpics;
	qint32 audioFade = _audioFade;
	qint32 askUriScheme = _askUriScheme;
	qint32 lastSeenInDialogs = _lastSeenInDialogs;
	QString uriScheme = _uriScheme;
	QString searchEngineUrl = _searchEngineUrl;
	qint32 searchEngine = _searchEngine;
	qint32 allRecentStickers = _allRecentStickers;
	qint32 customStickerSize = _customStickerSize;
	qint32 useBlackTrayIcon = _useBlackTrayIcon;
	qint32 useOriginalTrayIcon = _useOriginalTrayIcon;
	qint32 autoSubmitPasscode = _autoSubmitPasscode;
	qint32 emojiPopupOnClick = _emojiPopupOnClick;
	qint32 blockUsersInGroups = _blockUsersInGroups;

	if (!stream.atEnd()) {
		stream
			>> squareUserpics
			>> audioFade
			>> askUriScheme
			>> lastSeenInDialogs
			>> uriScheme
			>> searchEngineUrl
			>> searchEngine
			>> allRecentStickers
			>> customStickerSize
			>> useBlackTrayIcon
			>> useOriginalTrayIcon
			>> autoSubmitPasscode
			>> emojiPopupOnClick
			>> blockUsersInGroups
			;
	}
	if (stream.status() != QDataStream::Ok) {
		LOG(("App Error: "
			"Bad data for Core::ForkSettings::constructFromSerialized()"));
		return;
	}
	_squareUserpics = (squareUserpics == 1);
	_audioFade = (audioFade == 1);
	_askUriScheme = (askUriScheme == 1);
	_lastSeenInDialogs = (lastSeenInDialogs == 1);
	_uriScheme = uriScheme;
	_searchEngineUrl = searchEngineUrl;
	_searchEngine = (searchEngine == 1);
	_allRecentStickers = (allRecentStickers == 1);
	_customStickerSize = customStickerSize
		? std::clamp(customStickerSize, 50, kDefaultStickerSize)
		: kDefaultStickerSize;
	_useBlackTrayIcon = (useBlackTrayIcon == 1);
	_useOriginalTrayIcon = (useOriginalTrayIcon == 1);
	_autoSubmitPasscode = (autoSubmitPasscode == 1);
	_emojiPopupOnClick = (emojiPopupOnClick == 1);
	_blockUsersInGroups = (blockUsersInGroups == 1);
}

void ForkSettings::resetOnLastLogout() {
	_squareUserpics = false;
	_audioFade = true;
	_askUriScheme = false;
	_uriScheme = qsl("");
	_lastSeenInDialogs = false;
	_searchEngineUrl = qsl("https://dgg.gg/%q");
	_searchEngine = false;
	_allRecentStickers = true;
	_customStickerSize = kDefaultStickerSize;
	_useOriginalTrayIcon = false;
	_autoSubmitPasscode = false;
	_emojiPopupOnClick = false;
	_blockUsersInGroups = false;
}

} // namespace Core
