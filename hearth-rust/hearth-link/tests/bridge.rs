//! Hearth Link against a stand-in for the editor's bridge (the same protocol as Editor/Scripts/claude_bridge.py).

use std::io::{BufRead, BufReader, Write};
use std::net::TcpListener;
use std::path::PathBuf;
use std::time::Duration;

use hearth_link::{Bridge, Editor, Error, Session, Transport};
use serde_json::{json, Value};

fn temp_project(tag: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("hearth-link-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(Session::folder(&dir)).unwrap();
    dir
}

/// Answers each request line like the bridge does; `answer` gets the request and returns the reply's fields.
fn fake_bridge(token: &'static str, requests: usize, answer: fn(&Value) -> Value) -> u16 {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    std::thread::spawn(move || {
        for _ in 0..requests {
            let (stream, _) = listener.accept().unwrap();
            let mut reader = BufReader::new(stream.try_clone().unwrap());
            let mut line = String::new();
            reader.read_line(&mut line).unwrap();
            let request: Value = serde_json::from_str(&line).unwrap();
            let mut reply = if request["token"] == token { answer(&request) } else { json!({ "ok": false, "error": "bad token" }) };
            reply["id"] = request["id"].clone();
            let mut out = stream;
            out.write_all(format!("{reply}\n").as_bytes()).unwrap();
        }
    });
    port
}

#[test]
fn reads_the_level_over_the_socket() {
    let project = temp_project("socket");
    let port = fake_bridge("secret", 1, |r| {
        assert_eq!(r["op"], "python");
        assert!(r["code"].as_str().unwrap().contains("get_current_level_name"));
        json!({ "ok": true, "output": "something printed\nHEARTH_LINK {\"name\": \"Level0\", \"playing\": true}\n" })
    });
    let session = Session { port, token: "secret".into(), pid: None, started: None };
    let editor = Editor::new(Bridge::with_session(&project, session).transport(Transport::Socket));
    let level = editor.level().unwrap();
    assert_eq!(level.name, "Level0");
    assert!(level.playing);
}

#[test]
fn a_wrong_token_is_turned_away() {
    let project = temp_project("token");
    let port = fake_bridge("secret", 1, |_| json!({ "ok": true, "output": "pong" }));
    let session = Session { port, token: "wrong".into(), pid: None, started: None };
    let bridge = Bridge::with_session(&project, session).transport(Transport::Socket);
    match bridge.ping() {
        Err(Error::Editor(why)) => assert!(why.contains("bad token")),
        other => panic!("expected the editor to say no, got {other:?}"),
    }
}

#[test]
fn the_editors_own_error_comes_back_in_its_words() {
    let project = temp_project("error");
    let port = fake_bridge("secret", 1, |_| json!({ "ok": false, "error": "the owner paused Claude in the Script console" }));
    let session = Session { port, token: "secret".into(), pid: None, started: None };
    let editor = Editor::new(Bridge::with_session(&project, session).transport(Transport::Socket));
    let e = editor.play().unwrap_err();
    assert!(e.to_string().contains("paused Claude"), "{e}");
}

#[test]
fn works_through_files_when_there_is_no_socket() {
    let project = temp_project("files");
    let folder = Session::folder(&project);
    let watch = folder.clone();
    std::thread::spawn(move || {
        let inbox = watch.join("inbox");
        let outbox = watch.join("outbox");
        std::fs::create_dir_all(&outbox).unwrap();
        for _ in 0..200 {
            if let Ok(entries) = std::fs::read_dir(&inbox) {
                for entry in entries.flatten() {
                    let path = entry.path();
                    if path.extension().and_then(|e| e.to_str()) != Some("json") {
                        continue;
                    }
                    let request: Value = serde_json::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap();
                    std::fs::remove_file(&path).unwrap();
                    let reply = json!({ "id": request["id"], "ok": true, "output": "pong" });
                    std::fs::write(outbox.join(path.file_name().unwrap()), reply.to_string()).unwrap();
                    return;
                }
            }
            std::thread::sleep(Duration::from_millis(25));
        }
    });
    // Port 1 has nothing listening: Auto falls back to the files.
    let session = Session { port: 1, token: "secret".into(), pid: None, started: None };
    let bridge = Bridge::with_session(&project, session).timeout(Duration::from_secs(5));
    bridge.ping().unwrap();
}

#[test]
fn places_an_entity_with_its_name_sent_as_data() {
    let project = temp_project("place");
    let port = fake_bridge("secret", 1, |r| {
        let code = r["code"].as_str().unwrap();
        assert!(code.contains("CreateNewEntity"));
        assert!(!code.contains("\nGym door\n"), "the name only travels inside the JSON values");
        json!({ "ok": true, "output": "HEARTH_LINK {\"id\": \"[42]\", \"name\": \"Gym door\", \"position\": [1.0, 2.0, 0.0]}" })
    });
    let session = Session { port, token: "secret".into(), pid: None, started: None };
    let editor = Editor::new(Bridge::with_session(&project, session).transport(Transport::Socket));
    let placed = editor.place("Gym door", [1.0, 2.0, 0.0]).unwrap();
    assert_eq!(placed.id, "[42]");
    assert_eq!(placed.position, Some([1.0, 2.0, 0.0]));
}
