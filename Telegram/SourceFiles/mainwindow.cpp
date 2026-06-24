/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mainwindow.h"

#include "data/data_document.h"
#include "data/data_session.h"
#include "data/data_document_media.h"
#include "dialogs/ui/dialogs_layout.h"
#include "history/history.h"
#include "history/history_drag_area.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/buttons.h"
#include "ui/painter.h"
#include "ui/widgets/shadow.h"
#include "ui/widgets/tooltip.h"
#include "ui/emoji_config.h"
#include "ui/ui_utility.h"
#include "lang/lang_cloud_manager.h"
#include "lang/lang_instance.h"
#include "core/sandbox.h"
#include "core/application.h"
#include "export/export_manager.h"
#include "inline_bots/bot_attach_web_view.h" // AttachWebView::cancel.
#include "intro/intro_widget.h"
#include "main/main_session.h"
#include "main/main_account.h" // Account::sessionValue.
#include "main/main_domain.h"
#include "mainwidget.h"
#include "ui/boxes/confirm_box.h"
#include "boxes/connection_box.h"
#include "storage/storage_account.h"
#include "storage/localstorage.h"
#include "apiwrap.h"
#include "api/api_updates.h"
#include "settings/settings_intro.h"
#include "base/options.h"
#include "window/notifications_manager.h"
#include "window/themes/window_theme.h"
#include "window/themes/window_theme_warning.h"
#include "window/window_main_menu.h"
#include "window/window_controller.h" // App::wnd.
#include "window/window_session_controller.h"
#include "window/window_setup_email.h"
#include "window/window_media_preview.h"
#include "styles/style_dialogs.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"

#include <QtGui/QWindow>

namespace {

// Code for testing languages is F7-F6-F7-F8
void FeedLangTestingKey(int key) {
	static auto codeState = 0;
	if ((codeState == 0 && key == Qt::Key_F7)
		|| (codeState == 1 && key == Qt::Key_F6)
		|| (codeState == 2 && key == Qt::Key_F7)
		|| (codeState == 3 && key == Qt::Key_F8)) {
		++codeState;
	} else {
		codeState = 0;
	}
	if (codeState == 4) {
		codeState = 0;
		Lang::CurrentCloudManager().switchToTestLanguage();
	}
}

base::options::toggle AutoScrollInactiveChat({
	.id = kOptionAutoScrollInactiveChat,
	.name = "Mark as read of inactive chat",
	.description = "Mark new messages as read and scroll the chat "
		"even when the window is not in focus.",
});

constexpr auto kFpsCounterMeasureWindow = crl::time(1000);
constexpr auto kFpsCounterRefreshInterval = crl::time(250);

[[nodiscard]] bool HasOnlyModifiersWithoutKeypad(
		Qt::KeyboardModifiers modifiers,
		Qt::KeyboardModifiers expected) {
	return (modifiers & ~Qt::KeypadModifier) == expected;
}

[[nodiscard]] bool IsFpsCounterKey(const QKeyEvent *e) {
	if (!e || e->isAutoRepeat() || (e->key() != Qt::Key_F)) {
		return false;
	}
#ifdef Q_OS_MAC
	return HasOnlyModifiersWithoutKeypad(
		e->modifiers(),
		Qt::AltModifier | Qt::ShiftModifier | Qt::ControlModifier)
		|| HasOnlyModifiersWithoutKeypad(
			e->modifiers(),
			Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier);
#else // Q_OS_MAC
	return HasOnlyModifiersWithoutKeypad(
		e->modifiers(),
		Qt::AltModifier | Qt::ShiftModifier | Qt::ControlModifier);
#endif // !Q_OS_MAC
}

[[nodiscard]] bool WidgetInHierarchy(
		not_null<const QWidget*> widget,
		not_null<const QWidget*> root) {
	return (widget == root) || root->isAncestorOf(widget);
}

} // namespace

const char kOptionAutoScrollInactiveChat[]
	= "auto-scroll-inactive-chat";

MainWindow::MainWindow(not_null<Window::Controller*> controller)
: Platform::MainWindow(controller) {
	resize(st::windowDefaultWidth, st::windowDefaultHeight);

	setLocale(QLocale(QLocale::English, QLocale::UnitedStates));

	using Window::Theme::BackgroundUpdate;
	Window::Theme::Background()->updates(
	) | rpl::on_next([=](const BackgroundUpdate &data) {
		themeUpdated(data);
	}, lifetime());

	Core::App().passcodeLockChanges(
	) | rpl::on_next([=] {
		updateGlobalMenu();
	}, lifetime());

	Ui::Emoji::Updated(
	) | rpl::on_next([=] {
		Ui::ForceFullRepaint(this);
	}, lifetime());

	_fpsCounterTimer.setCallback([=] {
		refreshFpsCounter();
	});

	setAttribute(Qt::WA_OpaquePaintEvent);
}

void MainWindow::initHook() {
	Platform::MainWindow::initHook();
	QCoreApplication::instance()->installEventFilter(this);
}

void MainWindow::applyInitialWorkMode() {
	const auto workMode = Core::App().settings().workMode();
	workmodeUpdated(workMode);

	if (controller().isPrimary()) {
		if (Core::App().settings().windowPosition().maximized) {
			DEBUG_LOG(("Window Pos: First show, setting maximized."));
			setWindowState(Qt::WindowMaximized);
		}
		if (cStartInTray()
			|| (cLaunchMode() == LaunchModeAutoStart
				&& cStartMinimized()
				&& !Core::App().passcodeLocked())) {
			DEBUG_LOG(("Window Pos: First show, setting minimized after."));
			if (workMode == Core::Settings::WorkMode::TrayOnly
				|| workMode == Core::Settings::WorkMode::WindowAndTray) {
				hide();
			} else {
				setWindowState(windowState() | Qt::WindowMinimized);
			}
		}
	}
	setPositionInited();
}

void MainWindow::finishFirstShow() {
	applyInitialWorkMode();
	createGlobalMenu();

	windowActiveValue(
	) | rpl::skip(1) | rpl::filter(
		!rpl::mappers::_1
	) | rpl::on_next([=] {
		Ui::Tooltip::Hide();
	}, lifetime());

	setAttribute(Qt::WA_NoSystemBackground);

	if (!_passcodeLock && !_setupEmailLock && _main) {
		_main->activate();
	} else if (!_passcodeLock && !_setupEmailLock && _intro) {
		_intro->setInnerFocus();
	}
}

void MainWindow::clearWidgetsHook() {
	_mediaPreview.destroy();
	_main.destroy();
	_intro.destroy();
	if (!Core::App().passcodeLocked()) {
		_passcodeLock.destroy();
	}
	_setupEmailLock.destroy();
}

bool MainWindow::countsForFpsCounter(QObject *object) const {
	const auto widget = qobject_cast<QWidget*>(object);
	if (!_fpsCounterVisible || !widget || (widget->window() != this)) {
		return false;
	}
	if (_fpsCounter && WidgetInHierarchy(widget, _fpsCounter)) {
		return false;
	}
	const auto visibleRoot = [&]() -> QWidget* {
		if (_testingThemeWarning && !_testingThemeWarning->isHidden()) {
			return _testingThemeWarning;
		} else if (_mediaPreview && !_mediaPreview->isHidden()) {
			return _mediaPreview;
		} else if (_layer && !_layer->isHidden()) {
			return _layer.get();
		} else if (_passcodeLock && !_passcodeLock->isHidden()) {
			return _passcodeLock;
		} else if (_setupEmailLock && !_setupEmailLock->isHidden()) {
			return _setupEmailLock;
		} else if (_main && !_main->isHidden()) {
			return _main;
		} else if (_intro && !_intro->isHidden()) {
			return _intro;
		}
		return nullptr;
	}();
	return !visibleRoot || WidgetInHierarchy(widget, visibleRoot);
}

bool MainWindow::matchesFpsCounterKey(
		QObject *object,
		const QKeyEvent *e) const {
	if (!IsFpsCounterKey(e)) {
		return false;
	}
	if (const auto widget = qobject_cast<QWidget*>(object)) {
		return widget->window() == this;
	} else if (const auto window = qobject_cast<QWindow*>(object)) {
		return window == windowHandle();
	}
	return false;
}

void MainWindow::ensureFpsCounterCreated() {
	if (_fpsCounter) {
		return;
	}

	_fpsCounter.create(bodyWidget());
	_fpsCounter->hide();
	_fpsCounter->setAttribute(Qt::WA_TransparentForMouseEvents);
	_fpsCounter->paintRequest() | rpl::on_next([=] {
		if (!_fpsCounter) {
			return;
		}
		const auto &tooltip = st::defaultTooltip;
		auto p = Painter(_fpsCounter);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(QPen(st::boxTextFgGood, st::lineWidth));
			p.setBrush(st::imageBg);
			p.drawRoundedRect(
				QRectF(
					0.5,
					0.5,
					_fpsCounter->width() - 1.,
					_fpsCounter->height() - 1.),
				st::roundRadiusSmall,
				st::roundRadiusSmall);
		}
		p.setPen(st::dialogsOnlineBadgeFg);
		p.setFont(tooltip.textStyle.font);
		p.drawText(
			QRect(
				st::lineWidth + tooltip.textPadding.left(),
				st::lineWidth + tooltip.textPadding.top(),
				_fpsCounter->width()
					- (2 * st::lineWidth)
					- tooltip.textPadding.left()
					- tooltip.textPadding.right(),
				_fpsCounter->height()
					- (2 * st::lineWidth)
					- tooltip.textPadding.top()
					- tooltip.textPadding.bottom()),
			Qt::AlignCenter,
			_fpsCounterText);
	}, _fpsCounter->lifetime());
	updateFpsCounterGeometry();
}

void MainWindow::pruneFpsFrames(crl::time now) {
	const auto till = now - kFpsCounterMeasureWindow;
	while (!_fpsFrames.empty() && (_fpsFrames.front() <= till)) {
		_fpsFrames.pop_front();
	}
}

void MainWindow::recordFpsFrame() {
	if (!_fpsCounterVisible) {
		return;
	}
	const auto now = crl::now();
	_fpsFrames.push_back(now);
	pruneFpsFrames(now);
}

void MainWindow::refreshFpsCounter() {
	if (!_fpsCounterVisible || !_fpsCounter) {
		return;
	}
	const auto now = crl::now();
	pruneFpsFrames(now);
	const auto elapsed = std::min(
		kFpsCounterMeasureWindow,
		now - _fpsCounterShownAt);
	const auto fps = (elapsed > 0)
		? ((int(_fpsFrames.size()) * 1000) + (elapsed / 2)) / elapsed
		: 0;
	const auto displayed = (fps > 999) ? 999 : int(fps);
	const auto text = QString::number(displayed) + u" FPS"_q;
	if (_fpsCounterText != text) {
		_fpsCounterText = text;
		_fpsCounter->update();
	}
}

void MainWindow::toggleFpsCounter() {
	ensureFpsCounterCreated();
	if (!_fpsCounter) {
		return;
	}

	_fpsCounterVisible = !_fpsCounterVisible;
	_fpsFrameQueued = false;
	_fpsFrames.clear();
	if (!_fpsCounterVisible) {
		_fpsCounterTimer.cancel();
		_fpsCounter->hide();
		return;
	}

	_fpsCounterShownAt = crl::now();
	_fpsCounterText.clear();
	updateFpsCounterGeometry();
	_fpsCounter->show();
	refreshFpsCounter();
	fixOrder();
	_fpsCounterTimer.callEach(kFpsCounterRefreshInterval);
}

void MainWindow::updateFpsCounterGeometry() {
	if (!_fpsCounter) {
		return;
	}
	const auto &tooltip = st::defaultTooltip;
	_fpsCounter->resize(
		(2 * st::lineWidth)
			+ tooltip.textPadding.left()
			+ tooltip.textPadding.right()
			+ tooltip.textStyle.font->width(u"999 FPS"_q),
		(2 * st::lineWidth)
			+ tooltip.textPadding.top()
			+ tooltip.textPadding.bottom()
			+ tooltip.textStyle.font->height);
	_fpsCounter->moveToLeft(
		tooltip.skip,
		tooltip.skip);
}

QPixmap MainWindow::grabForSlideAnimation() {
	const auto body = bodyWidget();
	return body->size().isEmpty() ? QPixmap() : Ui::GrabWidget(body);
}

void MainWindow::preventOrInvoke(Fn<void()> callback) {
	if (_main && _main->preventsCloseSection(callback)) {
		return;
	}
	callback();
}

void MainWindow::setupPasscodeLock() {
	auto animated = (_main || _intro);
	auto oldContentCache = animated ? grabForSlideAnimation() : QPixmap();
	_passcodeLock.create(bodyWidget(), &controller());
	updateControlsGeometry();

	ui_hideSettingsAndLayer(anim::type::instant);
	if (_main) {
		_main->hide();
	}
	if (_intro) {
		_intro->hide();
	}
	if (animated) {
		_passcodeLock->showAnimated(std::move(oldContentCache));
	} else {
		_passcodeLock->showFinished();
		setInnerFocus();
	}
	if (const auto sessionController = controller().sessionController()) {
		sessionController->session().attachWebView().closeAll();
	}
}

void MainWindow::setupSetupEmailLock() {
	auto animated = (_main || _intro || _passcodeLock);
	auto oldContentCache = animated ? grabForSlideAnimation() : QPixmap();
	_setupEmailLock.create(bodyWidget(), &controller());
	updateControlsGeometry();

	ui_hideSettingsAndLayer(anim::type::instant);
	if (_main) {
		_main->hide();
	}
	if (_intro) {
		_intro->hide();
	}
	if (_passcodeLock) {
		_passcodeLock->hide();
	}
	if (animated) {
		_setupEmailLock->showAnimated(std::move(oldContentCache));
	} else {
		_setupEmailLock->showFinished();
		setInnerFocus();
	}
	if (const auto sessionController = controller().sessionController()) {
		sessionController->session().attachWebView().closeAll();
	}
}

void MainWindow::clearSetupEmailLock() {
	if (!_setupEmailLock) {
		return;
	}

	auto oldContentCache = grabForSlideAnimation();
	_setupEmailLock.destroy();
	if (_passcodeLock) {
		_passcodeLock->show();
		updateControlsGeometry();
		_passcodeLock->showAnimated(std::move(oldContentCache));
	} else if (_intro) {
		_intro->show();
		updateControlsGeometry();
		_intro->showAnimated(std::move(oldContentCache), true);
	} else if (_main) {
		_main->show();
		updateControlsGeometry();
		_main->showAnimated(std::move(oldContentCache), true);
		Core::App().checkStartUrls();
	}
}

void MainWindow::clearPasscodeLock() {
	Expects(_intro || _main);

	if (!_passcodeLock) {
		return;
	}

	auto oldContentCache = grabForSlideAnimation();
	_passcodeLock.destroy();
	if (_setupEmailLock) {
		_setupEmailLock->show();
		updateControlsGeometry();
		_setupEmailLock->showAnimated(std::move(oldContentCache));
	} else if (_intro) {
		_intro->show();
		updateControlsGeometry();
		_intro->showAnimated(std::move(oldContentCache), true);
	} else if (_main) {
		_main->show();
		updateControlsGeometry();
		_main->showAnimated(std::move(oldContentCache), true);
		Core::App().checkStartUrls();
	}
}

void MainWindow::setupIntro(
		Intro::EnterPoint point,
		Main::Account *accountBeforeIntro,
		QPixmap oldContentCache) {
	auto animated = (_main || _passcodeLock || _setupEmailLock);

	destroyLayer();
	auto created = object_ptr<Intro::Widget>(
		bodyWidget(),
		&controller(),
		&account(),
		point,
		accountBeforeIntro);
	created->showSettingsRequested(
	) | rpl::on_next([=] {
		showSettings();
	}, created->lifetime());

	clearWidgets();
	_intro = std::move(created);
	DragArea::SetupProxyDropArea(_intro.data(), [](const QString &localUrl) {
		Core::App().openLocalUrl(localUrl, {});
	});
	if (_passcodeLock || _setupEmailLock) {
		_intro->hide();
	} else {
		_intro->show();
		updateControlsGeometry();
		if (animated) {
			_intro->showAnimated(std::move(oldContentCache));
		} else {
			setInnerFocus();
		}
	}
	fixOrder();
}

void MainWindow::setupMain(
		MsgId singlePeerShowAtMsgId,
		QPixmap oldContentCache) {
	Expects(account().sessionExists());

	const auto animated = _intro
		|| (_passcodeLock && !Core::App().passcodeLocked())
		|| _setupEmailLock;
	const auto weakAnimatedLayer = (_main
			&& _layer
			&& !_passcodeLock
			&& !_setupEmailLock)
		? base::make_weak(_layer.get())
		: nullptr;
	if (weakAnimatedLayer) {
		Assert(!animated);
		_layer->hideAllAnimatedPrepare();
	} else {
		destroyLayer();
	}
	auto created = object_ptr<MainWidget>(bodyWidget(), sessionController());
	clearWidgets();
	_main = std::move(created);
	updateControlsGeometry();
	Ui::SendPendingMoveResizeEvents(_main);
	_main->controller()->showByInitialId(
		Window::SectionShow::Way::ClearStack,
		singlePeerShowAtMsgId);
	if (_passcodeLock || _setupEmailLock) {
		_main->hide();
	} else {
		_main->show();
		updateControlsGeometry();
		if (animated) {
			_main->showAnimated(std::move(oldContentCache));
		} else {
			_main->activate();
		}
		Core::App().checkStartUrls();
	}
	fixOrder();
	if (const auto strong = weakAnimatedLayer.get()) {
		strong->hideAllAnimatedRun();
	}
}

void MainWindow::showSettings() {
	if (_passcodeLock || _setupEmailLock) {
		return;
	}

	if (const auto session = sessionController()) {
		session->showSettings();
	} else {
		showSpecialLayer(
			Box<Settings::LayerWidget>(&controller()),
			anim::type::normal);
	}
}

void MainWindow::showSpecialLayer(
		object_ptr<Ui::LayerWidget> layer,
		anim::type animated) {
	if (_passcodeLock || _setupEmailLock) {
		return;
	}

	if (layer) {
		ensureLayerCreated();
		_layer->showSpecialLayer(std::move(layer), animated);
	} else if (_layer) {
		_layer->hideSpecialLayer(animated);
	}
}

bool MainWindow::showSectionInExistingLayer(
		not_null<Window::SectionMemento*> memento,
		const Window::SectionShow &params) {
	if (_layer) {
		return _layer->showSectionInternal(memento, params);
	}
	return false;
}

void MainWindow::showMainMenu() {
	if (_passcodeLock || _setupEmailLock) return;

	if (isHidden()) showFromTray();

	ensureLayerCreated();
	_layer->showMainMenu(
		object_ptr<Window::MainMenu>(body(), sessionController()),
		anim::type::normal);
}

void MainWindow::ensureLayerCreated() {
	if (_layer) {
		return;
	}
	_layer = base::make_unique_q<Ui::LayerStackWidget>(
		bodyWidget(),
		crl::guard(this, [=] { return controller().uiShow(); }));

	_layer->hideFinishEvents(
	) | rpl::filter([=] {
		return _layer != nullptr; // Last hide finish is sent from destructor.
	}) | rpl::on_next([=] {
		destroyLayer();
	}, _layer->lifetime());

	_layer->boxShownValue(
	) | rpl::on_next([=](bool shown) {
		_boxShown = shown;
	}, _layer->lifetime());

	if (const auto controller = sessionController()) {
		controller->enableGifPauseReason(Window::GifPauseReason::Layer);
	}
}

void MainWindow::destroyLayer() {
	if (!_layer) {
		return;
	}

	auto layer = base::take(_layer);
	_boxShown = false;
	const auto resetFocus = Ui::InFocusChain(layer);
	if (resetFocus) {
		setFocus();
	}
	layer = nullptr;

	if (const auto controller = sessionController()) {
		controller->disableGifPauseReason(Window::GifPauseReason::Layer);
	}
	if (resetFocus) {
		setInnerFocus();
	}
	InvokeQueued(this, [=] {
		checkActivation();
	});
}

void MainWindow::ui_hideSettingsAndLayer(anim::type animated) {
	if (animated == anim::type::instant) {
		destroyLayer();
	} else if (_layer) {
		_layer->hideAll(animated);
	}
}

void MainWindow::ui_removeLayerBlackout() {
	if (_layer) {
		_layer->removeBodyCache();
	}
}

MainWidget *MainWindow::sessionContent() const {
	return _main.data();
}

void MainWindow::showOrHideBoxOrLayer(
		std::variant<
			v::null_t,
			object_ptr<Ui::BoxContent>,
			std::unique_ptr<Ui::LayerWidget>> &&layer,
		Ui::LayerOptions options,
		anim::type animated) {
	using UniqueLayer = std::unique_ptr<Ui::LayerWidget>;
	using ObjectBox = object_ptr<Ui::BoxContent>;
	if (auto layerWidget = std::get_if<UniqueLayer>(&layer)) {
		ensureLayerCreated();
		_layer->showLayer(std::move(*layerWidget), options, animated);
	} else if (auto box = std::get_if<ObjectBox>(&layer)) {
		ensureLayerCreated();
		_layer->showBox(std::move(*box), options, animated);
	} else {
		if (_layer) {
			_layer->hideTopLayer(animated);
			if ((animated == anim::type::instant)
				&& _layer
				&& !_layer->layerShown()) {
				destroyLayer();
			}
		}
		Core::App().hideMediaView();
	}
}

bool MainWindow::ui_isLayerShown() const {
	return _layer != nullptr;
}

rpl::producer<bool> MainWindow::ui_boxShownValue() const {
	return _boxShown.value();
}

bool MainWindow::closeLayerByBackButton() {
	return _layer && _layer->closeCurrentByBackButton();
}

bool MainWindow::showMediaPreview(
		Data::FileOrigin origin,
		not_null<DocumentData*> document) {
	const auto media = document->activeMediaView();
	const auto preview = Data::VideoPreviewState(media.get());
	if (!document->sticker()
		&& (!document->isAnimation() || !preview.loaded())) {
		return false;
	}
	if (!_mediaPreview) {
		_mediaPreview.create(bodyWidget(), sessionController());
		updateControlsGeometry();
	}
	if (_mediaPreview->isHidden()) {
		fixOrder();
	}
	_mediaPreview->showPreview(origin, document);
	return true;
}

bool MainWindow::showMediaPreview(
		Data::FileOrigin origin,
		not_null<PhotoData*> photo) {
	if (!_mediaPreview) {
		_mediaPreview.create(bodyWidget(), sessionController());
		updateControlsGeometry();
	}
	if (_mediaPreview->isHidden()) {
		fixOrder();
	}
	_mediaPreview->showPreview(origin, photo);
	return true;
}

void MainWindow::hideMediaPreview() {
	if (!_mediaPreview) {
		return;
	}
	_mediaPreview->hidePreview();
}

void MainWindow::themeUpdated(const Window::Theme::BackgroundUpdate &data) {
	using Type = Window::Theme::BackgroundUpdate::Type;

	// We delay animating theme warning because we want all other
	// subscribers to receive palette changed notification before any
	// animations (that include pixmap caches with old palette values).
	if (data.type == Type::TestingTheme) {
		if (!_testingThemeWarning) {
			_testingThemeWarning.create(bodyWidget());
			_testingThemeWarning->hide();
			_testingThemeWarning->setGeometry(rect());
			_testingThemeWarning->setHiddenCallback([this] { _testingThemeWarning.destroyDelayed(); });
		}
		crl::on_main(this, [=] {
			if (_testingThemeWarning) {
				_testingThemeWarning->showAnimated();
			}
		});
	} else if (data.type == Type::RevertingTheme || data.type == Type::ApplyingTheme) {
		if (_testingThemeWarning) {
			if (_testingThemeWarning->isHidden()) {
				_testingThemeWarning.destroy();
			} else {
				crl::on_main(this, [=] {
					if (_testingThemeWarning) {
						_testingThemeWarning->hideAnimated();
						_testingThemeWarning = nullptr;
					}
					setInnerFocus();
				});
			}
		}
	}
}

bool MainWindow::markingAsRead() const {
	return _main
		&& !_main->isHidden()
		&& !_main->animatingShow()
		&& !_layer
		&& !isHidden()
		&& !isMinimized()
		&& windowHandle()->isExposed()
		&& (AutoScrollInactiveChat.value()
			|| (isActive() && !_main->session().updates().isIdle()));
}

void MainWindow::checkActivation() {
	updateIsActive();
	if (_main) {
		_main->checkActivation();
	}
}

bool MainWindow::contentOverlapped(const QRect &globalRect) {
	return (_main && _main->contentOverlapped(globalRect))
		|| (_layer && _layer->contentOverlapped(globalRect));
}

void MainWindow::setInnerFocus() {
	if (_testingThemeWarning) {
		_testingThemeWarning->setFocus();
	} else if (_layer && _layer->canSetFocus()) {
		_layer->setInnerFocus();
	} else if (_passcodeLock) {
		_passcodeLock->setInnerFocus();
	} else if (_setupEmailLock) {
		_setupEmailLock->setInnerFocus();
	} else if (_main) {
		_main->setInnerFocus();
	} else if (_intro) {
		_intro->setInnerFocus();
	}
}

bool MainWindow::eventFilter(QObject *object, QEvent *e) {
	switch (e->type()) {
	case QEvent::ShortcutOverride: {
		if (matchesFpsCounterKey(object, static_cast<QKeyEvent*>(e))) {
			e->accept();
			return true;
		}
	} break;

	case QEvent::KeyPress: {
		if (matchesFpsCounterKey(object, static_cast<QKeyEvent*>(e))) {
			toggleFpsCounter();
			return true;
		}
		if (Logs::DebugEnabled()
			&& object == windowHandle()) {
			const auto key = static_cast<QKeyEvent*>(e)->key();
			FeedLangTestingKey(key);
		}
#ifdef _DEBUG
		if (static_cast<QKeyEvent*>(e)->modifiers().testFlag(
				Qt::ControlModifier)) {
			switch (static_cast<QKeyEvent*>(e)->key()) {
			case Qt::Key_F11:
				anim::SetSlowMultiplier((anim::SlowMultiplier() == 10)
					? 1
					: 10);
				return true;
			case Qt::Key_F12:
				anim::SetSlowMultiplier((anim::SlowMultiplier() == 50)
					? 1
					: 50);
				return true;
			}
		}
#endif
	} break;

	case QEvent::UpdateRequest:
	case QEvent::Paint: {
		if (countsForFpsCounter(object) && !_fpsFrameQueued) {
			_fpsFrameQueued = true;
			recordFpsFrame();
			InvokeQueued(this, [=] {
				_fpsFrameQueued = false;
			});
		}
	} break;

	case QEvent::MouseMove: {
		const auto position = static_cast<QMouseEvent*>(e)->globalPos();
		if (_lastMousePosition != position) {
			if (const auto controller = sessionController()) {
				if (controller->session().updates().isIdle()) {
					Core::App().updateNonIdle();
				}
			}
		}
		_lastMousePosition = position;
	} break;

	case QEvent::MouseButtonRelease: {
		hideMediaPreview();
	} break;

	case QEvent::ApplicationActivate: {
		if (object == QCoreApplication::instance()) {
			InvokeQueued(this, [=] {
				handleActiveChanged(isActiveWindow());
			});
		}
	} break;

	case QEvent::WindowStateChange: {
		if (object == this) {
			auto state = (windowState() & Qt::WindowMinimized) ? Qt::WindowMinimized :
				((windowState() & Qt::WindowMaximized) ? Qt::WindowMaximized :
				((windowState() & Qt::WindowFullScreen) ? Qt::WindowFullScreen : Qt::WindowNoState));
			handleStateChanged(state);
		}
	} break;

	case QEvent::Move:
	case QEvent::Resize: {
		if (object == this) {
			positionUpdated();
		}
	} break;
	}

	return Platform::MainWindow::eventFilter(object, e);
}

bool MainWindow::takeThirdSectionFromLayer() {
	return _layer ? _layer->takeToThirdSection() : false;
}

void MainWindow::fixOrder() {
	if (_setupEmailLock) _setupEmailLock->raise();
	if (_passcodeLock) _passcodeLock->raise();
	if (_layer) _layer->raise();
	if (_mediaPreview) _mediaPreview->raise();
	if (_testingThemeWarning) _testingThemeWarning->raise();
	if (_fpsCounter) _fpsCounter->raise();
}

void MainWindow::closeEvent(QCloseEvent *e) {
	if (Core::Sandbox::Instance().isSavingSession() || Core::Quitting()) {
		e->accept();
		Core::Quit();
		return;
	} else if (Core::App().closeNonLastAsync(&controller())) {
		e->accept();
		return;
	}
	e->ignore();
	const auto hasAuth = [&] {
		if (!Core::App().domain().started()) {
			return false;
		}
		for (const auto &[_, account] : Core::App().domain().accounts()) {
			if (account->sessionExists()) {
				return true;
			}
		}
		return false;
	}();
	if (!hasAuth || !hideNoQuit()) {
		Core::Quit();
	}
}

void MainWindow::updateControlsGeometry() {
	Platform::MainWindow::updateControlsGeometry();

	auto body = bodyWidget()->rect();
	if (_passcodeLock) _passcodeLock->setGeometry(body);
	if (_setupEmailLock) _setupEmailLock->setGeometry(body);
	auto mainLeft = 0;
	auto mainWidth = body.width();
	if (const auto session = sessionController()) {
		if (const auto skip = session->filtersWidth()) {
			mainLeft += skip;
			mainWidth -= skip;
		}
	}
	if (_main) {
		_main->setGeometry({
			body.x() + mainLeft,
			body.y(),
			mainWidth,
			body.height() });
	}
	if (_intro) _intro->setGeometry(body);
	if (_layer) _layer->setGeometry(body);
	if (_mediaPreview) _mediaPreview->setGeometry(body);
	if (_testingThemeWarning) _testingThemeWarning->setGeometry(body);
	if (_fpsCounter) updateFpsCounterGeometry();

	if (_main) _main->checkMainSectionToLayer();
}

void MainWindow::handleStartFiles(
		QStringList interprets,
		QStringList paths) {
	if (controller().locked()) {
		return;
	}
	Core::App().hideMediaView();
	ui_hideSettingsAndLayer(anim::type::instant);
	if (_main) {
		_main->handleStartFiles(
			std::move(interprets),
			std::move(paths));
	}
}

MainWindow::~MainWindow() = default;
