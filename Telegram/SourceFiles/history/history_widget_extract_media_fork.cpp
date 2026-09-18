/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/history_widget_extract_media_fork.h"

#include "api/api_common.h"
#include "api/api_sending.h"
#include "chat_helpers/compose/compose_show.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_chat_participant_status.h"
#include "data/data_document.h"
#include "data/data_drafts.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_web_page.h"
#include "history/history.h"
#include "history/view/controls/history_view_webpage_processor.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/widgets/buttons.h"

#include "styles/style_chat_helpers.h"

namespace Fork {

ExtractMediaBar::ExtractMediaBar(not_null<QWidget*> parent, Hooks hooks)
: _hooks(std::move(hooks))
, _button(Ui::CreateChild<Ui::IconButton>(
	parent.get(),
	st::historyForkExtractMedia)) {
	_button->setAccessibleName(
		tr::lng_fork_extract_media_from_preview(tr::now));
	_button->setAcceptBoth(true);
	_button->addClickHandler([=](Qt::MouseButton button) {
		if (button == Qt::RightButton) {
			freezeAndClearField();
		} else {
			toggle();
		}
	});
	_button->hide();
}

ExtractMediaBar::~ExtractMediaBar() = default;

not_null<Ui::IconButton*> ExtractMediaBar::button() const {
	return _button;
}

bool ExtractMediaBar::active() const {
	return _active;
}

bool ExtractMediaBar::blocksPreviewUpdates() const {
	return _frozen != nullptr;
}

WebPageData *ExtractMediaBar::currentData() const {
	if (_frozen) {
		return _frozen;
	}
	const auto preview = _hooks.preview();
	return preview ? preview->data() : nullptr;
}

bool ExtractMediaBar::available() const {
	if (!_hooks.previewShown()) {
		return false;
	}
	const auto data = currentData();
	return data && (data->photo || data->document);
}

void ExtractMediaBar::updateIcon() {
	const auto icon = _active
		? &st::historyForkExtractMediaActiveIcon
		: nullptr;
	_button->setIconOverride(icon, icon);
}

void ExtractMediaBar::reset() {
	_frozen = nullptr;
	if (!_active) {
		return;
	}
	_active = false;
	updateIcon();
}

void ExtractMediaBar::toggle() {
	if (_active) {
		_active = false;
		_frozen = nullptr;
		if (const auto preview = _hooks.preview()) {
			preview->checkNow(true);
		}
	} else if (!available()) {
		_active = false;
		_frozen = nullptr;
	} else {
		_active = true;
		const auto preview = _hooks.preview();
		_frozen = preview ? preview->data() : nullptr;
	}
	updateIcon();
}

void ExtractMediaBar::freezeAndClearField() {
	if (!_active) {
		toggle();
		if (!_active) {
			return;
		}
	}
	if (_hooks.clearFieldTextUndoable) {
		_hooks.clearFieldTextUndoable();
		_hooks.saveDraftWithTextNow();
	}
}

void ExtractMediaBar::updateVisibility(bool barCancelShown) {
	const auto avail = available();
	if (!avail) {
		reset();
	}
	const auto shouldShow = avail && barCancelShown;
	if (shouldShow == !_button->isHidden()) {
		return;
	}
	if (shouldShow) {
		_button->show();
	} else {
		_button->hide();
	}
}

bool ExtractMediaBar::trySend(Api::SendOptions options) {
	if (!_active || !available()) {
		return false;
	}
	const auto history = _hooks.history();
	const auto peer = _hooks.peer();
	if (!history || !peer || !_hooks.canSendMessages()) {
		return false;
	}
	const auto data = currentData();
	if (!data || (!data->photo && !data->document)) {
		return false;
	}
	const auto restriction = data->document
		? Data::RestrictionError(peer, ChatRestriction::SendFiles)
		: Data::RestrictionError(peer, ChatRestriction::SendPhotos);
	if (restriction) {
		Data::ShowSendErrorToast(_hooks.show(), peer, restriction);
		return false;
	} else if (_hooks.showSlowmodeError && _hooks.showSlowmodeError()) {
		return false;
	}
	auto message = Api::MessageToSend(_hooks.prepareSendAction(options));
	message.textWithTags = _hooks.currentTextWithTags();

	const auto withPaymentApproved = [=](int approved) {
		auto copy = options;
		copy.starsApproved = approved;
		[[maybe_unused]] const auto resent = trySend(copy);
	};
	if (_hooks.checkSendPayment
		&& !_hooks.checkSendPayment(
			1,
			message.action.options,
			withPaymentApproved)) {
		return false;
	}

	if (data->document) {
		Api::SendExistingDocument(std::move(message), data->document);
	} else {
		Api::SendExistingPhoto(std::move(message), data->photo);
	}

	_hooks.clearFieldText();
	reset();
	if (const auto preview = _hooks.preview()) {
		preview->apply({ .removed = true });
	}
	_hooks.saveDraftWithTextNow();
	_hooks.hideSelectorControlsAnimated();
	_hooks.setInnerFocus();
	history->session().changes().historyUpdated(
		history,
		(options.scheduled
			? Data::HistoryUpdate::Flag::ScheduledSent
			: Data::HistoryUpdate::Flag::MessageSent));
	return true;
}

} // namespace Fork
