use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};

use hearth_pack::*;

fn export(root: &Path) -> PathBuf {
    let e = root.join("export");
    let _ = fs::remove_dir_all(&e);
    fs::create_dir_all(e.join("bin")).unwrap();
    fs::create_dir_all(e.join("Cache/linux")).unwrap();
    fs::write(e.join("bin/NoclipProtocol.GameLauncher"), "game").unwrap();
    fs::write(e.join("bin/Editor"), "editor").unwrap();
    fs::write(e.join("bin/libfoo.so.debug"), "dbg").unwrap();
    fs::write(e.join("bin/AssetProcessor"), "ap").unwrap();
    fs::write(e.join("Cache/linux/level.spawnable"), "lvl").unwrap();
    std::os::unix::fs::symlink("NoclipProtocol.GameLauncher", e.join("bin/game-link")).unwrap();
    e
}

fn root(name: &str) -> PathBuf {
    let r = std::env::temp_dir().join(format!("hearth-pack-{name}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&r);
    fs::create_dir_all(&r).unwrap();
    r
}

fn pkg(r: &Path, target: Target) -> Package {
    let hud = r.join("HUD.uicanvas");
    fs::write(&hud, "hud").unwrap();
    Package {
        export: export(r),
        target,
        name: "NoclipProtocol".into(),
        display: "Noclip Protocol".into(),
        version: "0.1".into(),
        out: r.join("packages"),
        icon: None,
        leave_out_editor: true,
        extras: vec![(hud, "ui/hud.uicanvas".into())],
        appimagetool: None,
    }
}

#[test]
fn a_test_archive_leaves_out_the_editor_and_runs() {
    let r = root("test");
    let files = make(&pkg(&r, Target::Test)).unwrap();
    assert_eq!(files[0].file_name().unwrap(), "noclip-protocol-0.1-test.tar.gz");
    let game = r.join("packages/noclip-protocol-0.1-test/noclip-protocol");
    assert!(!game.join("bin/Editor").exists() && !game.join("bin/AssetProcessor").exists() && !game.join("bin/libfoo.so.debug").exists());
    assert!(game.join("Cache/linux/ui/hud.uicanvas").exists(), "extras go into the products folder");
    assert!(fs::symlink_metadata(game.join("bin/game-link")).unwrap().file_type().is_symlink(), "links stay links");
    let run = fs::read_to_string(game.join("run.sh")).unwrap();
    assert!(run.contains("exec \"./bin/NoclipProtocol.GameLauncher\""), "{run}");
    assert!(fs::metadata(game.join("run.sh")).unwrap().permissions().mode() & 0o111 != 0);
    let gz = fs::File::open(&files[0]).unwrap();
    let mut a = tar::Archive::new(flate2::read::GzDecoder::new(gz));
    let names: Vec<String> = a.entries().unwrap().map(|e| e.unwrap().path().unwrap().display().to_string()).collect();
    assert!(names.iter().any(|n| n == "noclip-protocol/run.sh"), "{names:?}");
}

#[test]
fn a_steam_folder_and_a_hearth_os_appimage_with_checksums() {
    let r = root("steam");
    let files = make(&pkg(&r, Target::Steam)).unwrap();
    assert!(files[0].ends_with("Noclip Protocol"));
    assert!(r.join("packages/noclip-protocol-0.1-steam/README-steam.txt").exists());

    // A stand-in appimagetool: copies the AppDir's AppRun to the image path.
    let tool = r.join("appimagetool");
    fs::write(&tool, "#!/bin/sh\n[ \"$1\" = --no-appstream ] || exit 3\n[ -x \"$2/AppRun\" ] || exit 4\ncp \"$2/AppRun\" \"$3\"\n").unwrap();
    fs::set_permissions(&tool, fs::Permissions::from_mode(0o755)).unwrap();
    let mut p = pkg(&r, Target::HearthOs);
    p.appimagetool = Some(tool);
    let files = make(&p).unwrap();
    assert_eq!(files[0].file_name().unwrap(), "Noclip-Protocol-0.1-x86_64.AppImage");
    verify(&files[0], &files[1]).unwrap();
    fs::write(&files[0], "changed").unwrap();
    assert!(verify(&files[0], &files[1]).unwrap_err().contains("doesn't match"));
}

#[test]
fn mistakes_are_explained() {
    let r = root("bad");
    let mut p = pkg(&r, Target::Test);
    p.version = " ".into();
    assert!(make(&p).unwrap_err().contains("owner picks"));
    let mut p = pkg(&r, Target::Test);
    p.export = r.join("nothing");
    assert!(make(&p).unwrap_err().contains("no exported game"));
    let mut p = pkg(&r, Target::Test);
    fs::remove_file(r.join("export/bin/NoclipProtocol.GameLauncher")).unwrap();
    fs::remove_file(r.join("export/bin/game-link")).unwrap();
    p.export = r.join("export");
    assert!(make(&p).unwrap_err().contains("no game launcher"));
    assert!(Target::parse("xbox").is_err());
}
