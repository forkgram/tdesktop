//! Offline GUI harness: synthetic chat and a local channel, no Telegram session.
#![allow(dead_code)]
mod production {
    include!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/src/bin/rustgram-gui.rs"
    ));

    pub fn run_fixture() -> eframe::Result {
        let options = eframe::NativeOptions {
            viewport: egui::ViewportBuilder::default().with_inner_size([960.0, 640.0]),
            ..Default::default()
        };
        eframe::run_native(
            "rustGram offline fixture",
            options,
            Box::new(|cc| {
                let mut style = (*cc.egui_ctx.style_of(egui::Theme::Light)).clone();
                style.visuals = egui::Visuals::light();
                style.visuals.selection.bg_fill = Color32::from_rgb(62, 140, 200);
                cc.egui_ctx.set_style_of(egui::Theme::Light, style);
                cc.egui_ctx.set_theme(egui::Theme::Light);
                let (tx, mut requests) = async_mpsc::unbounded_channel();
                let (events, rx) = mpsc::channel();
                let ctx = cc.egui_ctx.clone();
                thread::spawn(move || {
                    let mut sent = String::new();
                    send_event(&events, &ctx, Event::MessagesChanged(Some(1)));
                    while let Some(request) = requests.blocking_recv() {
                        match request {
                            Request::Send(1, text) => {
                                sent = text;
                                eprintln!("OFFLINE_SEND_RECEIVED");
                                send_event(&events, &ctx, Event::Sent(1));
                            }
                            Request::History(1) => {
                                if sent.is_empty() {
                                    eprintln!("OFFLINE_LIVE_UPDATE_REFRESHED");
                                } else {
                                    eprintln!("OFFLINE_HISTORY_REFRESHED");
                                }
                                send_event(
                                    &events,
                                    &ctx,
                                    Event::History(
                                        1,
                                        vec![
                                            Message {
                                                sender: "Local example".into(),
                                                text: "This is an offline fixture.".into(),
                                            },
                                            Message {
                                                sender: "You".into(),
                                                text: sent.clone(),
                                            },
                                        ],
                                    ),
                                );
                            }
                            _ => (),
                        }
                    }
                });
                Ok(Box::new(App {
                    tx,
                    rx,
                    screen: Screen::Chats,
                    phone: String::new(),
                    secret: String::new(),
                    error: None,
                    chats: vec![Chat {
                        id: 1,
                        title: "Offline chat".into(),
                    }],
                    selected: Some(1),
                    messages: vec![Message {
                        sender: "Local example".into(),
                        text: "This is an offline fixture.".into(),
                    }],
                    drafts: HashMap::new(),
                    pending: HashMap::new(),
                    send_errors: HashMap::new(),
                    loading: false,
                    history_dirty: false,
                }))
            }),
        )
    }
}
fn main() -> eframe::Result {
    production::run_fixture()
}
