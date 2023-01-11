/*
Author: 23rd.
*/
#pragma once

namespace Core {

class ForkSettings final {
public:
	ForkSettings();

	[[nodiscard]] QByteArray serialize() const;
	void addFromSerialized(const QByteArray &serialized);

	void resetOnLastLogout();

	[[nodiscard]] bool squareUserpics() const {
		return _squareUserpics;
	}
	void setSquareUserpics(bool newValue) {
		_squareUserpics = newValue;
	}
	[[nodiscard]] bool audioFade() const {
		return _audioFade;
	}
	void setAudioFade(bool newValue) {
		_audioFade = newValue;
	}
	[[nodiscard]] bool askUriScheme() const {
		return _askUriScheme;
	}
	void setAskUriScheme(bool newValue) {
		_askUriScheme = newValue;
	}
	[[nodiscard]] QString uriScheme() const {
		return _uriScheme;
	}
	void setUriScheme(QString newValue) {
		_uriScheme = newValue;
	}
	[[nodiscard]] bool lastSeenInDialogs() const {
		return _lastSeenInDialogs;
	}
	void setLastSeenInDialogs(bool newValue) {
		_lastSeenInDialogs = newValue;
	}
	[[nodiscard]] QString searchEngineUrl() const {
		return _searchEngineUrl;
	}
	void setSearchEngineUrl(QString newValue) {
		_searchEngineUrl = newValue;
	}
	[[nodiscard]] bool searchEngine() const {
		return _searchEngine;
	}
	void setSearchEngine(bool newValue) {
		_searchEngine = newValue;
	}
	[[nodiscard]] bool allRecentStickers() const {
		return _allRecentStickers;
	}
	void setAllRecentStickers(bool newValue) {
		_allRecentStickers = newValue;
	}
	[[nodiscard]] int customStickerSize() const {
		return _customStickerSize;
	}
	void setCustomStickerSize(int newValue) {
		_customStickerSize = newValue;
	}
	[[nodiscard]] bool useBlackTrayIcon() const {
		return _useBlackTrayIcon;
	}
	void setUseBlackTrayIcon(bool newValue) {
		_useBlackTrayIcon = newValue;
	}
	[[nodiscard]] bool useOriginalTrayIcon() const {
		return _useOriginalTrayIcon;
	}
	void setUseOriginalTrayIcon(bool newValue) {
		_useOriginalTrayIcon = newValue;
	}
	[[nodiscard]] bool autoSubmitPasscode() const {
		return _autoSubmitPasscode;
	}
	void setAutoSubmitPasscode(bool newValue) {
		_autoSubmitPasscode = newValue;
	}
	[[nodiscard]] bool emojiPopupOnClick() const {
		return _emojiPopupOnClick;
	}
	void setEmojiPopupOnClick(bool newValue) {
		_emojiPopupOnClick = newValue;
	}
	[[nodiscard]] bool blockUsersInGroups() const {
		return _blockUsersInGroups;
	}
	void setBlockUsersInGroups(bool newValue) {
		_blockUsersInGroups = newValue;
	}

private:
	bool _squareUserpics = false;
	bool _audioFade = true;
	bool _askUriScheme = false;
	bool _lastSeenInDialogs = false;
	QString _uriScheme = qsl("");
	QString _searchEngineUrl = qsl("https://dgg.gg/%q");
	bool _searchEngine = false;
	bool _allRecentStickers = true;
	int _customStickerSize = 256;
	bool _useBlackTrayIcon = false;
	bool _useOriginalTrayIcon = false;
	bool _autoSubmitPasscode = false;
	bool _emojiPopupOnClick = false;
	bool _blockUsersInGroups = false;

};

} // namespace Core

