/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class History;
class PeerData;
struct WebPageData;
struct TextWithTags;

namespace Api {
struct SendOptions;
struct SendAction;
} // namespace Api

namespace HistoryView::Controls {
class WebpageProcessor;
} // namespace HistoryView::Controls

namespace ChatHelpers {
class Show;
} // namespace ChatHelpers

namespace Ui {
class IconButton;
} // namespace Ui

namespace Fork {

class ExtractMediaBar final {
public:
	struct Hooks {
		Fn<HistoryView::Controls::WebpageProcessor*()> preview;
		Fn<PeerData*()> peer;
		Fn<History*()> history;
		Fn<bool()> canSendMessages;
		Fn<bool()> previewShown;
		Fn<std::shared_ptr<ChatHelpers::Show>()> show;
		Fn<Api::SendAction(Api::SendOptions)> prepareSendAction;
		Fn<bool(int, Api::SendOptions, Fn<void(int)>)> checkSendPayment;
		Fn<bool()> showSlowmodeError;
		Fn<TextWithTags()> currentTextWithTags;
		Fn<void()> clearFieldText;
		Fn<void()> clearFieldTextUndoable;
		Fn<void()> saveDraftWithTextNow;
		Fn<void()> hideSelectorControlsAnimated;
		Fn<void()> setInnerFocus;
	};

	ExtractMediaBar(not_null<QWidget*> parent, Hooks hooks);
	~ExtractMediaBar();

	[[nodiscard]] not_null<Ui::IconButton*> button() const;
	[[nodiscard]] bool active() const;
	[[nodiscard]] bool blocksPreviewUpdates() const;

	void toggle();
	void reset();
	void updateVisibility(bool barCancelShown);
	[[nodiscard]] bool trySend(Api::SendOptions options);

private:
	void freezeAndClearField();
	[[nodiscard]] bool available() const;
	[[nodiscard]] WebPageData *currentData() const;
	void updateIcon();

	Hooks _hooks;
	not_null<Ui::IconButton*> _button;
	WebPageData *_frozen = nullptr;
	bool _active = false;

};

} // namespace Fork
