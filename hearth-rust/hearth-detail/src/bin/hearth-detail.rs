//! `hearth-detail model.glb [-o model.hdetail]`: builds the detail levels for a model and says what came out.

use std::path::PathBuf;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut input = None;
    let mut output = None;
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "-o" => output = it.next().map(PathBuf::from),
            _ => input = Some(PathBuf::from(a)),
        }
    }
    let Some(input) = input else {
        eprintln!("usage: hearth-detail model.glb|model.obj [-o model.hdetail]");
        std::process::exit(2);
    };
    let output = output.unwrap_or_else(|| input.with_extension("hdetail"));
    let started = std::time::Instant::now();
    let result = hearth_detail::load::load(&input).and_then(|m| hearth_detail::build(&m).map(|d| (m, d)));
    let (mesh, detail) = match result {
        Ok(x) => x,
        Err(e) => {
            eprintln!("{}: {e}", input.display());
            std::process::exit(1);
        }
    };
    let roots: usize = detail.clusters.iter().filter(|c| c.parent_error.is_infinite()).map(|c| c.index_count as usize / 3).sum();
    if let Err(e) = std::fs::write(&output, hearth_detail::format::write(&detail)) {
        eprintln!("can't save {}: {e}", output.display());
        std::process::exit(1);
    }
    println!(
        "{}: {} triangles in {} pieces over {} levels; the farthest version has {} triangles. Saved {} ({:.1} s)",
        input.display(),
        mesh.triangle_count(),
        detail.clusters.len(),
        detail.levels,
        roots,
        output.display(),
        started.elapsed().as_secs_f32()
    );
}
