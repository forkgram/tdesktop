use eframe::egui::{self, Color32, RichText};
use grammers_client::{client::DialogIter, update::Update, Client, SignInError};
use grammers_mtsender::{SenderPool, UpdatesConfiguration};
use grammers_session::{storages::SqliteSession, types::PeerRef, updates::UpdatesLike};
use rustgram::{data_dir, private_session, Result};
use std::{
    collections::HashMap,
    env,
    sync::{mpsc, Arc},
    thread,
};
use tokio::sync::mpsc as async_mpsc;

const PAGE_SIZE: usize = 50;

#[derive(Clone, Debug)]
struct Chat {
    id: i64,
    title: String,
}
#[derive(Clone, Debug)]
struct Message {
    id: i32,
    sender: String,
    text: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct HistoryRequest {
    chat_id: i64,
    serial: u64,
    before: Option<i32>,
}

enum Request {
    Connect(String),
    Code(String),
    Password(String),
    MoreChats,
    History(HistoryRequest),
    Send(i64, String),
}
enum Event {
    Code,
    Password,
    Chats(Vec<Chat>, bool),
    MoreChats(Vec<Chat>, bool),
    History(HistoryRequest, Vec<Message>),
    MessagesChanged(Option<i64>),
    UpdatesStopped(String),
    Sent(i64),
    SendFailed(i64),
    Error(String),
    Fatal(String),
}

fn send_event(tx: &mpsc::Sender<Event>, ctx: &egui::Context, event: Event) {
    if tx.send(event).is_ok() {
        ctx.request_repaint();
    }
}

async fn next_chat_page(
    iter: &mut DialogIter,
    peers: &mut HashMap<i64, PeerRef>,
) -> Result<(Vec<Chat>, bool)> {
    let mut page = Vec::new();
    while page.len() < PAGE_SIZE {
        let Some(dialog) = iter.next().await? else {
            return Ok((page, false));
        };
        let peer = dialog.peer();
        if let Some(id) = peer.id().bot_api_dialog_id() {
            if let std::collections::hash_map::Entry::Vacant(entry) = peers.entry(id) {
                if let Some(reference) = peer.to_ref().await? {
                    entry.insert(reference);
                    page.push(Chat {
                        id,
                        title: peer.name().unwrap_or("Без названия").to_owned(),
                    });
                }
            }
        }
    }
    // If the final page has exactly PAGE_SIZE items, one more request is needed
    // to distinguish that case from an account with additional dialogs.
    Ok((page, true))
}

fn start_worker(
    ctx: egui::Context,
) -> (async_mpsc::UnboundedSender<Request>, mpsc::Receiver<Event>) {
    let (request_tx, mut request_rx) = async_mpsc::unbounded_channel();
    let (event_tx, event_rx) = mpsc::channel();
    thread::spawn(move || {
        let runtime = match tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
        {
            Ok(runtime) => runtime,
            Err(e) => {
                send_event(&event_tx, &ctx, Event::Fatal(format!("Runtime: {e}")));
                return;
            }
        };
        runtime.block_on(async {
            while let Some(request) = request_rx.recv().await {
                if let Request::Connect(phone) = request {
                    if let Err(e) = connected(phone, &mut request_rx, &event_tx, &ctx).await {
                        send_event(&event_tx, &ctx, Event::Fatal(e.to_string()));
                    }
                }
            }
        });
    });
    (request_tx, event_rx)
}

async fn connected(
    phone: String,
    rx: &mut async_mpsc::UnboundedReceiver<Request>,
    tx: &mpsc::Sender<Event>,
    ctx: &egui::Context,
) -> Result<()> {
    let api_id: i32 = env::var("RUSTGRAM_API_ID")
        .map_err(|_| "Задайте RUSTGRAM_API_ID")?
        .parse()?;
    let api_hash = env::var("RUSTGRAM_API_HASH").map_err(|_| "Задайте RUSTGRAM_API_HASH")?;
    if api_id <= 0 || api_hash.trim().is_empty() {
        return Err("Неверные данные Telegram API".into());
    }
    let session = Arc::new(SqliteSession::open(private_session(&data_dir()?)?).await?);
    let SenderPool {
        runner,
        handle,
        updates,
    } = SenderPool::new(session, api_id);
    let client = Client::new(handle);
    let task = tokio::spawn(runner.run());
    let result = connected_inner(&client, phone, &api_hash, updates, rx, tx, ctx).await;
    task.abort();
    result
}

async fn connected_inner(
    client: &Client,
    phone: String,
    api_hash: &str,
    updates: async_mpsc::Receiver<UpdatesLike>,
    rx: &mut async_mpsc::UnboundedReceiver<Request>,
    tx: &mpsc::Sender<Event>,
    ctx: &egui::Context,
) -> Result<()> {
    if !client.is_authorized().await? {
        if phone.trim().is_empty() {
            return Err("Введите номер телефона с кодом страны".into());
        }
        let token = client.request_login_code(phone.trim(), api_hash).await?;
        send_event(tx, ctx, Event::Code);
        loop {
            let Some(request) = rx.recv().await else {
                return Ok(());
            };
            let Request::Code(code) = request else {
                continue;
            };
            match client.sign_in(&token, code.trim()).await {
                Ok(_) => break,
                Err(SignInError::InvalidCode) => {
                    send_event(
                        tx,
                        ctx,
                        Event::Error("Неверный код. Попробуйте ещё раз.".into()),
                    );
                    send_event(tx, ctx, Event::Code);
                }
                Err(SignInError::PasswordRequired(mut password_token)) => {
                    send_event(tx, ctx, Event::Password);
                    loop {
                        let Some(request) = rx.recv().await else {
                            return Ok(());
                        };
                        let Request::Password(password) = request else {
                            continue;
                        };
                        match client.check_password(password_token, &password).await {
                            Ok(_) => break,
                            Err(SignInError::InvalidPassword(next)) => {
                                password_token = next;
                                send_event(
                                    tx,
                                    ctx,
                                    Event::Error("Неверный пароль. Попробуйте ещё раз.".into()),
                                );
                                send_event(tx, ctx, Event::Password);
                            }
                            Err(e) => return Err(e.into()),
                        }
                    }
                    break;
                }
                Err(e) => return Err(e.into()),
            }
        }
    }
    let mut iter = client.iter_dialogs();
    let mut peers = HashMap::new();
    let (chats, has_more) = next_chat_page(&mut iter, &mut peers).await?;
    let mut stream = client
        .stream_updates(updates, UpdatesConfiguration { catch_up: true })
        .await?;
    let event_tx = tx.clone();
    let repaint = ctx.clone();
    // The update stream must be consumed while a history request is in flight.
    // Aborting this guard on disconnect also drops the stream and its client handle.
    let _updates_task = AbortOnDrop(tokio::spawn(async move {
        loop {
            let update = match stream.next().await {
                Ok(update) => update,
                Err(error) => {
                    send_event(
                        &event_tx,
                        &repaint,
                        Event::UpdatesStopped(error.to_string()),
                    );
                    break;
                }
            };
            let changed = match update {
                Update::NewMessage(message) | Update::MessageEdited(message) => {
                    message.peer_id().bot_api_dialog_id().map(Some)
                }
                // Non-channel deletion updates do not identify the chat.
                Update::MessageDeleted(_) => Some(None),
                _ => None,
            };
            if let Some(chat) = changed {
                send_event(&event_tx, &repaint, Event::MessagesChanged(chat));
                if let Err(error) = stream.sync_update_state().await {
                    send_event(
                        &event_tx,
                        &repaint,
                        Event::UpdatesStopped(error.to_string()),
                    );
                    break;
                }
            }
        }
    }));
    send_event(tx, ctx, Event::Chats(chats, has_more));
    while let Some(request) = rx.recv().await {
        match request {
            Request::MoreChats => {
                let (page, has_more) = next_chat_page(&mut iter, &mut peers).await?;
                send_event(tx, ctx, Event::MoreChats(page, has_more));
            }
            Request::History(history) => {
                let Some(reference) = peers.get(&history.chat_id) else {
                    continue;
                };
                let mut iter = client.iter_messages(*reference).limit(PAGE_SIZE);
                if let Some(before) = history.before {
                    iter = iter.offset_id(before);
                }
                let mut messages = Vec::new();
                while messages.len() < PAGE_SIZE {
                    let Some(message) = iter.next().await? else {
                        break;
                    };
                    messages.push(Message {
                        id: message.id(),
                        sender: message
                            .sender()
                            .and_then(|peer| peer.name())
                            .unwrap_or("Сервис")
                            .to_owned(),
                        text: if message.text().is_empty() {
                            "[Медиа или служебное сообщение]".into()
                        } else {
                            message.text().to_owned()
                        },
                    });
                }
                messages.reverse();
                send_event(tx, ctx, Event::History(history, messages));
            }
            Request::Send(id, text) => {
                let peer = peers.get(&id).copied();
                let succeeded = match peer {
                    Some(peer) => client.send_message(peer, text.as_str()).await.is_ok(),
                    None => false,
                };
                send_event(
                    tx,
                    ctx,
                    if succeeded {
                        Event::Sent(id)
                    } else {
                        Event::SendFailed(id)
                    },
                );
            }
            _ => (),
        }
    }
    Ok(())
}

struct AbortOnDrop(tokio::task::JoinHandle<()>);
impl Drop for AbortOnDrop {
    fn drop(&mut self) {
        self.0.abort();
    }
}

#[derive(PartialEq, Eq)]
enum Screen {
    Connect,
    Busy,
    Code,
    Password,
    Chats,
}
struct App {
    tx: async_mpsc::UnboundedSender<Request>,
    rx: mpsc::Receiver<Event>,
    screen: Screen,
    phone: String,
    secret: String,
    error: Option<String>,
    chats: Vec<Chat>,
    chats_has_more: bool,
    chats_loading: bool,
    selected: Option<i64>,
    messages: Vec<Message>,
    drafts: HashMap<i64, String>,
    pending: HashMap<i64, String>,
    send_errors: HashMap<i64, String>,
    loading: bool,
    history_dirty: bool,
    history_latest: bool,
    history_changed: bool,
    history_has_more: bool,
    history_serial: u64,
}
impl App {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        let mut style = (*cc.egui_ctx.style_of(egui::Theme::Light)).clone();
        style.visuals = egui::Visuals::light();
        style.visuals.selection.bg_fill = Color32::from_rgb(62, 140, 200);
        cc.egui_ctx.set_style_of(egui::Theme::Light, style);
        cc.egui_ctx.set_theme(egui::Theme::Light);
        let (tx, rx) = start_worker(cc.egui_ctx.clone());
        Self {
            tx,
            rx,
            screen: Screen::Connect,
            phone: String::new(),
            secret: String::new(),
            error: None,
            chats: vec![],
            chats_has_more: false,
            chats_loading: false,
            selected: None,
            messages: vec![],
            drafts: HashMap::new(),
            pending: HashMap::new(),
            send_errors: HashMap::new(),
            loading: false,
            history_dirty: false,
            history_latest: true,
            history_changed: false,
            history_has_more: false,
            history_serial: 0,
        }
    }
    fn send(&mut self, request: Request) {
        if self.tx.send(request).is_err() {
            self.error = Some("Фоновое соединение завершилось".into());
            self.screen = Screen::Connect;
        }
    }
    fn send_draft(&mut self, id: i64) {
        if self.pending.contains_key(&id) {
            return;
        }
        let Some(text) = self
            .drafts
            .get(&id)
            .filter(|text| !text.trim().is_empty())
            .cloned()
        else {
            return;
        };
        if self.tx.send(Request::Send(id, text.clone())).is_err() {
            self.send_errors
                .insert(id, "Соединение завершилось. Черновик сохранён.".into());
            self.error = Some("Фоновое соединение завершилось. Подключитесь снова.".into());
            self.screen = Screen::Connect;
            return;
        }
        self.pending.insert(id, text);
        self.send_errors.remove(&id);
    }
    fn refresh_selected(&mut self) {
        if self.selected.is_none() {
            return;
        }
        if !self.history_latest {
            self.history_changed = true;
            return;
        }
        if self.loading {
            self.history_dirty = true;
        } else {
            self.load_history(None);
        }
    }
    fn load_history(&mut self, before: Option<i32>) {
        let Some(chat_id) = self.selected else {
            return;
        };
        self.history_serial = self.history_serial.wrapping_add(1);
        self.history_latest = before.is_none();
        self.history_dirty = false;
        self.loading = true;
        self.send(Request::History(HistoryRequest {
            chat_id,
            serial: self.history_serial,
            before,
        }));
    }
    fn load_more_chats(&mut self) {
        if self.chats_has_more && !self.chats_loading {
            self.chats_loading = true;
            self.send(Request::MoreChats);
        }
    }
    fn receive(&mut self) {
        while let Ok(event) = self.rx.try_recv() {
            match event {
                Event::Code => self.screen = Screen::Code,
                Event::Password => self.screen = Screen::Password,
                Event::Chats(chats, has_more) => {
                    self.chats = chats;
                    self.chats_has_more = has_more;
                    self.chats_loading = false;
                    self.screen = Screen::Chats;
                    self.error = None;
                }
                Event::MoreChats(chats, has_more) => {
                    self.chats.extend(chats);
                    self.chats_has_more = has_more;
                    self.chats_loading = false;
                }
                Event::History(request, messages)
                    if self.selected == Some(request.chat_id)
                        && self.history_serial == request.serial =>
                {
                    self.history_has_more = messages.len() == PAGE_SIZE;
                    // An empty older page leaves the current page on screen.
                    if request.before.is_none() || !messages.is_empty() {
                        self.messages = messages;
                    }
                    if request.before.is_none() {
                        self.history_changed = false;
                    }
                    self.loading = false;
                    if std::mem::take(&mut self.history_dirty) {
                        self.refresh_selected();
                    }
                }
                Event::History(_, _) => (),
                Event::MessagesChanged(chat)
                    if self.selected.is_some() && (chat.is_none() || chat == self.selected) =>
                {
                    self.refresh_selected();
                }
                Event::MessagesChanged(_) => (),
                Event::UpdatesStopped(error) => {
                    self.error = Some(format!(
                        "Автообновление остановлено: {error}. Обновите чат вручную."
                    ));
                }
                Event::Sent(id) => {
                    if let Some(sent) = self.pending.remove(&id) {
                        if self.drafts.get(&id) == Some(&sent) {
                            self.drafts.remove(&id);
                        }
                    }
                    self.send_errors.remove(&id);
                    if self.selected == Some(id) {
                        self.refresh_selected();
                    }
                }
                Event::SendFailed(id) => {
                    self.pending.remove(&id);
                    self.send_errors.insert(id, "Не удалось подтвердить отправку. Проверьте чат перед повтором; черновик сохранён.".into());
                }
                Event::Error(error) => {
                    self.error = Some(error);
                    self.loading = false;
                }
                Event::Fatal(error) => {
                    self.error = Some(error);
                    self.screen = Screen::Connect;
                    for (id, _) in self.pending.drain() {
                        self.send_errors.insert(id, "Соединение прервано до подтверждения. Проверьте чат перед повтором; черновик сохранён.".into());
                    }
                    self.chats.clear();
                    self.chats_has_more = false;
                    self.chats_loading = false;
                    self.messages.clear();
                    self.selected = None;
                    self.loading = false;
                    self.history_dirty = false;
                    self.history_latest = true;
                    self.history_changed = false;
                    self.history_has_more = false;
                }
            }
        }
    }
}
impl eframe::App for App {
    fn logic(&mut self, _: &egui::Context, _: &mut eframe::Frame) {
        self.receive();
    }
    fn ui(&mut self, ui: &mut egui::Ui, _: &mut eframe::Frame) {
        if self.screen == Screen::Chats {
            egui::Panel::left("chats")
                .resizable(true)
                .default_size(260.0)
                .show(ui, |ui| {
                    ui.add_space(12.0);
                    ui.heading("rustGram");
                    ui.label(RichText::new("Чаты").weak());
                    ui.separator();
                    egui::ScrollArea::vertical().show(ui, |ui| {
                        let mut select = None;
                        for chat in &self.chats {
                            if ui
                                .selectable_label(self.selected == Some(chat.id), &chat.title)
                                .clicked()
                            {
                                select = Some(chat.id);
                            }
                        }
                        if let Some(id) = select {
                            self.selected = Some(id);
                            self.messages.clear();
                            self.history_changed = false;
                            self.history_has_more = false;
                            self.load_history(None);
                        }
                        if self.chats_has_more
                            && ui
                                .add_enabled(
                                    !self.chats_loading,
                                    egui::Button::new(if self.chats_loading {
                                        "Загрузка…"
                                    } else {
                                        "Показать ещё чаты"
                                    }),
                                )
                                .clicked()
                        {
                            self.load_more_chats();
                        }
                    });
                });
            if let Some(id) = self.selected {
                egui::Panel::bottom("compose")
                    .resizable(false)
                    .show(ui, |ui| {
                        if let Some(error) = self.send_errors.get(&id) {
                            ui.colored_label(Color32::DARK_RED, error);
                        }
                        let pending = self.pending.contains_key(&id);
                        let send = {
                            let draft = self.drafts.entry(id).or_default();
                            ui.add_enabled(
                                !pending,
                                egui::TextEdit::multiline(draft)
                                    .desired_rows(2)
                                    .desired_width(f32::INFINITY)
                                    .hint_text("Напишите сообщение"),
                            );
                            ui.add_enabled(
                                !pending && !draft.trim().is_empty(),
                                egui::Button::new(if pending {
                                    "Отправка…"
                                } else {
                                    "Отправить"
                                }),
                            )
                            .clicked()
                        };
                        if send {
                            self.send_draft(id);
                        }
                    });
            }
            egui::CentralPanel::default().show(ui, |ui| {
                if let Some(id) = self.selected {
                    let title = self
                        .chats
                        .iter()
                        .find(|c| c.id == id)
                        .map(|c| c.title.clone())
                        .unwrap_or_else(|| "Чат".into());
                    ui.horizontal(|ui| {
                        ui.heading(&title);
                        let label = if self.history_latest {
                            "Обновить"
                        } else {
                            "К последним"
                        };
                        if ui
                            .add_enabled(!self.loading, egui::Button::new(label))
                            .clicked()
                        {
                            self.load_history(None);
                        }
                    });
                    ui.separator();
                    if !self.history_latest && self.history_changed {
                        ui.label("Есть новые изменения. Вернитесь к последним сообщениям.");
                    }
                    if self.loading {
                        ui.spinner();
                        ui.label("Загрузка сообщений…");
                    } else {
                        let older = self.messages.first().map(|message| message.id);
                        if self.history_has_more && ui.button("Более ранние сообщения").clicked()
                        {
                            if let Some(before) = older {
                                self.load_history(Some(before));
                            }
                        }
                        egui::ScrollArea::vertical()
                            .stick_to_bottom(true)
                            .show(ui, |ui| {
                                for message in &self.messages {
                                    ui.add_space(8.0);
                                    ui.label(RichText::new(&message.sender).strong());
                                    ui.label(&message.text);
                                }
                            });
                    }
                } else {
                    ui.centered_and_justified(|ui| {
                        ui.label("Выберите чат слева");
                    });
                }
                if let Some(error) = &self.error {
                    ui.colored_label(Color32::DARK_RED, error);
                }
            });
        } else {
            egui::CentralPanel::default().show(ui, |ui| {
                ui.add_space(48.0);
                ui.vertical_centered(|ui| {
                    ui.heading(RichText::new("rustGram").size(28.0));
                    ui.add_space(8.0); ui.label("Вход в отдельную сессию Telegram"); ui.add_space(24.0);
                    ui.set_max_width(360.0);
                    match self.screen {
                        Screen::Connect => {
                            ui.label("Номер телефона с кодом страны"); ui.add(egui::TextEdit::singleline(&mut self.phone).hint_text("+7 …"));
                            ui.add_space(12.0);
                            if ui.button("Подключиться").clicked() { if self.tx.is_closed() { let (tx, rx) = start_worker(ui.ctx().clone()); self.tx = tx; self.rx = rx; } self.error = None; self.screen = Screen::Busy; self.send(Request::Connect(self.phone.clone())); }
                            ui.add_space(12.0); ui.label(RichText::new("Для существующей сессии номер можно не указывать. Нужны RUSTGRAM_API_ID и RUSTGRAM_API_HASH.").weak().small());
                        }
                        Screen::Busy => { ui.spinner(); ui.label("Подключение…"); }
                        Screen::Code | Screen::Password => {
                            let code = self.screen == Screen::Code;
                            ui.label(if code { "Код из Telegram" } else { "Пароль двухэтапной проверки" });
                            ui.add(egui::TextEdit::singleline(&mut self.secret).password(true)); ui.add_space(12.0);
                            if ui.button("Продолжить").clicked() { let secret = std::mem::take(&mut self.secret); self.error = None; self.screen = Screen::Busy; self.send(if code { Request::Code(secret) } else { Request::Password(secret) }); }
                        }
                        Screen::Chats => (),
                    }
                    if let Some(error) = &self.error { ui.add_space(16.0); ui.colored_label(Color32::DARK_RED, error); }
                });
            });
        }
    }
}
fn main() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([960.0, 640.0])
            .with_min_inner_size([640.0, 420.0]),
        ..Default::default()
    };
    eframe::run_native(
        "rustGram",
        options,
        Box::new(|cc| Ok(Box::new(App::new(cc)))),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn app() -> (
        App,
        mpsc::Sender<Event>,
        async_mpsc::UnboundedReceiver<Request>,
    ) {
        let (tx, requests) = async_mpsc::unbounded_channel();
        let (events, rx) = mpsc::channel();
        (
            App {
                tx,
                rx,
                screen: Screen::Chats,
                phone: String::new(),
                secret: String::new(),
                error: None,
                chats_has_more: false,
                chats_loading: false,
                chats: vec![Chat {
                    id: 1,
                    title: "first".into(),
                }],
                selected: Some(1),
                messages: vec![],
                drafts: HashMap::new(),
                pending: HashMap::new(),
                send_errors: HashMap::new(),
                loading: true,
                history_dirty: false,
                history_latest: true,
                history_changed: false,
                history_has_more: false,
                history_serial: 1,
            },
            events,
            requests,
        )
    }
    #[test]
    fn stale_history_does_not_replace_selected_chat() {
        let (mut app, events, _requests) = app();
        events
            .send(Event::History(
                HistoryRequest {
                    chat_id: 2,
                    serial: 1,
                    before: None,
                },
                vec![Message {
                    id: 10,
                    sender: "x".into(),
                    text: "wrong chat".into(),
                }],
            ))
            .unwrap();
        app.receive();
        assert!(app.messages.is_empty());
        assert!(app.loading);
        events
            .send(Event::History(
                HistoryRequest {
                    chat_id: 1,
                    serial: 1,
                    before: None,
                },
                vec![Message {
                    id: 10,
                    sender: "y".into(),
                    text: "right chat".into(),
                }],
            ))
            .unwrap();
        app.receive();
        assert_eq!(app.messages[0].text, "right chat");
        assert!(!app.loading);
    }
    #[test]
    fn fatal_error_resets_disconnected_view() {
        let (mut app, events, _requests) = app();
        events.send(Event::Fatal("disconnected".into())).unwrap();
        app.receive();
        assert!(app.screen == Screen::Connect);
        assert!(app.chats.is_empty());
        assert_eq!(app.error.as_deref(), Some("disconnected"));
    }
    #[test]
    fn successful_send_clears_only_the_matching_draft_and_refreshes_selected_chat() {
        let (mut app, events, mut requests) = app();
        app.drafts.insert(1, "hello".into());
        app.drafts.insert(2, "other".into());
        app.send_draft(1);
        assert!(matches!(requests.try_recv(), Ok(Request::Send(1, text)) if text == "hello"));
        app.send_draft(1);
        assert!(requests.try_recv().is_err());
        assert_eq!(app.drafts.get(&1).map(String::as_str), Some("hello"));
        events.send(Event::Sent(1)).unwrap();
        app.receive();
        assert!(!app.drafts.contains_key(&1));
        assert_eq!(app.drafts.get(&2).map(String::as_str), Some("other"));
        // A previous history request is already in flight: queue a single
        // follow-up rather than fetching twice concurrently.
        assert!(app.history_dirty);
        assert!(requests.try_recv().is_err());
        events
            .send(Event::History(
                HistoryRequest {
                    chat_id: 1,
                    serial: 1,
                    before: None,
                },
                vec![],
            ))
            .unwrap();
        app.receive();
        assert!(matches!(
            requests.try_recv(),
            Ok(Request::History(HistoryRequest { chat_id: 1, .. }))
        ));
    }

    #[test]
    fn failed_send_preserves_draft_and_does_not_retry_in_ui() {
        let (mut app, events, mut requests) = app();
        app.drafts.insert(1, "do not lose".into());
        app.send_draft(1);
        assert!(matches!(requests.try_recv(), Ok(Request::Send(1, _))));
        app.selected = Some(2);
        events.send(Event::SendFailed(1)).unwrap();
        app.receive();
        assert_eq!(app.drafts.get(&1).map(String::as_str), Some("do not lose"));
        assert!(app.send_errors.contains_key(&1));
        assert!(!app.pending.contains_key(&1));
        assert!(requests.try_recv().is_err());
    }

    #[test]
    fn modified_draft_is_not_erased_after_confirmation() {
        let (mut app, events, mut requests) = app();
        app.drafts.insert(1, "first".into());
        app.send_draft(1);
        let _ = requests.try_recv();
        app.drafts.insert(1, "new draft".into());
        events.send(Event::Sent(1)).unwrap();
        app.receive();
        assert_eq!(app.drafts.get(&1).map(String::as_str), Some("new draft"));
    }
    #[test]
    fn disconnect_during_send_keeps_draft_and_warns_about_uncertain_delivery() {
        let (mut app, events, mut requests) = app();
        app.drafts.insert(1, "message".into());
        app.send_draft(1);
        let _ = requests.try_recv();
        events
            .send(Event::Fatal("connection closed".into()))
            .unwrap();
        app.receive();
        assert_eq!(app.drafts.get(&1).map(String::as_str), Some("message"));
        assert!(app.pending.is_empty());
        assert!(app.send_errors.get(&1).unwrap().contains("подтверждения"));
        assert!(requests.try_recv().is_err());
    }

    #[test]
    fn empty_draft_cannot_be_submitted() {
        let (mut app, _events, mut requests) = app();
        app.drafts.insert(1, " \n ".into());
        app.send_draft(1);
        assert!(requests.try_recv().is_err());
        assert!(app.pending.is_empty());
    }
    #[test]
    fn closed_worker_keeps_draft_and_allows_reconnection() {
        let (mut app, _events, requests) = app();
        drop(requests);
        app.drafts.insert(1, "unsent".into());
        app.send_draft(1);
        assert_eq!(app.drafts.get(&1).map(String::as_str), Some("unsent"));
        assert!(app.screen == Screen::Connect);
        assert!(app.error.is_some());
        assert!(app.pending.is_empty());
    }
    #[test]
    fn updates_during_loading_coalesce_into_one_follow_up() {
        let (mut app, events, mut requests) = app();
        for _ in 0..10 {
            events.send(Event::MessagesChanged(Some(1))).unwrap();
        }
        app.receive();
        assert!(app.history_dirty);
        assert!(requests.try_recv().is_err());
        events
            .send(Event::History(
                HistoryRequest {
                    chat_id: 1,
                    serial: 1,
                    before: None,
                },
                vec![],
            ))
            .unwrap();
        app.receive();
        assert!(!app.history_dirty);
        assert!(matches!(
            requests.try_recv(),
            Ok(Request::History(HistoryRequest { chat_id: 1, .. }))
        ));
        assert!(requests.try_recv().is_err());
    }

    #[test]
    fn unrelated_updates_do_not_refresh_but_deletion_without_chat_does() {
        let (mut app, events, mut requests) = app();
        app.loading = false;
        events.send(Event::MessagesChanged(Some(2))).unwrap();
        app.receive();
        assert!(requests.try_recv().is_err());
        events.send(Event::MessagesChanged(None)).unwrap();
        app.receive();
        assert!(matches!(
            requests.try_recv(),
            Ok(Request::History(HistoryRequest { chat_id: 1, .. }))
        ));
    }

    #[test]
    fn update_stream_error_is_visible_and_chat_stays_open() {
        let (mut app, events, _requests) = app();
        events
            .send(Event::UpdatesStopped("offline".into()))
            .unwrap();
        app.receive();
        assert!(app.screen == Screen::Chats);
        assert!(app.error.as_deref().unwrap().contains("offline"));
    }
    #[test]
    fn chat_pages_append_and_duplicate_clicks_are_ignored() {
        let (mut app, events, mut requests) = app();
        app.chats_has_more = true;
        app.load_more_chats();
        app.load_more_chats();
        assert!(matches!(requests.try_recv(), Ok(Request::MoreChats)));
        assert!(requests.try_recv().is_err());
        events
            .send(Event::MoreChats(
                vec![Chat {
                    id: 2,
                    title: "second".into(),
                }],
                false,
            ))
            .unwrap();
        app.receive();
        assert_eq!(app.chats.len(), 2);
        assert_eq!(app.chats[1].id, 2);
        assert!(!app.chats_has_more);
        assert!(!app.chats_loading);
    }

    #[test]
    fn older_history_stays_visible_on_update_until_latest_is_requested() {
        let (mut app, events, mut requests) = app();
        app.loading = false;
        app.load_history(Some(51));
        let request = match requests.try_recv().unwrap() {
            Request::History(request) => request,
            _ => panic!("expected older history request"),
        };
        assert_eq!(request.before, Some(51));
        events.send(Event::MessagesChanged(Some(1))).unwrap();
        events
            .send(Event::History(
                request,
                vec![Message {
                    id: 50,
                    sender: "a".into(),
                    text: "older".into(),
                }],
            ))
            .unwrap();
        app.receive();
        assert_eq!(app.messages[0].text, "older");
        assert!(!app.history_latest);
        assert!(app.history_changed);
        assert!(requests.try_recv().is_err());
        app.load_history(None);
        assert!(matches!(
            requests.try_recv(),
            Ok(Request::History(HistoryRequest { before: None, .. }))
        ));
    }

    #[test]
    fn stale_response_for_same_chat_does_not_override_newer_page() {
        let (mut app, events, mut requests) = app();
        app.load_history(Some(50));
        let newer = match requests.try_recv().unwrap() {
            Request::History(request) => request,
            _ => panic!("expected history request"),
        };
        events
            .send(Event::History(
                HistoryRequest {
                    chat_id: 1,
                    serial: 1,
                    before: None,
                },
                vec![Message {
                    id: 99,
                    sender: "a".into(),
                    text: "stale".into(),
                }],
            ))
            .unwrap();
        app.receive();
        assert!(app.messages.is_empty());
        assert!(app.loading);
        events
            .send(Event::History(
                newer,
                vec![Message {
                    id: 49,
                    sender: "b".into(),
                    text: "older".into(),
                }],
            ))
            .unwrap();
        app.receive();
        assert_eq!(app.messages[0].text, "older");
        assert!(!app.loading);
    }

    #[test]
    fn empty_older_page_keeps_current_messages() {
        let (mut app, events, mut requests) = app();
        app.messages.push(Message {
            id: 1,
            sender: "a".into(),
            text: "first".into(),
        });
        app.load_history(Some(1));
        let request = match requests.try_recv().unwrap() {
            Request::History(request) => request,
            _ => panic!("expected history request"),
        };
        events.send(Event::History(request, vec![])).unwrap();
        app.receive();
        assert_eq!(app.messages[0].text, "first");
        assert!(!app.history_has_more);
    }
}
