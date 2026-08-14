/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_fork.h"

#include "api/api_authorizations.h"
#include "apiwrap.h"
#include "base/timer.h"
#include "calls/calls_call.h"
#include "calls/calls_instance.h"
#include "calls/calls_video_bubble.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "platform/platform_specific.h"
#include "settings/sections/settings_main.h"
#include "settings/settings_builder.h"
#include "settings/settings_common_session.h"
#include "tgcalls/VideoCaptureInterface.h"
#include "ui/boxes/confirm_box.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/effects/animations.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/level_meter.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "webrtc/webrtc_audio_input_tester.h"
#include "webrtc/webrtc_create_adm.h"
#include "webrtc/webrtc_environment.h"
#include "webrtc/webrtc_video_track.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include "base/options.h"
#include "base/qthelp_url.h"
#include "base/weak_ptr.h"
#include "boxes/abstract_box.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "menu/menu_item_save_to_markdown.h"
#include "settings/settings_common.h"
#include "storage/localstorage.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"

namespace Settings {

namespace {
using langString = tr::phrase<>;
using SessionController = not_null<Window::SessionController*>;

class SettingBox : public Ui::BoxContent, public base::has_weak_ptr  {
public:
	explicit SettingBox(
		QWidget*,
		Fn<void(bool)> callback,
		langString title,
		langString info);

	void setInnerFocus() override;

protected:
	void prepare() override;

	virtual QString getOrSetGlobal(QString value) = 0;
	virtual bool isInvalidUrl(QString linkUrl) = 0;

	Fn<void(bool)> _callback;
	Fn<void()> _setInnerFocus;
	langString _info;
	langString _title;
};

SettingBox::SettingBox(
	QWidget*,
	Fn<void(bool)> callback,
	langString title,
	langString info)
: _callback(std::move(callback))
, _info(info)
, _title(title) {
	Expects(_callback != nullptr);
}

void SettingBox::setInnerFocus() {
	Expects(_setInnerFocus != nullptr);

	_setInnerFocus();
}

void SettingBox::prepare() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	const auto url = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::defaultInputField,
			_info(),
			getOrSetGlobal(QString())),
		st::markdownLinkFieldPadding);

	const auto submit = [=] {
		const auto linkUrl = url->getLastText();
		const auto isInvalid = isInvalidUrl(linkUrl);
		if (isInvalid) {
			url->showError();
			return;
		}
		const auto weak = base::make_weak(this);
		getOrSetGlobal(linkUrl);
		Core::App().saveSettings();
		_callback(!isInvalid);
		if (weak) {
			closeBox();
		}
	};

	url->submits(
	) | rpl::on_next([=] {
		submit();
	}, lifetime());

	setTitle(_title());

	addButton(tr::lng_box_ok(), submit);
	addButton(tr::lng_cancel(), [=] {
		_callback(!getOrSetGlobal(QString()).isEmpty());
		closeBox();
	});

	content->resizeToWidth(st::boxWidth);
	content->moveToLeft(0, 0);
	setDimensions(st::boxWidth, content->height());

	_setInnerFocus = [=] {
		url->setFocusFast();
	};
}

//////

class SearchEngineBox : public SettingBox {

	using SettingBox::SettingBox;

protected:
	QString getOrSetGlobal(QString value) override;
	bool isInvalidUrl(QString linkUrl) override;
};

QString SearchEngineBox::getOrSetGlobal(QString value) {
	if (value.isEmpty()) {
		return Core::App().settings().fork().searchEngineUrl();
	}
	Core::App().settings().fork().setSearchEngineUrl(value);
	return QString();
}

bool SearchEngineBox::isInvalidUrl(QString linkUrl) {
	linkUrl = qthelp::validate_url(linkUrl);
	return linkUrl.isEmpty() || linkUrl.indexOf("%q") == -1;
}


//////

class URISchemeBox : public SettingBox {

	using SettingBox::SettingBox;

protected:
	QString getOrSetGlobal(QString value) override;
	bool isInvalidUrl(QString linkUrl) override;
};

QString URISchemeBox::getOrSetGlobal(QString value) {
	if (value.isEmpty()) {
		return Core::App().settings().fork().uriScheme();
	}
	Core::App().settings().fork().setUriScheme(value);
	return QString();
}

bool URISchemeBox::isInvalidUrl(QString linkUrl) {
	return linkUrl.indexOf("://") < 2;
}

//////

class StickerSizeBox : public SettingBox {
	using SettingBox::SettingBox;

protected:
	QString getOrSetGlobal(QString value) override;
	bool isInvalidUrl(QString linkUrl) override;

private:
	int _startSize = 0;
};

QString StickerSizeBox::getOrSetGlobal(QString value) {
	if (value.isEmpty()) {
		if (!_startSize) {
			_startSize = Core::App().settings().fork().customStickerSize();
		} else if (_startSize
				== Core::App().settings().fork().customStickerSize()) {
			return QString();
		}
		return QString::number(
			Core::App().settings().fork().customStickerSize());
	}
	if (const auto number = value.toInt()) {
		Core::App().settings().fork().setCustomStickerSize(number);
	}
	return QString();
}

bool StickerSizeBox::isInvalidUrl(QString linkUrl) {
	const auto number = linkUrl.toInt();
	return !number || number < 50 || number > 256;
}

//////

class MarkdownClipboardTextBox : public Ui::BoxContent {
public:
	MarkdownClipboardTextBox(QWidget*) {
	}

	void setInnerFocus() override {
		Expects(_setInnerFocus != nullptr);

		_setInnerFocus();
	}

protected:
	void prepare() override;

private:
	Fn<void()> _setInnerFocus;

};

void MarkdownClipboardTextBox::prepare() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			tr::lng_settings_markdown_clipboard_text_label(),
			st::boxDividerLabel),
		st::defaultBoxDividerLabelPadding);

	auto &option = base::options::lookup<QString>(
		Menu::kOptionMarkdownClipboardText);

	const auto field = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_settings_markdown_clipboard_text_placeholder(),
			option.value()),
		st::markdownLinkFieldPadding);

	const auto submit = [=, &option] {
		option.set(field->getLastText());
		closeBox();
	};

	setTitle(tr::lng_settings_markdown_clipboard_text_box_title());

	addButton(tr::lng_box_ok(), submit);
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	content->moveToLeft(0, 0);
	setDimensionsToContent(st::boxWidth, content);

	_setInnerFocus = [=] {
		field->setFocusFast();
	};
}

//////

using namespace Builder;

void BuildForkSectionContent(SectionBuilder &builder) {
	const auto controller = builder.controller();
	struct State {
		rpl::variable<bool> checked;
	};

	const auto add = [&](
			auto id,
			QStringList keywords,
			auto title,
			auto checkedCallback,
			auto ok) {
		const auto checkbox = builder.addButton({
			.id = std::move(id),
			.title = std::move(title),
			.st = &st::settingsButtonNoIcon,
			.toggled = rpl::single(checkedCallback()),
			.keywords = std::move(keywords),
		});
		if (!checkbox) {
			return;
		}
		checkbox->toggledValue(
		) | rpl::filter([=](bool checked) {
			return (checked != checkedCallback());
		}) | rpl::on_next([=](bool checked) {
			ok(checked);
			Core::App().saveSettings();
		}, checkbox->lifetime());
	};

	const auto restartBox = [=](Fn<void()> ok, Fn<void()> cancel) {
		controller->show(
			Ui::MakeConfirmBox({
				.text = tr::lng_settings_need_restart(tr::now),
				.confirmed = [=] {
					ok();
					Core::App().saveSettings();
					Core::Restart();
				},
				.cancelled = [=](Fn<void()> &&close) {
					cancel();
					close();
				},
				.confirmText = tr::lng_settings_restart_now(tr::now)
			}),
			Ui::LayerOption::KeepOther);
	};
	const auto addWithBox = [&](
			auto id,
			QStringList keywords,
			auto title,
			auto checkedCallback,
			auto ok,
			auto customBox) {
		const auto state = std::make_shared<State>();
		const auto checkbox = builder.addButton({
			.id = std::move(id),
			.title = std::move(title),
			.st = &st::settingsButtonNoIcon,
			.toggled = rpl::single(
				checkedCallback()
			) | rpl::then(state->checked.changes()),
			.keywords = std::move(keywords),
		});
		if (!checkbox) {
			return;
		}
		checkbox->toggledValue(
		) | rpl::filter([=](bool checked) {
			return (checked != checkedCallback());
		}) | rpl::on_next([=](bool checked) {
			customBox(checked, state.get());
		}, checkbox->lifetime());
	};
	const auto addRestart = [&](
			auto id,
			QStringList keywords,
			auto title,
			auto checkedCallback,
			auto ok) {
		addWithBox(
			std::move(id),
			std::move(keywords),
			std::move(title),
			std::move(checkedCallback),
			std::move(ok),
			[=](bool checked, State *state) {
				restartBox(
					[=] { ok(checked); },
					[=] { state->checked.force_assign(!checked); });
			});
	};

	//
	addRestart(
		u"fork/square_avatars"_q,
		{ u"square"_q, u"avatars"_q, u"userpic"_q, u"circle"_q },
		tr::lng_settings_square_avatats(),
		[] { return Core::App().settings().fork().squareUserpics(); },
		[=](bool checked) {
			Core::App().settings().fork().setSquareUserpics(checked);
		});

	//
	add(
		u"fork/audio_fade"_q,
		{ u"audio"_q, u"fade"_q },
		tr::lng_settings_audio_fade(),
		[] { return Core::App().settings().fork().audioFade(); },
		[=](bool checked) {
			Core::App().settings().fork().setAudioFade(checked);
		});

	//
	addWithBox(
		u"fork/uri_scheme"_q,
		{ u"URI"_q, u"scheme"_q, u"custom link"_q },
		tr::lng_settings_uri_scheme(),
		[] { return Core::App().settings().fork().askUriScheme(); },
		[=](bool checked) {
			Core::App().settings().fork().setAskUriScheme(checked);
		},
		[=](bool checked, State *state) {
			const auto callback = [=](bool isSuccess) {
				if (isSuccess) {
					Core::App().settings().fork().setAskUriScheme(isSuccess);
					Core::App().saveSettings();
				} else {
					state->checked.force_assign(false);
				}
			};
			if (!checked) {
				Core::App().settings().fork().setAskUriScheme(false);
				Core::App().saveSettings();
				return;
			}
			controller->show(
				Box<URISchemeBox>(
					std::move(callback),
					tr::lng_settings_uri_scheme_box_title,
					tr::lng_settings_uri_scheme_field_label),
				Ui::LayerOption::KeepOther);
		});

	//
	add(
		u"fork/last_seen_in_dialogs"_q,
		{ u"last"_q, u"seen"_q, u"dialogs"_q, u"online"_q },
		tr::lng_settings_last_seen_in_dialogs(),
		[] { return Core::App().settings().fork().lastSeenInDialogs(); },
		[=](bool checked) {
			Core::App().settings().fork().setLastSeenInDialogs(checked);
		});

	//
	addWithBox(
		u"fork/custom_search"_q,
		{ u"custom"_q, u"search"_q, u"engine"_q },
		tr::lng_settings_search_engine(),
		[] { return Core::App().settings().fork().searchEngine(); },
		[=](bool checked) {
			Core::App().settings().fork().setSearchEngine(checked);
		},
		[=](bool checked, State *state) {
			const auto callback = [=](bool isSuccess) {
				if (isSuccess) {
					Core::App().settings().fork().setSearchEngine(isSuccess);
					Core::App().saveSettings();
				} else {
					state->checked.force_assign(false);
				}
			};
			if (!checked) {
				Core::App().settings().fork().setSearchEngine(false);
				Core::App().saveSettings();
				return;
			}
			controller->show(
				Box<SearchEngineBox>(
					std::move(callback),
					tr::lng_settings_search_engine_box_title,
					tr::lng_settings_search_engine_field_label),
				Ui::LayerOption::KeepOther);
		});

	//
	add(
		u"fork/all_recent_stickers"_q,
		{ u"all"_q, u"recent"_q, u"stickers"_q },
		tr::lng_settings_show_all_recent_stickers(),
		[] { return Core::App().settings().fork().allRecentStickers(); },
		[=](bool checked) {
			Core::App().settings().fork().setAllRecentStickers(checked);
		});

#ifndef Q_OS_LINUX
#ifdef Q_OS_WIN
	builder.addSkip();
	builder.addDivider();
	builder.addSkip();
	add(
		u"fork/use_black_tray_icon"_q,
		{ u"icon"_q, u"black"_q, u"tray"_q },
		tr::lng_settings_use_black_tray_icon(),
		[] { return Core::App().settings().fork().useBlackTrayIcon(); },
		[](bool checked) {
			Core::App().settings().fork().setUseBlackTrayIcon(checked);
			Core::App().saveSettings();
			Core::App().domain().notifyUnreadBadgeChanged();
		});
#else // !Q_OS_WIN
	builder.addSkip();
	builder.addDivider();
	builder.addSkip();
	addRestart(
		u"fork/use_black_tray_icon"_q,
		{ u"icon"_q, u"black"_q, u"tray"_q },
		tr::lng_settings_use_black_tray_icon(),
		[] { return Core::App().settings().fork().useBlackTrayIcon(); },
		[](bool checked) {
			Core::App().settings().fork().setUseBlackTrayIcon(checked);
		});
#endif // Q_OS_WIN

	addRestart(
		u"fork/use_original_tray_icon"_q,
		{ u"icon"_q, u"original"_q, u"tray"_q },
		tr::lng_settings_use_original_tray_icon(),
		[] { return Core::App().settings().fork().useOriginalTrayIcon(); },
		[](bool checked) {
			Core::App().settings().fork().setUseOriginalTrayIcon(checked);
		});
#endif // !Q_OS_LINUX
	builder.addSkip();
	builder.addDivider();
	builder.addSkip();

	//
	builder.addButton({
		.id = u"fork/custom_sticker_size"_q,
		.title = tr::lng_settings_custom_sticker_size(),
		.st = &st::settingsButton,
		.icon = { &st::menuIconStickers },
		.label = rpl::single(QString::number(Core::App().settings().fork().customStickerSize())),
		.onClick = [=] {
			controller->show(
				Box<StickerSizeBox>(
					[=](bool isSuccess) {
						if (isSuccess) {
							restartBox([] {}, [] {});
						}
					},
					tr::lng_settings_custom_sticker_size,
					tr::lng_settings_sticker_size_label));
		},
		.keywords = { u"custom"_q, u"sticker"_q, u"size"_q },
	});

	//
	builder.addButton({
		.id = u"fork/markdown_clipboard_text"_q,
		.title = tr::lng_settings_markdown_clipboard_text(),
		.st = &st::settingsButton,
		.icon = { &st::menuIconExport },
		.onClick = [=] {
			controller->show(Box<MarkdownClipboardTextBox>());
		},
		.keywords = {
			u"markdown"_q,
			u"clipboard"_q,
			u"save"_q,
			u"text"_q,
		},
	});

	//
	add(
		u"fork/auto_submit_passcode"_q,
		{ u"auto"_q, u"submit"_q, u"passcode"_q },
		tr::lng_settings_auto_submit_passcode(),
		[] { return Core::App().settings().fork().autoSubmitPasscode(); },
		[](bool checked) {
			Core::App().settings().fork().setAutoSubmitPasscode(checked);
			Core::App().saveSettings();
		});

	//
	addRestart(
		u"fork/emoji_on_click"_q,
		{ u"emoji"_q, u"click"_q, u"panel"_q },
		tr::lng_settings_emoji_on_click(),
		[] { return Core::App().settings().fork().emojiPopupOnClick(); },
		[](bool checked) {
			Core::App().settings().fork().setEmojiPopupOnClick(checked);
		});

	//
	add(
		u"fork/remember_media_menu"_q,
		{ u"remember"_q, u"media"_q, u"menu"_q },
		tr::lng_settings_remember_media_menu(),
		[] { return Core::App().settings().fork().addToMenuRememberMedia(); },
		[](bool checked) {
			Core::App().settings().fork().setAddToMenuRememberMedia(checked);
		});

	//
	addRestart(
		u"fork/hide_all_chats_tab"_q,
		{ u"hide"_q, u"all_chats"_q, u"tab"_q },
		tr::lng_settings_hide_all_chats_tab(),
		[] { return Core::App().settings().fork().hideAllChatsTab(); },
		[](bool checked) {
			Core::App().settings().fork().setHideAllChatsTab(checked);
		});

	//
	add(
		u"fork/disable_global_search"_q,
		{ u"disable"_q, u"global"_q, u"search"_q },
		tr::lng_settings_disable_global_search(),
		[] { return Core::App().settings().fork().globalSearchDisabled(); },
		[](bool checked) {
			Core::App().settings().fork().setGlobalSearchDisabled(checked);
		});

	//
	add(
		u"fork/forward_and_remove"_q,
		{ u"forward"_q, u"button"_q, u"remove"_q },
		tr::lng_settings_forward_and_remove(),
		[] { return Core::App().settings().fork().thirdButtonTopBar(); },
		[](bool checked) {
			Core::App().settings().fork().setThirdButtonTopBar(checked);
		});

	//
	add(
		u"fork/auto_copy_incoming_login_codes"_q,
		{ u"auto_copy"_q, u"login"_q, u"code"_q },
		tr::lng_settings_auto_copy_login_codes(),
		[] { return Core::App().settings().fork().copyLoginCode(); },
		[](bool checked) {
			Core::App().settings().fork().setCopyLoginCode(checked);
		});

	//
	add(
		u"fork/hide_archived_stories"_q,
		{ u"hide"_q, u"archived"_q, u"stories"_q },
		tr::lng_settings_hide_archived_stories(),
		[] { return Core::App().settings().fork().archivedStoriesAreHidden(); },
		[](bool checked) {
			Core::App().settings().fork().setArchivedStoriesAreHidden(checked);
		});

	//
	add(
		u"fork/hide_from_blocked_users"_q,
		{ u"hide"_q, u"blocked"_q, u"users"_q, u"messages"_q },
		tr::lng_settings_hide_from_blocked_users(),
		[] { return Core::App().settings().fork().hideFromBlockedUsers(); },
		[](bool checked) {
			Core::App().settings().fork().setHideFromBlockedUsers(checked);
		});

	add(
		u"fork/hide_voice_video_button"_q,
		{ u"hide"_q, u"voice"_q, u"video"_q, u"button"_q },
		tr::lng_settings_hide_voice_video_button(),
		[] { return Core::App().settings().fork().hideVoiceVideoButton(); },
		[](bool checked) {
			Core::App().settings().fork().setHideVoiceVideoButton(checked);
		});

	builder.addSkip();
	builder.addDivider();
	builder.addSkip();

	builder.addSubsectionTitle(tr::lng_filters_type_bots());
	//
	add(
		u"fork/skip_share_from_bot"_q,
		{ u"skip"_q, u"share"_q, u"bot"_q },
		tr::lng_settings_skip_share_from_bot(),
		[] { return Core::App().settings().fork().skipShareFromBot(); },
		[](bool checked) {
			Core::App().settings().fork().setSkipShareFromBot(checked);
		});
	add(
		u"fork/additional_buttons_web_bot"_q,
		{ u"additional"_q, u"button"_q, u"web_bot"_q },
		tr::lng_settings_additional_buttons_web_bot(),
		[] { return Core::App().settings().fork().additionalButtonsWebBot(); },
		[](bool checked) {
			Core::App().settings().fork().setAdditionalButtonsWebBot(checked);
		});

	builder.addSkip();
	builder.addDivider();
	builder.addSkip();
}

class Fork : public Section<Fork> {
public:
	Fork(QWidget *parent, not_null<Window::SessionController*> controller);
	~Fork();

	[[nodiscard]] rpl::producer<QString> title() override;
	void sectionSaveChanges(FnMut<void()> done) override;

private:
	void setupContent();

};

const auto kMeta = BuildHelper({
	.id = Fork::Id(),
	.parentId = MainId(),
	.title = &tr::lng_settings_section_fork,
	.icon = &st::menuIconForkSettings,
}, [](SectionBuilder &builder) {
	BuildForkSectionContent(builder);
});

const SectionBuildMethod kForkSection = kMeta.build;

Fork::Fork(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

Fork::~Fork() = default;

rpl::producer<QString> Fork::title() {
	return tr::lng_settings_section_fork();
}

void Fork::sectionSaveChanges(FnMut<void()> done) {
	done();
}

void Fork::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	build(content, kForkSection);
	Ui::ResizeFitChild(this, content);
}

} // namespace

Type ForkId() {
	return Fork::Id();
}

namespace Builder {

SectionBuildMethod ForkSection = kForkSection;

} // namespace Builder
} // namespace Settings
