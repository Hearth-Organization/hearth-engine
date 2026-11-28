use hearth_logs::{read, Kind};

const LOG: &str = "\
<12:00:01> (Editor) - Hearth Engine starting
<12:00:02> (Shader) - Trace::Error
<12:00:02> (Shader) -  Gems/Atom/RPI/Code/Source/Shader.cpp(210): 'Compile'
<12:00:02> (Shader) - Failed to compile shader Materials/Door_17.shader
<12:00:03> (AssetProcessor) - [Warning] texture Wall_4.png is 8192 wide
<12:00:03> (AssetProcessor) - [Warning] texture Wall_5.png is 8192 wide
<12:00:04> (Shader) - Trace::Error
<12:00:04> (Shader) -  Gems/Atom/RPI/Code/Source/Shader.cpp(210): 'Compile'
<12:00:04> (Shader) - Failed to compile shader Materials/Door_18.shader
<12:00:05> (Game) - Could not find asset levels/level9/house_3.prefab
<12:00:06> (Game) - Trace::Assert
<12:00:06> (Game) -  Gem/Source/Level/EndlessLevelComponent.cpp(812): (1234) 'Spawn'
<12:00:06> (Game) - chunk spawned twice
<12:00:07> (Game) - loading chunk 4 5
Segmentation fault (core dumped)
";

#[test]
fn problems_group_and_the_crash_is_found() {
    let r = read(LOG);
    assert!(r.crashed);
    assert_eq!(r.crash_line, Some(15));
    assert!(r.before_crash.last().unwrap().contains("loading chunk"));
    assert_eq!(r.count(Kind::Error), 2);
    assert_eq!(r.count(Kind::Warning), 2);
    assert_eq!(r.count(Kind::Assert), 1);
    let first = &r.problems[0];
    assert_eq!(first.kind, Kind::Assert, "worst first");
    assert_eq!(first.at.as_deref(), Some("Gem/Source/Level/EndlessLevelComponent.cpp(812)"));
    let shader = r.problems.iter().find(|p| p.message.contains("shader")).unwrap();
    assert_eq!(shader.times, 2, "the same problem with a different number is one line");
    assert_eq!(shader.source, "Shader");
    assert_eq!(shader.first_line, 2);
}

#[test]
fn common_problems_get_plain_fixes() {
    let r = read(LOG);
    let hints: Vec<&str> = r.hints.iter().map(|h| h.what.as_str()).collect();
    assert!(hints.iter().any(|h| h.contains("shader")));
    assert!(hints.iter().any(|h| h.contains("missing")));
    let s = r.summary();
    assert!(s.starts_with("It crashed (line 15)."), "{s}");
    assert!(s.contains("(2 times)"));
    assert_eq!(r.to_json()["asserts"], 1);
}

#[test]
fn a_clean_log_says_so() {
    let r = read("<1> (Editor) - all good\n<2> (Editor) - still good\n");
    assert!(!r.crashed && r.problems.is_empty() && r.hints.is_empty());
    assert!(r.summary().contains("0 errors, 0 warnings"));
}
