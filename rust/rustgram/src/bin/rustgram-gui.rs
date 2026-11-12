use eframe::egui::{self, Color32, RichText};
use grammers_client::{Client, SignInError};
use grammers_mtsender::SenderPool;
use grammers_session::{storages::SqliteSession, types::PeerRef};
use rustgram::{data_dir, private_session, Result};
use std::{
    collections::HashMap,
    env,
    sync::{mpsc, Arc},
    thread,
};
use tokio::sync::mpsc as async_mpsc;

#[derive(Clone, Debug)]
struct Chat {
    id: i64,
    title: String,
}
#[derive(Clone, Debug)]
struct Message {
    sender: String,
    text: String,
}

enum Request {
    Connect(String),
    Code(String),
    Password(String),
    History(i64),
    Send(i64, String),
}
enum Event {
    Code,
    Password,
    Chats(Vec<Chat>),
    History(i64, Vec<Message>),
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
    let SenderPool { runner, handle, .. } = SenderPool::new(session, api_id);
    let client = Client::new(handle);
    let task = tokio::spawn(runner.run());
    let result = connected_inner(&client, phone, &api_hash, rx, tx, ctx).await;
    task.abort();
    result
}

async fn connected_inner(
    client: &Client,
    phone: String,
    api_hash: &str,
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
    let mut chats: Vec<(Chat, PeerRef)> = Vec::new();
    while chats.len() < 50 {
        let Some(dialog) = iter.next().await? else {
            break;
        };
        let peer = dialog.peer();
        if let (Some(id), Some(reference)) = (peer.id().bot_api_dialog_id(), peer.to_ref().await?) {
            chats.push((
                Chat {
                    id,
                    title: peer.name().unwrap_or("Без названия").to_owned(),
                },
                reference,
            ));
        }
    }
    send_event(
        tx,
        ctx,
        Event::Chats(chats.iter().map(|(chat, _)| chat.clone()).collect()),
    );
    while let Some(request) = rx.recv().await {
        match request {
            Request::History(id) => {
                let Some((_, reference)) = chats.iter().find(|(chat, _)| chat.id == id) else {
                    continue;
                };
                let mut iter = client.iter_messages(*reference);
                let mut messages = Vec::new();
                while messages.len() < 50 {
                    let Some(message) = iter.next().await? else {
                        break;
                    };
                    messages.push(Message {
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
                send_event(tx, ctx, Event::History(id, messages));
            }
            Request::Send(id, text) => {
                let peer = chats
                    .iter()
                    .find(|(chat, _)| chat.id == id)
                    .map(|(_, peer)| *peer);
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
    selected: Option<i64>,
    messages: Vec<Message>,
    drafts: HashMap<i64, String>,
    pending: HashMap<i64, String>,
    send_errors: HashMap<i64, String>,
    loading: bool,
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
            selected: None,
            messages: vec![],
            drafts: HashMap::new(),
            pending: HashMap::new(),
            send_errors: HashMap::new(),
            loading: false,
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
    fn receive(&mut self) {
        while let Ok(event) = self.rx.try_recv() {
            match event {
                Event::Code => self.screen = Screen::Code,
                Event::Password => self.screen = Screen::Password,
                Event::Chats(chats) => {
                    self.chats = chats;
                    self.screen = Screen::Chats;
                    self.error = None;
                }
                Event::History(id, messages) if self.selected == Some(id) => {
                    self.messages = messages;
                    self.loading = false;
                }
                Event::History(_, _) => (),
                Event::Sent(id) => {
                    if let Some(sent) = self.pending.remove(&id) {
                        if self.drafts.get(&id) == Some(&sent) {
                            self.drafts.remove(&id);
                        }
                    }
                    self.send_errors.remove(&id);
                    if self.selected == Some(id) {
                        self.loading = true;
                        self.send(Request::History(id));
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
                    self.messages.clear();
                    self.selected = None;
                    self.loading = false;
                }
            }
        }
    }
}
impl eframe::App for App {
    fn ui(&mut self, ui: &mut egui::Ui, _: &mut eframe::Frame) {
        self.receive();
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
                            self.loading = true;
                            self.send(Request::History(id));
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
                        .map(|c| c.title.as_str())
                        .unwrap_or("Чат");
                    ui.heading(title);
                    ui.separator();
                    if self.loading {
                        ui.spinner();
                        ui.label("Загрузка сообщений…");
                    } else {
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
                2,
                vec![Message {
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
                1,
                vec![Message {
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
        assert!(matches!(requests.try_recv(), Ok(Request::History(1))));
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
}
