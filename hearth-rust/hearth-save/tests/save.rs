use std::fs;

use hearth_save::*;
use serde_json::json;

fn dir(name: &str) -> std::path::PathBuf {
    let d = std::env::temp_dir().join(format!("hearth-save-{name}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&d);
    fs::create_dir_all(&d).unwrap();
    d
}

#[test]
fn saves_merge_and_keep_a_backup() {
    let d = dir("merge");
    let p = d.join("settings.json");
    assert_eq!(load(&p), Loaded::Missing);
    save_changes(&p, &json!({"audio": {"music": 0.5}, "language": "en"})).unwrap();
    save_changes(&p, &json!({"audio": {"voices": 0.8}})).unwrap();
    let Loaded::Good(v) = load(&p) else { panic!() };
    assert_eq!(v, json!({"audio": {"music": 0.5, "voices": 0.8}, "language": "en"}), "the second save didn't undo the first");
    assert!(backup_path(&p).exists());
    save_changes(&p, &json!({"language": null})).unwrap();
    let Loaded::Good(v) = load(&p) else { panic!() };
    assert!(v.get("language").is_none());
    assert!(fs::read_dir(&d).unwrap().all(|e| !e.unwrap().file_name().to_string_lossy().contains(".tmp-")), "no leftovers");
}

#[test]
fn a_broken_file_falls_back_to_the_backup_and_is_never_overwritten_with_defaults() {
    let d = dir("broken");
    let p = d.join("settings.json");
    save_changes(&p, &json!({"brightness": 70})).unwrap();
    save_changes(&p, &json!({"brightness": 80})).unwrap();
    fs::write(&p, "{\"brightness\": 8").unwrap(); // a cut-off write
    assert_eq!(load(&p), Loaded::FromBackup(json!({"brightness": 70})));

    // Saving again builds on the backup, not on nothing.
    save_changes(&p, &json!({"volume": 3})).unwrap();
    assert_eq!(load(&p), Loaded::Good(json!({"brightness": 70, "volume": 3})));

    // Both broken: loading says so, saving refuses and changes nothing.
    fs::write(&p, "nope").unwrap();
    fs::write(backup_path(&p), "").unwrap();
    assert!(matches!(load(&p), Loaded::Broken(_)));
    assert!(save_changes(&p, &json!({"volume": 4})).unwrap_err().contains("Nothing was changed"));
    assert_eq!(fs::read_to_string(&p).unwrap(), "nope");

    // Only when asked is it moved aside (never deleted).
    save_whole(&p, &json!({"fresh": true}), true).unwrap();
    assert_eq!(load(&p), Loaded::Good(json!({"fresh": true})));
    assert!(fs::read_dir(&d).unwrap().any(|e| e.unwrap().file_name().to_string_lossy().contains(".broken-")));
}

#[test]
fn slots_list_with_summaries() {
    let d = dir("slots");
    let s = Slots { dir: d.join("saves") };
    s.save(2, json!({"level": "Level 9", "minutes": 42}), json!({"hp": 80})).unwrap();
    s.save(1, json!({"level": "Level 0", "minutes": 5}), json!({"hp": 100})).unwrap();
    assert!(s.load(3).unwrap_err().contains("empty"));
    assert_eq!(s.load(2).unwrap(), json!({"hp": 80}));
    let l = s.list();
    assert_eq!(l.iter().map(|i| i.slot).collect::<Vec<_>>(), vec![1, 2]);
    assert_eq!(l[1].summary["level"], "Level 9");
    fs::write(s.path(1), "x").unwrap();
    let l = s.list();
    assert!(l[0].broken.is_some(), "a broken slot shows as broken, not missing");
}
