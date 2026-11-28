//! Package and Share: turns an exported game folder into what it's for (a Rust port of the game's
//! `Tools/HearthPackage/finish_package.py`, made general):
//!
//! - `hearthos`: an AppImage plus SHA256SUMS.txt (the Hearth Store installs AppImages from a release and checks them);
//! - `steam`: a folder ready to be a Steam depot (Linux), with a launch script;
//! - `test`: a .tar.gz of a folder to try on another device.
//!
//! Files a game reads at run time from its products folder (its HUD, its monster list) are given as extras
//! (`--extra UI/HUD.uicanvas=ui/hud.uicanvas`) instead of being written into the tool.

use std::fs;
use std::io::Read;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::Command;

use sha2::{Digest, Sha256};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Target {
    HearthOs,
    Steam,
    Test,
}

impl Target {
    pub fn parse(s: &str) -> Result<Target, String> {
        match s {
            "hearthos" => Ok(Target::HearthOs),
            "steam" => Ok(Target::Steam),
            "test" => Ok(Target::Test),
            _ => Err(format!("{s} isn't a target (hearthos, steam or test)")),
        }
    }
    fn word(self) -> &'static str {
        match self {
            Target::HearthOs => "hearthos",
            Target::Steam => "steam",
            Target::Test => "test",
        }
    }
}

#[derive(Clone, Debug)]
pub struct Package {
    pub export: PathBuf,
    pub target: Target,
    /// The project's name (finds `<name>.GameLauncher`), and the name people see.
    pub name: String,
    pub display: String,
    /// Picked by the owner, never by this tool.
    pub version: String,
    pub out: PathBuf,
    pub icon: Option<PathBuf>,
    pub leave_out_editor: bool,
    /// (file on this computer, where it goes under the products folder).
    pub extras: Vec<(PathBuf, String)>,
    /// The AppImage maker (default: $APPIMAGETOOL, then appimagetool on the PATH).
    pub appimagetool: Option<PathBuf>,
}

const EDITOR_ONLY: [&str; 6] = ["Editor", "AssetProcessor", "AssetBuilder", "AssetBundler", "SerializeContextTools", "PythonBindingsExample"];

pub fn slug(display: &str) -> String {
    display.to_lowercase().split_whitespace().collect::<Vec<_>>().join("-")
}

fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else { return };
    let mut entries: Vec<_> = entries.flatten().collect();
    entries.sort_by_key(|e| e.file_name());
    for e in entries {
        let p = e.path();
        let is_dir = e.file_type().map(|t| t.is_dir()).unwrap_or(false);
        out.push(p.clone());
        if is_dir {
            walk(&p, out);
        }
    }
}

/// Copies a folder, keeping links as links.
pub fn copy_tree(from: &Path, to: &Path) -> std::io::Result<()> {
    fs::create_dir_all(to)?;
    for e in fs::read_dir(from)? {
        let e = e?;
        let t = e.file_type()?;
        let dest = to.join(e.file_name());
        if t.is_symlink() {
            let target = fs::read_link(e.path())?;
            let _ = fs::remove_file(&dest);
            std::os::unix::fs::symlink(target, &dest)?;
        } else if t.is_dir() {
            copy_tree(&e.path(), &dest)?;
        } else {
            fs::copy(e.path(), &dest)?;
        }
    }
    Ok(())
}

pub fn find_launcher(folder: &Path, name: &str) -> Option<PathBuf> {
    let mut all = Vec::new();
    walk(folder, &mut all);
    let named = format!("{name}.GameLauncher");
    all.iter().find(|p| p.is_file() && p.file_name().map(|n| n == named.as_str()).unwrap_or(false)).cloned().or_else(|| {
        all.into_iter().find(|p| p.is_file() && p.file_name().and_then(|n| n.to_str()).map(|n| n.ends_with(".GameLauncher")).unwrap_or(false))
    })
}

/// Removes editor-only programs and debug symbols. Returns how many files went.
pub fn leave_out_editor(folder: &Path) -> usize {
    let mut all = Vec::new();
    walk(folder, &mut all);
    let mut removed = 0;
    for p in all {
        if !p.is_file() {
            continue;
        }
        let name = p.file_name().and_then(|n| n.to_str()).unwrap_or("");
        let debug = name.ends_with(".pdb") || name.ends_with(".debug") || name.ends_with(".dbg");
        let editor = EDITOR_ONLY.iter().any(|part| name.starts_with(part) || name.contains(&format!(".{part}.")));
        if (debug || editor) && fs::remove_file(&p).is_ok() {
            removed += 1;
        }
    }
    removed
}

fn executable(p: &Path) -> std::io::Result<()> {
    let mut perm = fs::metadata(p)?.permissions();
    perm.set_mode(perm.mode() | 0o111);
    fs::set_permissions(p, perm)
}

pub fn sha256(p: &Path) -> std::io::Result<String> {
    let mut f = fs::File::open(p)?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(h.finalize().iter().map(|b| format!("{b:02x}")).collect())
}

fn products_folders(game: &Path) -> Vec<PathBuf> {
    let mut all = Vec::new();
    walk(game, &mut all);
    let found: Vec<PathBuf> = all
        .into_iter()
        .filter(|p| p.is_dir() && p.file_name().map(|n| n == "linux").unwrap_or(false))
        .filter(|p| p.parent().and_then(|q| q.file_name()).and_then(|n| n.to_str()).map(|n| n.eq_ignore_ascii_case("cache")).unwrap_or(false))
        .collect();
    if found.is_empty() {
        vec![game.to_path_buf()]
    } else {
        found
    }
}

fn io(what: &'static str) -> impl Fn(std::io::Error) -> String {
    move |e| format!("{what}: {e}")
}

/// Makes the package. Returns the files made (the .tar.gz, the folder, or the AppImage and its SHA256SUMS.txt).
pub fn make(p: &Package) -> Result<Vec<PathBuf>, String> {
    if p.version.trim().is_empty() {
        return Err("a version is needed (the owner picks it)".into());
    }
    let export = fs::canonicalize(&p.export).map_err(|_| format!("there's no exported game at {}", p.export.display()))?;
    if !export.is_dir() {
        return Err(format!("there's no exported game at {}", export.display()));
    }
    fs::create_dir_all(&p.out).map_err(io("can't make the output folder"))?;
    let out = fs::canonicalize(&p.out).map_err(io("can't find the output folder"))?;
    if out.starts_with(&export) {
        return Err("the output folder can't be inside the exported game".into());
    }
    let slug = slug(&p.display);
    let work = out.join(format!("{slug}-{}-{}", p.version, p.target.word()));
    let _ = fs::remove_dir_all(&work);
    let game = work.join(if p.target == Target::Steam { p.display.clone() } else { slug.clone() });
    copy_tree(&export, &game).map_err(io("can't copy the game"))?;
    if p.leave_out_editor {
        println!("left out {} editor-only files", leave_out_editor(&game));
    }
    for (from, to) in &p.extras {
        if !from.is_file() {
            return Err(format!("{} isn't there (an extra file for the package)", from.display()));
        }
        for folder in products_folders(&game) {
            let dest = folder.join(to);
            if let Some(d) = dest.parent() {
                fs::create_dir_all(d).map_err(io("can't make a folder"))?;
            }
            fs::copy(from, &dest).map_err(io("can't copy an extra file"))?;
        }
    }
    let launcher = find_launcher(&game, &p.name).ok_or("the export has no game launcher")?;
    executable(&launcher).map_err(io("can't make the launcher runnable"))?;
    let rel = launcher.strip_prefix(&game).unwrap_or(&launcher).display().to_string();
    let run = game.join("run.sh");
    fs::write(&run, format!("#!/bin/sh\n# {} {}\ncd \"$(dirname \"$0\")\"\nexec \"./{rel}\" \"$@\"\n", p.display, p.version)).map_err(io("can't write run.sh"))?;
    executable(&run).map_err(io("can't make run.sh runnable"))?;

    match p.target {
        Target::Test => {
            let archive = out.join(format!("{slug}-{}-test.tar.gz", p.version));
            let f = fs::File::create(&archive).map_err(io("can't make the archive"))?;
            let gz = flate2::write::GzEncoder::new(f, flate2::Compression::default());
            let mut tar = tar::Builder::new(gz);
            tar.follow_symlinks(false);
            tar.append_dir_all(&slug, &game).map_err(io("can't fill the archive"))?;
            tar.into_inner().and_then(|gz| gz.finish()).map_err(io("can't finish the archive"))?;
            Ok(vec![archive])
        }
        Target::Steam => {
            fs::write(work.join("README-steam.txt"), format!("{} {} for Steam (Linux).\n\nUpload the folder '{}' as the Linux depot. Launch option: run.sh\n", p.display, p.version, p.display))
                .map_err(io("can't write the Steam note"))?;
            Ok(vec![game])
        }
        Target::HearthOs => {
            let appdir = work.join(format!("{}.AppDir", p.display));
            fs::create_dir_all(&appdir).map_err(io("can't make the AppDir"))?;
            fs::rename(&game, appdir.join("game")).map_err(io("can't move the game into the AppDir"))?;
            let apprun = appdir.join("AppRun");
            fs::write(&apprun, "#!/bin/sh\nexec \"$(dirname \"$(readlink -f \"$0\")\")/game/run.sh\" \"$@\"\n").map_err(io("can't write AppRun"))?;
            executable(&apprun).map_err(io("can't make AppRun runnable"))?;
            fs::write(appdir.join(format!("{slug}.desktop")), format!("[Desktop Entry]\nType=Application\nName={}\nExec=AppRun\nIcon={slug}\nCategories=Game;\n", p.display))
                .map_err(io("can't write the .desktop file"))?;
            if let Some(icon) = p.icon.as_ref().filter(|i| i.is_file()) {
                fs::copy(icon, appdir.join(format!("{slug}.png"))).map_err(io("can't copy the icon"))?;
            }
            let tool = p
                .appimagetool
                .clone()
                .or_else(|| std::env::var_os("APPIMAGETOOL").map(PathBuf::from))
                .or_else(|| which("appimagetool"))
                .or_else(|| which("appimagetool-x86_64.AppImage"))
                .ok_or("appimagetool isn't on this computer (GitHub packaging has it)")?;
            let image = out.join(format!("{}-{}-x86_64.AppImage", p.display.replace(' ', "-"), p.version));
            let status = Command::new(&tool)
                .arg("--no-appstream")
                .arg(&appdir)
                .arg(&image)
                .env("ARCH", "x86_64")
                .env("APPIMAGE_EXTRACT_AND_RUN", "1")
                .status()
                .map_err(io("can't run appimagetool"))?;
            if !status.success() || !image.is_file() {
                return Err(format!("appimagetool failed ({status})"));
            }
            let sums = out.join("SHA256SUMS.txt");
            let name = image.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
            fs::write(&sums, format!("{}  {name}\n", sha256(&image).map_err(io("can't check the AppImage"))?)).map_err(io("can't write SHA256SUMS.txt"))?;
            Ok(vec![image, sums])
        }
    }
}

fn which(name: &str) -> Option<PathBuf> {
    std::env::var_os("PATH").and_then(|paths| std::env::split_paths(&paths).map(|d| d.join(name)).find(|p| p.is_file()))
}

/// Checks a downloaded file against a SHA256SUMS.txt (what the Hearth Store does before installing).
pub fn verify(file: &Path, sums: &Path) -> Result<(), String> {
    let text = fs::read_to_string(sums).map_err(|e| format!("can't read {}: {e}", sums.display()))?;
    let name = file.file_name().and_then(|n| n.to_str()).unwrap_or("");
    let want = text
        .lines()
        .filter_map(|l| l.split_once("  "))
        .find(|(_, n)| n.trim() == name)
        .map(|(h, _)| h.trim().to_lowercase())
        .ok_or_else(|| format!("{name} isn't listed in {}", sums.display()))?;
    let have = sha256(file).map_err(|e| format!("can't read {}: {e}", file.display()))?;
    if have == want {
        Ok(())
    } else {
        Err(format!("{name} doesn't match its checksum: the download is damaged or was changed"))
    }
}
