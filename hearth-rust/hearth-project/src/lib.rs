//! Projects and gems, the Rust way (the first part of the engine rewritten: O3DE's `scripts/o3de` Python tools).
//!
//! Where things are found, the same as O3DE: the manifest (`~/.o3de/o3de_manifest.json`) lists engines, projects
//! and outside gem folders; a project names its engine (`project.json` "engine", or a path in
//! `<project>/user/project.json` "engine_path"); gems come from the engine's, the project's and the manifest's
//! `external_subdirectories`, and from gems inside gems.
//!
//! What's safer than before: a gem is only switched on when it, and every gem it needs, can be found for that
//! project's engine, so switching one on can never leave a project that won't open; and project.json is written
//! with Hearth Save (atomic, with a backup).

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use serde_json::{json, Value};

fn read(p: &Path) -> Result<Value, String> {
    let text = fs::read_to_string(p).map_err(|e| format!("can't read {}: {e}", p.display()))?;
    serde_json::from_str(&text).map_err(|e| format!("{} is broken: {e}", p.display()))
}

fn strings(v: &Value) -> Vec<String> {
    v.as_array().into_iter().flatten().filter_map(|x| x.as_str().map(str::to_string).or_else(|| x["path"].as_str().map(str::to_string))).collect()
}

/// A gem name without its version wish ("Atom_RPI>=1.0" → "Atom_RPI").
pub fn bare(name: &str) -> &str {
    let end = name.find(|c: char| "<>=~!".contains(c)).unwrap_or(name.len());
    name[..end].trim()
}

#[derive(Clone, Debug)]
pub struct Gem {
    pub name: String,
    pub path: PathBuf,
    pub needs: Vec<String>,
}

pub struct Home {
    pub manifest: Value,
    pub dir: PathBuf,
}

impl Home {
    /// The `.o3de` folder (default: in the home folder).
    pub fn load(dir: Option<PathBuf>) -> Result<Home, String> {
        let dir = dir.or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".o3de"))).ok_or("can't find the home folder")?;
        let p = dir.join("o3de_manifest.json");
        let manifest = if p.exists() { read(&p)? } else { json!({}) };
        Ok(Home { manifest, dir })
    }

    pub fn engines(&self) -> Vec<PathBuf> {
        strings(&self.manifest["engines"]).into_iter().map(PathBuf::from).collect()
    }
}

/// The engine a project uses: `<project>/user/project.json` "engine_path" first, then the registered engine whose
/// engine.json has the project's "engine" name.
pub fn engine_for(home: &Home, project: &Path) -> Result<PathBuf, String> {
    let pj = read(&project.join("project.json"))?;
    if let Ok(user) = read(&project.join("user/project.json")) {
        if let Some(p) = user["engine_path"].as_str() {
            let p = PathBuf::from(p);
            if p.join("engine.json").is_file() {
                return Ok(p);
            }
            return Err(format!("user/project.json points at an engine at {} that isn't there", p.display()));
        }
    }
    let want = pj["engine"].as_str().unwrap_or("o3de").to_string();
    for e in home.engines() {
        if let Ok(ej) = read(&e.join("engine.json")) {
            if ej["engine_name"].as_str() == Some(want.as_str()) {
                return Ok(e);
            }
        }
    }
    Err(format!("the project wants the engine \"{want}\", and no installed engine has that name (install Hearth Engine, or register it)"))
}

/// Every gem a project can use, by name (the first found wins, as in O3DE: project, then engine, then manifest).
pub fn gems_for(home: &Home, project: &Path) -> Result<BTreeMap<String, Gem>, String> {
    let engine = engine_for(home, project)?;
    let mut dirs: Vec<PathBuf> = Vec::new();
    let pj = read(&project.join("project.json"))?;
    dirs.extend(strings(&pj["external_subdirectories"]).into_iter().map(|s| project.join(s)));
    let ej = read(&engine.join("engine.json"))?;
    dirs.extend(strings(&ej["external_subdirectories"]).into_iter().map(|s| engine.join(s)));
    dirs.extend(strings(&home.manifest["external_subdirectories"]).into_iter().map(PathBuf::from));
    let mut found = BTreeMap::new();
    let mut seen = std::collections::HashSet::new();
    while let Some(d) = (!dirs.is_empty()).then(|| dirs.remove(0)) {
        let key = fs::canonicalize(&d).unwrap_or(d.clone());
        if !seen.insert(key) {
            continue;
        }
        let Ok(gj) = read(&d.join("gem.json")) else { continue };
        let Some(name) = gj["gem_name"].as_str() else { continue };
        let needs = strings(&gj["dependencies"]).iter().map(|n| bare(n).to_string()).collect();
        found.entry(name.to_string()).or_insert(Gem { name: name.to_string(), path: d.clone(), needs });
        // Gems inside gems.
        let inner: Vec<PathBuf> = strings(&gj["external_subdirectories"]).into_iter().map(|s| d.join(s)).collect();
        for (k, p) in inner.into_iter().enumerate() {
            dirs.insert(k, p);
        }
    }
    Ok(found)
}

/// The gem and everything it needs, or what's missing.
pub fn needs_all(gems: &BTreeMap<String, Gem>, name: &str) -> Result<Vec<String>, Vec<String>> {
    let mut out = Vec::new();
    let mut missing = Vec::new();
    let mut todo = vec![name.to_string()];
    while let Some(n) = todo.pop() {
        if out.contains(&n) || missing.contains(&n) {
            continue;
        }
        match gems.get(&n) {
            Some(g) => {
                out.push(n.clone());
                todo.extend(g.needs.iter().cloned());
            }
            None => missing.push(n),
        }
    }
    if missing.is_empty() {
        Ok(out)
    } else {
        Err(missing)
    }
}

pub fn enabled(project: &Path) -> Result<Vec<String>, String> {
    let pj = read(&project.join("project.json"))?;
    Ok(strings(&pj["gem_names"]).iter().map(|n| bare(n).to_string()).collect())
}

fn write_project(project: &Path, pj: &Value) -> Result<(), String> {
    let mut out = Vec::new();
    let fmt = serde_json::ser::PrettyFormatter::with_indent(b"    ");
    let mut ser = serde_json::Serializer::with_formatter(&mut out, fmt);
    serde::Serialize::serialize(pj, &mut ser).map_err(|e| e.to_string())?;
    out.push(b'\n');
    hearth_save::write_atomic(&project.join("project.json"), &out)
}

/// What's wrong with a project's gems, in plain words (empty: it will open).
pub fn check(home: &Home, project: &Path) -> Result<Vec<String>, String> {
    let gems = gems_for(home, project)?;
    let mut problems = Vec::new();
    for g in enabled(project)? {
        match needs_all(&gems, &g) {
            Ok(_) => {}
            Err(missing) if missing.contains(&g) => problems.push(format!("{g} is switched on but this engine doesn't have it")),
            Err(missing) => problems.push(format!("{g} needs {}, which this engine doesn't have", missing.join(", "))),
        }
    }
    Ok(problems)
}

/// Switches a gem on, only if it and everything it needs can be found. Returns what was added.
pub fn enable(home: &Home, project: &Path, name: &str) -> Result<String, String> {
    let gems = gems_for(home, project)?;
    let engine = engine_for(home, project)?;
    let name = bare(name);
    if let Err(missing) = needs_all(&gems, name) {
        return Err(if missing.iter().any(|m| m == name) {
            format!("{name} isn't in the engine at {} (or the project's gems): install a Hearth Engine that has it first. Nothing was changed", engine.display())
        } else {
            format!("{name} needs {}, which the engine doesn't have. Nothing was changed", missing.join(", "))
        });
    }
    let mut pj = read(&project.join("project.json"))?;
    let list = pj.as_object_mut().ok_or("project.json isn't a record")?.entry("gem_names").or_insert(json!([]));
    let names = list.as_array_mut().ok_or("project.json's gem_names isn't a list")?;
    if names.iter().any(|n| n.as_str().map(bare) == Some(name) || n["name"].as_str().map(bare) == Some(name)) {
        return Ok(format!("{name} was already on"));
    }
    // Keep the list in order when it was in order.
    let sorted = names.windows(2).all(|w| w[0].as_str().unwrap_or("").to_lowercase() <= w[1].as_str().unwrap_or("").to_lowercase());
    let at = if sorted { names.iter().position(|n| n.as_str().unwrap_or("").to_lowercase() > name.to_lowercase()).unwrap_or(names.len()) } else { names.len() };
    names.insert(at, json!(name));
    write_project(project, &pj)?;
    Ok(format!("{name} is on"))
}

/// Switches a gem off, unless another gem that's on needs it.
pub fn disable(home: &Home, project: &Path, name: &str) -> Result<String, String> {
    let name = bare(name);
    let gems = gems_for(home, project).unwrap_or_default();
    let on = enabled(project)?;
    let users: Vec<String> = on.iter().filter(|g| g.as_str() != name).filter(|g| needs_all(&gems, g).map(|all| all.iter().any(|n| n == name)).unwrap_or(false)).cloned().collect();
    if !users.is_empty() {
        return Err(format!("{} need {name}: switch them off first. Nothing was changed", users.join(", ")));
    }
    let mut pj = read(&project.join("project.json"))?;
    let Some(names) = pj["gem_names"].as_array_mut() else { return Ok(format!("{name} wasn't on")) };
    let before = names.len();
    names.retain(|n| n.as_str().map(bare) != Some(name) && n["name"].as_str().map(bare) != Some(name));
    if names.len() == before {
        return Ok(format!("{name} wasn't on"));
    }
    write_project(project, &pj)?;
    Ok(format!("{name} is off"))
}
