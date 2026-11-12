use grammers_client::{Client, SignInError};
use grammers_mtsender::SenderPool;
use grammers_session::storages::SqliteSession;
use std::{
    env,
    io::{self, Write},
    sync::Arc,
};

use rustgram::{data_dir, private_session, Result};

fn prompt(label: &str) -> Result<String> {
    print!("{label}");
    io::stdout().flush()?;
    let mut line = String::new();
    io::stdin().read_line(&mut line)?;
    if line.trim().is_empty() {
        return Err("empty input".into());
    }
    Ok(line.trim().to_owned())
}

#[derive(Debug, PartialEq, Eq)]
enum Action {
    Help,
    Dialogs,
    History { chat_id: i64, limit: usize },
}

fn parse_args(args: &[String]) -> Result<Action> {
    match args {
        [] => Ok(Action::Dialogs),
        [flag] if flag == "--help" || flag == "-h" => Ok(Action::Help),
        [command] if command == "dialogs" => Ok(Action::Dialogs),
        [command, id] if command == "history" => Ok(Action::History {
            chat_id: id.parse()?,
            limit: 20,
        }),
        [command, id, limit] if command == "history" => {
            let limit: usize = limit.parse()?;
            if !(1..=100).contains(&limit) {
                return Err("history limit must be between 1 and 100".into());
            }
            Ok(Action::History {
                chat_id: id.parse()?,
                limit,
            })
        }
        _ => Err("usage: rustgram [dialogs | history CHAT_ID [LIMIT] | --help]".into()),
    }
}

// Telegram content is untrusted and can include terminal escape sequences.
fn terminal_text(text: &str) -> String {
    text.chars()
        .map(|c| if c.is_control() { ' ' } else { c })
        .collect()
}

async fn run(action: Action) -> Result<()> {
    let api_id: i32 = env::var("RUSTGRAM_API_ID")
        .map_err(|_| "RUSTGRAM_API_ID is required")?
        .parse()?;
    let api_hash = env::var("RUSTGRAM_API_HASH").map_err(|_| "RUSTGRAM_API_HASH is required")?;
    if api_id <= 0 || api_hash.trim().is_empty() {
        return Err("invalid Telegram API credentials".into());
    }
    let path = private_session(&data_dir()?)?;
    let session = Arc::new(SqliteSession::open(path).await?);
    let SenderPool { runner, handle, .. } = SenderPool::new(session, api_id);
    let client = Client::new(handle);
    let runner = tokio::spawn(runner.run());
    let result = async {
        if !client.is_authorized().await? {
            let phone = prompt("Phone (+country code): ")?;
            let token = client.request_login_code(&phone, &api_hash).await?;
            let code = rpassword::prompt_password("Telegram login code: ")?;
            match client.sign_in(&token, code.trim()).await {
                Ok(_) => (),
                Err(SignInError::PasswordRequired(token)) => {
                    let password = rpassword::prompt_password("Two-step verification password: ")?;
                    client.check_password(token, &password).await?;
                }
                Err(e) => return Err(Box::new(e) as Box<dyn std::error::Error + Send + Sync>),
            }
        }
        let mut dialogs = client.iter_dialogs();
        match action {
            Action::Dialogs => {
                let mut count = 0;
                while count < 20 {
                    let Some(dialog) = dialogs.next().await? else {
                        break;
                    };
                    let peer = dialog.peer();
                    if let Some(id) = peer.id().bot_api_dialog_id() {
                        println!(
                            "{}\t{}",
                            id,
                            terminal_text(peer.name().unwrap_or("(unnamed)"))
                        );
                    }
                    count += 1;
                }
            }
            Action::History { chat_id, limit } => {
                let mut selected = None;
                while let Some(dialog) = dialogs.next().await? {
                    let peer = dialog.peer();
                    if peer.id().bot_api_dialog_id() == Some(chat_id) {
                        selected = Some(
                            peer.to_ref()
                                .await?
                                .ok_or("chat reference is unavailable")?,
                        );
                        break;
                    }
                }
                let peer = selected.ok_or("chat not found in dialogs")?;
                let mut messages = client.iter_messages(peer);
                let mut count = 0;
                while count < limit {
                    let Some(message) = messages.next().await? else {
                        break;
                    };
                    let sender = message
                        .sender()
                        .and_then(|peer| peer.name())
                        .unwrap_or("unknown");
                    let body = if message.text().is_empty() {
                        "[non-text message]"
                    } else {
                        message.text()
                    };
                    println!(
                        "{}\t{}\t{}",
                        message.id(),
                        terminal_text(sender),
                        terminal_text(body)
                    );
                    count += 1;
                }
            }
            Action::Help => unreachable!(),
        }
        Ok(())
    }
    .await;
    runner.abort();
    result
}

fn main() {
    let args: Vec<_> = env::args().skip(1).collect();
    let action = match parse_args(&args) {
        Ok(Action::Help) => {
            println!("rustgram — experimental Telegram client\n\nUsage: rustgram [dialogs | history CHAT_ID [LIMIT] | --help]\nSet RUSTGRAM_API_ID and RUSTGRAM_API_HASH before connecting.");
            return;
        }
        Ok(action) => action,
        Err(error) => {
            eprintln!("rustgram: {error}");
            std::process::exit(2);
        }
    };
    let runtime = match tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
    {
        Ok(runtime) => runtime,
        Err(e) => {
            eprintln!("runtime: {e}");
            std::process::exit(1);
        }
    };
    if let Err(error) = runtime.block_on(run(action)) {
        eprintln!("rustgram: {error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        fs,
        os::unix::fs::PermissionsExt,
        path::PathBuf,
        time::{SystemTime, UNIX_EPOCH},
    };

    fn temporary_dir() -> PathBuf {
        env::temp_dir().join(format!(
            "rustgram-test-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ))
    }

    #[test]
    fn parses_commands_and_rejects_invalid_limits() {
        let args = |s: &[&str]| s.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert_eq!(parse_args(&args(&[])).unwrap(), Action::Dialogs);
        assert_eq!(
            parse_args(&args(&["history", "-100123", "30"])).unwrap(),
            Action::History {
                chat_id: -100123,
                limit: 30
            }
        );
        assert!(parse_args(&args(&["history", "1", "0"])).is_err());
        assert!(parse_args(&args(&["history", "1", "101"])).is_err());
        assert!(parse_args(&args(&["bad"])).is_err());
    }

    #[test]
    fn strips_terminal_control_sequences() {
        assert_eq!(terminal_text("a\x1b[2J\nb"), "a [2J b");
    }

    #[test]
    fn creates_private_separate_session_and_can_reopen() {
        let base = temporary_dir();
        let dir = base.join("rustgram");
        let path = private_session(&dir).unwrap();
        assert_eq!(fs::metadata(&dir).unwrap().permissions().mode() & 0o077, 0);
        assert_eq!(fs::metadata(&path).unwrap().permissions().mode() & 0o077, 0);
        assert_eq!(private_session(&dir).unwrap(), path);
        fs::remove_dir_all(base).unwrap();
    }

    #[test]
    fn rejects_public_session_directory() {
        let dir = temporary_dir();
        fs::create_dir(&dir).unwrap();
        fs::set_permissions(&dir, fs::Permissions::from_mode(0o755)).unwrap();
        assert!(private_session(&dir).is_err());
        fs::remove_dir_all(dir).unwrap();
    }
}
