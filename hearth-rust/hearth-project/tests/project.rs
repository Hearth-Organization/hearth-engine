use std::fs;
use std::path::{Path, PathBuf};

use hearth_project::*;
use serde_json::json;

fn write(p: &Path, v: serde_json::Value) {
    fs::create_dir_all(p.parent().unwrap()).unwrap();
    fs::write(p, serde_json::to_string_pretty(&v).unwrap()).unwrap();
}

/// An engine with gems A, B (needs A), Nested inside B, D (needs a missing X); a project on it with Atom on.
fn setup(name: &str) -> (Home, PathBuf, PathBuf) {
    let r = std::env::temp_dir().join(format!("hearth-project-{name}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&r);
    let engine = r.join("engine");
    write(&engine.join("engine.json"), json!({"engine_name": "hearth-engine", "external_subdirectories": ["Gems/A", "Gems/B", "Gems/D", "Gems/Atom"]}));
    write(&engine.join("Gems/A/gem.json"), json!({"gem_name": "A"}));
    write(&engine.join("Gems/Atom/gem.json"), json!({"gem_name": "Atom"}));
    write(&engine.join("Gems/B/gem.json"), json!({"gem_name": "B", "dependencies": ["A>=1.0"], "external_subdirectories": ["Inner"]}));
    write(&engine.join("Gems/B/Inner/gem.json"), json!({"gem_name": "Nested"}));
    write(&engine.join("Gems/D/gem.json"), json!({"gem_name": "D", "dependencies": ["X"]}));
    let project = r.join("game");
    write(&project.join("project.json"), json!({"project_name": "Game", "engine": "hearth-engine", "gem_names": ["Atom", "Game"], "external_subdirectories": ["Gem"]}));
    write(&project.join("Gem/gem.json"), json!({"gem_name": "Game"}));
    write(&r.join("o3de/o3de_manifest.json"), json!({"engines": [engine.to_str().unwrap()]}));
    (Home::load(Some(r.join("o3de"))).unwrap(), project, engine)
}

#[test]
fn finds_the_engine_and_every_gem() {
    let (home, project, engine) = setup("find");
    assert_eq!(engine_for(&home, &project).unwrap(), engine);
    let gems = gems_for(&home, &project).unwrap();
    assert_eq!(gems.keys().cloned().collect::<Vec<_>>(), ["A", "Atom", "B", "D", "Game", "Nested"]);
    assert_eq!(gems["B"].needs, ["A"]);
    assert!(check(&home, &project).unwrap().is_empty());
}

#[test]
fn switching_on_is_safe_and_keeps_order() {
    let (home, project, _) = setup("enable");
    assert_eq!(enable(&home, &project, "B").unwrap(), "B is on");
    assert_eq!(enabled(&project).unwrap(), ["Atom", "B", "Game"], "in order");
    assert!(enable(&home, &project, "B").unwrap().contains("already"));
    let before = fs::read_to_string(project.join("project.json")).unwrap();
    assert!(before.contains("    \"project_name\""), "4-space indent, keys in their order");
    assert!(before.find("project_name").unwrap() < before.find("engine").unwrap());
    let e = enable(&home, &project, "HearthRust").unwrap_err();
    assert!(e.contains("install a Hearth Engine that has it") && e.contains("Nothing was changed"), "{e}");
    assert!(enable(&home, &project, "D").unwrap_err().contains("needs X"));
    assert_eq!(fs::read_to_string(project.join("project.json")).unwrap(), before, "a refused change changes nothing");
}

#[test]
fn switching_off_respects_what_needs_it_and_check_explains() {
    let (home, project, _) = setup("disable");
    enable(&home, &project, "A").unwrap();
    enable(&home, &project, "B").unwrap();
    assert!(disable(&home, &project, "A").unwrap_err().contains("B need A"));
    assert_eq!(disable(&home, &project, "B").unwrap(), "B is off");
    assert_eq!(disable(&home, &project, "A").unwrap(), "A is off");
    assert!(disable(&home, &project, "A").unwrap().contains("wasn't on"));

    // A project that has a gem the engine lacks (what happens when the engine is older than the project).
    let mut pj: serde_json::Value = serde_json::from_str(&fs::read_to_string(project.join("project.json")).unwrap()).unwrap();
    pj["gem_names"].as_array_mut().unwrap().push(json!("HearthRust"));
    fs::write(project.join("project.json"), pj.to_string()).unwrap();
    let p = check(&home, &project).unwrap();
    assert_eq!(p, ["HearthRust is switched on but this engine doesn't have it"]);
}

#[test]
fn a_wrong_engine_name_is_explained() {
    let (home, project, _) = setup("noengine");
    write(&project.join("project.json"), json!({"engine": "other-engine", "gem_names": []}));
    assert!(engine_for(&home, &project).unwrap_err().contains("\"other-engine\""));
    assert_eq!(bare("Atom_RPI>=1.2.3"), "Atom_RPI");
}
