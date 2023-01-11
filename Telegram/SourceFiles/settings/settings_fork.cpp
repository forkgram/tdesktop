/*
Author: 23rd.
*/
#include "settings/settings_fork.h"

#include "base/qthelp_url.h"
#include "base/weak_ptr.h"
#include "boxes/abstract_box.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "storage/localstorage.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/input_fields.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

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
		Core::App().saveSettingsDelayed();
		_callback(!isInvalid);
		if (weak) {
			closeBox();
		}
	};

	connect(url, &Ui::InputField::submitted, [=] {
		submit();
	});

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

void SetupForkContent(
	not_null<Ui::VerticalLayout*> container,
	SessionController controller) {

	auto wrap = object_ptr<Ui::VerticalLayout>(container);
	const auto inner = wrap.data();
	container->add(object_ptr<Ui::OverrideMargins>(
		container,
		std::move(wrap),
		QMargins(0, 0, 0, st::settingsCheckbox.margin.bottom())));

	const auto checkbox = [&](const QString &label, bool checked) {
		return object_ptr<Ui::Checkbox>(
			container,
			label,
			checked,
			st::settingsCheckbox);
	};
	const auto add = [&](const QString &label, bool checked, auto &&handle) {
		inner->add(
			checkbox(label, checked),
			st::settingsCheckboxPadding
		)->checkedChanges(
		) | rpl::start_with_next(
			std::move(handle),
			inner->lifetime());
	};

	const auto restartBox = [=](Fn<void()> ok, Fn<void()> cancel) {
		controller->show(
			Ui::MakeConfirmBox({
				.text = tr::lng_settings_need_restart(tr::now),
				.confirmed = [=] {
					ok();
					Core::App().saveSettingsDelayed(0);
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
	const auto addRestart = [&](
			const QString &label,
			auto checkedCallback,
			auto ok) {
		const auto checkRow = inner->add(
			checkbox(label, checkedCallback()),
			st::settingsCheckboxPadding
		);
		checkRow->checkedChanges(
		) | rpl::filter([=](bool checked) {
			return (checked != checkedCallback());
		}) | rpl::start_with_next([=](bool checked) {
			restartBox(
				[=] { ok(checked); },
				[=] { checkRow->setChecked(!checked); });
		}, inner->lifetime());
	};

	//
	addRestart(
		tr::lng_settings_square_avatats(tr::now),
		[] { return Core::App().settings().fork().squareUserpics(); },
		[=](bool checked) {
			Core::App().settings().fork().setSquareUserpics(checked);
		});

	//
	add(
		tr::lng_settings_audio_fade(tr::now),
		Core::App().settings().fork().audioFade(),
		[=](bool checked) {
			Core::App().settings().fork().setAudioFade(checked);
			Core::App().saveSettingsDelayed();
		});

	//
	const auto uriScheme = inner->add(
		checkbox(
			tr::lng_settings_uri_scheme(tr::now),
			Core::App().settings().fork().askUriScheme()),
		st::settingsCheckboxPadding
	);
	uriScheme->checkedChanges(
	) | rpl::start_with_next([=](bool checked) {
		if (checked) {
			auto callback = [=](bool isSuccess) {
				uriScheme->setChecked(isSuccess);
				Core::App().settings().fork().setAskUriScheme(checked);
				Core::App().saveSettingsDelayed();
			};
			controller->show(Box<URISchemeBox>(
				std::move(callback),
				tr::lng_settings_uri_scheme_box_title,
				tr::lng_settings_uri_scheme_field_label),
			Ui::LayerOption::KeepOther);
		} else {
			Core::App().settings().fork().setAskUriScheme(checked);
			Core::App().saveSettingsDelayed();
		}
	}, uriScheme->lifetime());

	//
	add(
		tr::lng_settings_last_seen_in_dialogs(tr::now),
		Core::App().settings().fork().lastSeenInDialogs(),
		[=](bool checked) {
			Core::App().settings().fork().setLastSeenInDialogs(checked);
			Core::App().saveSettingsDelayed();
		});

	//
	const auto searchEngine = inner->add(
		checkbox(
			tr::lng_settings_search_engine(tr::now),
			Core::App().settings().fork().searchEngine()),
		st::settingsCheckboxPadding
	);

	//
	searchEngine->checkedChanges(
	) | rpl::start_with_next([=](bool checked) {
		if (checked) {
			auto callback = [=](bool isSuccess) {
				searchEngine->setChecked(isSuccess);
				Core::App().settings().fork().setSearchEngine(checked);
				Core::App().saveSettingsDelayed();
			};
			controller->show(Box<SearchEngineBox>(
				std::move(callback),
				tr::lng_settings_search_engine_box_title,
				tr::lng_settings_search_engine_field_label),
			Ui::LayerOption::KeepOther);
		} else {
			Core::App().settings().fork().setSearchEngine(checked);
			Core::App().saveSettingsDelayed();
		}
	}, searchEngine->lifetime());

#ifndef Q_OS_LINUX
#ifdef Q_OS_WIN
	AddDivider(inner);
	add(
		tr::lng_settings_use_black_tray_icon(tr::now),
		Core::App().settings().fork().useBlackTrayIcon(),
		[](bool checked) {
			Core::App().settings().fork().setUseBlackTrayIcon(checked);
			Core::App().saveSettingsDelayed();
			Core::App().domain().notifyUnreadBadgeChanged();
		});
#else // !Q_OS_WIN
	AddDivider(inner);
	addRestart(
		tr::lng_settings_use_black_tray_icon(tr::now),
		[] { return Core::App().settings().fork().useBlackTrayIcon(); },
		[](bool checked) {
			Core::App().settings().fork().setUseBlackTrayIcon(checked);
		});
#endif // Q_OS_WIN

	addRestart(
		tr::lng_settings_use_original_tray_icon(tr::now),
		[] { return Core::App().settings().fork().useOriginalTrayIcon(); },
		[](bool checked) {
			Core::App().settings().fork().setUseOriginalTrayIcon(checked);
		});
#endif // !Q_OS_LINUX
	AddDivider(inner);

	AddButton(
		inner,
		tr::lng_settings_custom_sticker_size(),
		st::settingsButton,
		{ &st::settingsIconStickers, kIconLightOrange }
	)->addClickHandler([=] {
		controller->show(Box<StickerSizeBox>([=](bool isSuccess) {
			if (isSuccess) {
				restartBox([] {}, [] {});
			}
		},
		tr::lng_settings_custom_sticker_size,
		tr::lng_settings_sticker_size_label));
	});

	//
	add(
		tr::lng_settings_auto_submit_passcode(tr::now),
		Core::App().settings().fork().autoSubmitPasscode(),
		[=](bool checked) {
			Core::App().settings().fork().setAutoSubmitPasscode(checked);
			Core::App().saveSettingsDelayed();
		});

	//
	addRestart(
		tr::lng_settings_emoji_on_click(tr::now),
		[] { return Core::App().settings().fork().emojiPopupOnClick(); },
		[=](bool checked) {
			Core::App().settings().fork().setEmojiPopupOnClick(checked);
		});

	//
	addRestart(
		tr::lng_settings_block_users_in_groups(tr::now),
		[] { return Core::App().settings().fork().blockUsersInGroups(); },
		[=](bool checked) {
			Core::App().settings().fork().setBlockUsersInGroups(checked);
			Core::App().saveSettingsDelayed();
		});

	AddDivider(inner);

}

void SetupFork(
	not_null<Ui::VerticalLayout*> container,
	SessionController controller) {
	AddSkip(container, st::settingsCheckboxesSkip);

	auto wrap = object_ptr<Ui::VerticalLayout>(container);
	SetupForkContent(wrap.data(), controller);

	container->add(object_ptr<Ui::OverrideMargins>(
		container,
		std::move(wrap)));

	AddSkip(container, st::settingsCheckboxesSkip);
}

} // namespace

Fork::Fork(QWidget *parent, SessionController controller)
: Section(parent) {
	setupContent(controller);
}

rpl::producer<QString> Fork::title() {
	return tr::lng_settings_section_fork();
}

void Fork::setupContent(SessionController controller) {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	SetupFork(content, controller);

	Ui::ResizeFitChild(this, content);
}

} // namespace Settings
