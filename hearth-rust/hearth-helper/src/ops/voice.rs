//! Voice Studio (the owner, Nov 27: the Foundation's lines, made "in voice studio"). The same server and lines file
//! as the editor's Voice Studio: the server and its key come from ~/.config/hearth-engine/settings.ini (voice/server,
//! voice/apiKey) and stay on this PC; lines go into <project>/Voice/voice_lines.json with the .wav and .srt.

use std::fs;
use std::path::Path;
use std::sync::Mutex;

use serde_json::{json, Map, Value};

use crate::ctx::p;
use crate::ops::forge::http_json;
use crate::util::*;

const MOODS: [&str; 4] = ["Calm", "Urgent", "Whisper", "Over radio"];
const MOOD_INSTRUCTIONS: [&str; 4] = ["calm, steady, clear", "urgent, fast, tense", "whispering, quiet, close", "speaking over a radio, clipped"];
const ROOM_FILTERS: [&str; 5] = ["", "aecho=0.8:0.6:20:0.25", "aecho=0.8:0.7:40|70:0.3|0.2", "aecho=0.8:0.8:80|140:0.35|0.25", "aecho=0.8:0.9:200|350:0.4|0.3"];

fn voice_settings() -> (String, String) {
    let ini = settings_ini();
    let mut server = ini_get(&ini, "voice", "server").unwrap_or_default().trim().to_string();
    if !server.is_empty() && !server.contains("://") {
        server = format!("http://{server}");
    }
    if !server.is_empty() {
        let last = server.rsplit(':').next().unwrap_or("").trim_matches('/');
        if last.is_empty() || !last.chars().all(|c| c.is_ascii_digit()) {
            server.push_str(":8890");
        }
    }
    (server.trim_end_matches('/').to_string(), ini_get(&ini, "voice", "apiKey").unwrap_or_default().trim().to_string())
}

fn voice_http(method: &str, path: &str, body: Option<&Value>, timeout: u64) -> (u16, Value, Option<Vec<u8>>) {
    let (server, key) = voice_settings();
    if server.is_empty() {
        return (0, json!({"error": "Voice Studio has no Hearth server set (Hearth Engine › Voice Studio › Server)"}), None);
    }
    let (code, v, raw) = http_json(method, &format!("{server}{path}"), body, timeout, Some(&key));
    if code == 0 {
        return (0, json!({"error": format!("the Hearth server isn't answering at {server} ({})", v["error"].as_str().unwrap_or(""))}), None);
    }
    (code, v, raw)
}

pub fn op_voice_status(_args: &Value) -> Value {
    let (server, key) = voice_settings();
    let (code, health, _) = voice_http("GET", "/health", None, 15);
    let (vcode, voices, raw) = voice_http("GET", "/v1/audio/voices", None, 30);
    let voices = if voices.is_object() { voices.get("voices").cloned().unwrap_or(Value::Null) } else { json!(String::from_utf8_lossy(&raw.unwrap_or_default())) };
    json!({"server": server, "has_key": !key.is_empty(), "health_code": code, "health": if health.is_object() { health } else { Value::Null },
           "voices_code": vcode, "voices": voices})
}

pub fn op_voice_design(args: &Value) -> Value {
    let (code, answer, raw) = voice_http("POST", "/api/voices/design", Some(&json!({"name": text(args, "name"), "description": text(args, "description")})), 300);
    json!({"ok": code == 200, "code": code,
           "answer": if answer.is_object() { answer } else { json!(String::from_utf8_lossy(&raw.unwrap_or_default()).chars().take(300).collect::<String>()) }})
}

/// Subtitles: one cue per sentence, timed by its share of the letters.
fn write_srt(path: &Path, text: &str, seconds: f64) {
    let mut parts: Vec<String> = Vec::new();
    let mut cur = String::new();
    let chars: Vec<char> = text.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        cur.push(chars[i]);
        if ".!?".contains(chars[i]) && i + 1 < chars.len() && chars[i + 1].is_whitespace() {
            parts.push(cur.clone());
            cur.clear();
            while i + 1 < chars.len() && chars[i + 1].is_whitespace() {
                i += 1;
            }
        }
        i += 1;
    }
    if !cur.is_empty() {
        parts.push(cur);
    }
    if parts.is_empty() {
        parts.push(text.to_string());
    }
    let total = parts.iter().map(|p| p.chars().count()).sum::<usize>().max(1) as f64;
    let stamp = |t: f64| {
        let ms = (t * 1000.0).round() as i64;
        format!("{:02}:{:02}:{:02},{:03}", ms / 3_600_000, ms % 3_600_000 / 60_000, ms % 60_000 / 1000, ms % 1000)
    };
    let mut out = Vec::new();
    let mut at = 0.0;
    for (i, part) in parts.iter().enumerate() {
        let end = if i == parts.len() - 1 { seconds } else { at + seconds * part.chars().count() as f64 / total };
        out.push(format!("{}\n{} --> {}\n{}\n", i + 1, stamp(at), stamp(end), part));
        at = end;
    }
    let _ = fs::write(path, out.join("\n"));
}

/// A .wav's length in seconds, from its header.
pub fn wav_seconds(path: &Path) -> Option<f64> {
    let b = fs::read(path).ok()?;
    if b.len() < 12 || &b[0..4] != b"RIFF" || &b[8..12] != b"WAVE" {
        return None;
    }
    let (mut i, mut byte_rate, mut data) = (12usize, 0u32, None);
    while i + 8 <= b.len() {
        let id = &b[i..i + 4];
        let size = u32::from_le_bytes([b[i + 4], b[i + 5], b[i + 6], b[i + 7]]) as usize;
        if id == b"fmt " && i + 20 <= b.len() {
            byte_rate = u32::from_le_bytes([b[i + 16], b[i + 17], b[i + 18], b[i + 19]]);
        }
        if id == b"data" {
            data = Some(size.min(b.len().saturating_sub(i + 8)));
            break;
        }
        i += 8 + size + (size & 1);
    }
    let rate = if byte_rate == 0 { 48000 } else { byte_rate }; // Python's default: 24000 Hz of 16-bit mono
    Some(data? as f64 / rate as f64)
}

static VOICE_LOCK: Mutex<()> = Mutex::new(());

/// One batch at a time (they share the lines file).
pub fn op_voice_lines(args: &Value) -> Value {
    let _g = VOICE_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    voice_lines(args)
}

fn slug(s: &str) -> String {
    let mut out = String::new();
    let mut dash = false;
    for c in s.to_lowercase().chars() {
        if c.is_ascii_lowercase() || c.is_ascii_digit() {
            out.push(c);
            dash = false;
        } else if !dash {
            out.push('-');
            dash = true;
        }
    }
    out
}

/// voice_lines {"scene", "lines": [{"id", "who", "voice", "text", "mood", "speed", "room", "usedIn"}], "remake", "adopt"}:
/// adds (or updates, by id) the lines in the project's Voice Studio and makes each one that's new or changed.
fn voice_lines(args: &Value) -> Value {
    let folder = p().project.join("Voice");
    let _ = fs::create_dir_all(&folder);
    let data_file = folder.join("voice_lines.json");
    let mut data = read_json(&data_file).filter(Value::is_object).unwrap_or(json!({}));
    if !data.get("scenes").map(Value::is_array).unwrap_or(false) || data["scenes"].as_array().map(|a| a.is_empty()).unwrap_or(true) {
        data["scenes"] = json!(["Foundation intro", "PA announcements", "Player radio", "Level 0 whispers"].iter().map(|n| json!({"name": n, "lines": []})).collect::<Vec<_>>());
    }
    let scene_name = text(args, "scene");
    let scenes = data["scenes"].as_array_mut().expect("made an array above");
    let index = match scenes.iter().position(|x| x["name"] == scene_name.as_str()) {
        Some(i) => i,
        None => {
            scenes.push(json!({"name": scene_name, "lines": []}));
            scenes.len() - 1
        }
    };
    let scene_folder = slug(&scene_name);
    let _ = fs::create_dir_all(folder.join(&scene_folder));
    let (mut made, mut failed) = (Vec::new(), Vec::new());
    let wanted_lines = args.get("lines").and_then(Value::as_array).cloned().unwrap_or_default();
    for wanted in wanted_lines {
        let id = wanted.get("id").map(|v| v.as_str().map(str::to_string).unwrap_or_else(|| v.to_string())).unwrap_or_default();
        let lines = data["scenes"][index]["lines"].as_array_mut().expect("scenes have lines");
        let li = match lines.iter().position(|l| l.get("id").map(|v| v.as_str().map(str::to_string).unwrap_or_else(|| v.to_string())) == Some(id.clone())) {
            Some(i) => i,
            None => {
                lines.push(json!({"id": wanted["id"]}));
                lines.len() - 1
            }
        };
        let line: &mut Map<String, Value> = lines[li].as_object_mut().expect("a line is an object");
        for (k, default) in [("voice", json!("")), ("who", json!("Someone")), ("text", json!("")), ("mood", json!("Calm")), ("speed", json!(1.0)), ("room", json!(0)), ("usedIn", json!(""))] {
            let v = wanted.get(k).cloned().or_else(|| line.get(k).cloned()).unwrap_or(default);
            line.insert(k.into(), v);
        }
        let get_s = |line: &Map<String, Value>, k: &str| line.get(k).and_then(Value::as_str).unwrap_or("").to_string();
        let (line_text, line_voice, line_mood) = (get_s(line, "text"), get_s(line, "voice"), get_s(line, "mood"));
        let speed = line.get("speed").and_then(|v| v.as_f64().or_else(|| v.as_str().and_then(|t| t.parse().ok()))).unwrap_or(1.0);
        let room = line.get("room").and_then(|v| v.as_i64().or_else(|| v.as_f64().map(|f| f as i64))).unwrap_or(0);
        let key = format!("{line_text}|{line_voice}|{line_mood}|{speed:.2}|{room}");
        let digest = sha1_smol::Sha1::from(key.as_bytes()).digest().to_string()[..12].to_string();
        let relative = format!("{scene_folder}/{id}.wav");
        let final_path = folder.join(&relative);
        let srt_rel = format!("{scene_folder}/{id}.srt");
        let has_made = line.get("made").map(|m| m.is_object()).unwrap_or(false);
        if line.get("made").and_then(|m| m.get("hash")).and_then(Value::as_str) == Some(&digest) && final_path.is_file() && !flag(args, "remake", false) {
            continue;
        }
        if !has_made && final_path.is_file() && flag(args, "adopt", false) {
            // Already recorded (its entry was lost): keep the recording, just list it again.
            let seconds = wav_seconds(&final_path).unwrap_or(0.0);
            line.insert("made".into(), json!({"hash": digest, "file": relative, "takes": [], "subtitles": srt_rel, "lipsync": "", "seconds": (seconds * 1000.0).round() / 1000.0}));
            made.push(json!({"id": id, "seconds": (seconds * 100.0).round() / 100.0, "adopted": true}));
            continue;
        }
        let mood = MOODS.iter().position(|m| *m == line_mood).unwrap_or(0);
        let request = json!({"input": line_text, "voice": line_voice, "speed": speed, "response_format": "wav", "instructions": MOOD_INSTRUCTIONS[mood]});
        let (code, answer, raw) = voice_http("POST", "/v1/audio/speech", Some(&request), 300);
        let audio = raw.unwrap_or_default();
        if code != 200 || !audio.starts_with(b"RIFF") {
            failed.push(json!({"id": id, "code": code, "error": if audio.is_empty() { answer.to_string() } else { String::from_utf8_lossy(&audio).chars().take(200).collect::<String>() }}));
            continue;
        }
        let radio = if mood == 3 { "highpass=f=300,lowpass=f=3200" } else { "" };
        let filters: Vec<&str> = [radio, ROOM_FILTERS[room.clamp(0, 4) as usize]].into_iter().filter(|f| !f.is_empty()).collect();
        let raw_path = final_path.with_extension("raw.wav");
        let _ = fs::write(if filters.is_empty() { &final_path } else { &raw_path }, &audio);
        if !filters.is_empty() {
            let rp = raw_path.display().to_string();
            let fp = final_path.display().to_string();
            let af = filters.join(",");
            let (_, _, done) = run(&["ffmpeg", "-y", "-loglevel", "error", "-i", &rp, "-af", &af, &fp], None, std::time::Duration::from_secs(300));
            if done != 0 {
                let _ = fs::rename(&raw_path, &final_path);
            }
            let _ = fs::remove_file(&raw_path);
        }
        let seconds = wav_seconds(&final_path).unwrap_or(0.0);
        write_srt(&folder.join(&srt_rel), &line_text, seconds);
        line.insert("made".into(), json!({"hash": digest, "file": relative, "takes": [], "subtitles": srt_rel, "lipsync": "", "seconds": (seconds * 1000.0).round() / 1000.0}));
        made.push(json!({"id": id, "seconds": (seconds * 100.0).round() / 100.0}));
        let _ = write_atomic(&data_file, &pretty(&data, 4));
    }
    let _ = write_atomic(&data_file, &pretty(&data, 4));
    json!({"ok": failed.is_empty(), "made": made, "failed": failed, "lines_file": data_file.display().to_string()})
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scene_folders_like_python() {
        assert_eq!(slug("Foundation intro"), "foundation-intro");
        assert_eq!(slug("Level 0 whispers!"), "level-0-whispers-");
    }

    #[test]
    fn reads_a_wav_header() {
        let dir = std::env::temp_dir().join(format!("hearth-helper-wav-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("a.wav");
        let data = vec![0u8; 48000]; // one second of 24 kHz 16-bit mono
        let mut b = b"RIFF".to_vec();
        b.extend(((36 + data.len()) as u32).to_le_bytes());
        b.extend(b"WAVEfmt ");
        b.extend(16u32.to_le_bytes());
        b.extend(1u16.to_le_bytes());
        b.extend(1u16.to_le_bytes());
        b.extend(24000u32.to_le_bytes());
        b.extend(48000u32.to_le_bytes());
        b.extend(2u16.to_le_bytes());
        b.extend(16u16.to_le_bytes());
        b.extend(b"data");
        b.extend((data.len() as u32).to_le_bytes());
        b.extend(&data);
        fs::write(&path, b).unwrap();
        assert_eq!(wav_seconds(&path), Some(1.0));
        fs::remove_dir_all(&dir).unwrap();
    }
}
