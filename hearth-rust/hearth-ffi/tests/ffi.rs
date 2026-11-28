use std::ffi::{c_char, c_void, CStr, CString};

use hearth_ffi::*;

extern "C" fn unlock(user: *mut c_void, name: *const c_char, args: *const c_char) -> *const c_char {
    let log = unsafe { &mut *(user as *mut Vec<String>) };
    let name = unsafe { CStr::from_ptr(name) }.to_string_lossy();
    let args = unsafe { CStr::from_ptr(args) }.to_string_lossy();
    log.push(format!("{name} {args}"));
    if args.contains("Vault") {
        return c"{\"error\":\"the vault stays shut\"}".as_ptr();
    }
    c"{\"ok\":true}".as_ptr()
}

fn s(p: *const c_char) -> Option<String> {
    (!p.is_null()).then(|| unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned())
}

#[test]
fn scripts_run_through_the_bridge() {
    let mut log: Vec<String> = Vec::new();
    unsafe {
        let e = hs_new(c"door.hs".as_ptr());
        hs_register(e, c"unlock".as_ptr(), unlock, &mut log as *mut Vec<String> as *mut c_void);
        let src = CString::new("when picked_up(item, n) {\n  let ok = unlock(\"{item} door\", n)\n  say(\"unlocked\", ok)\n}\nwhen vault() { unlock(\"Vault\") }").unwrap();
        assert_eq!(s(hs_load(e, src.as_ptr())), None);
        assert!(hs_listens(e, c"picked_up".as_ptr()));
        assert_eq!(s(hs_fire(e, c"picked_up".as_ptr(), c"[\"Gym\", 2]".as_ptr())), None);
        assert_eq!(s(hs_take_output(e)).unwrap(), "unlocked true");
        let err = s(hs_fire(e, c"vault".as_ptr(), c"[]".as_ptr())).unwrap();
        assert!(err.contains("the vault stays shut") && err.contains("door.hs"), "{err}");
        let bad = CString::new("let x = ").unwrap();
        assert!(s(hs_load(e, bad.as_ptr())).unwrap().contains("line 1"));
        hs_free(e);
    }
    assert_eq!(log, ["unlock [\"Gym door\",2.0]", "unlock [\"Vault\"]"]);
}

#[test]
fn detail_opens_builds_and_selects_by_distance() {
    // A bumpy sheet as an .obj.
    let mut obj = String::new();
    let n = 48;
    for y in 0..=n {
        for x in 0..=n {
            let (fx, fy) = (x as f32 / n as f32, y as f32 / n as f32);
            obj += &format!("v {fx} {fy} {}\nvt {fx} {fy}\n", 0.03 * (fx * 19.0).sin() * (fy * 11.0).cos());
        }
    }
    for y in 0..n {
        for x in 0..n {
            let i = |x: i32, y: i32| y * (n + 1) + x + 1;
            obj += &format!("f {0}/{0} {1}/{1} {2}/{2}\nf {0}/{0} {2}/{2} {3}/{3}\n", i(x, y), i(x + 1, y), i(x + 1, y + 1), i(x, y + 1));
        }
    }
    let dir = std::env::temp_dir().join(format!("hearth-ffi-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("sheet.obj");
    std::fs::write(&path, obj).unwrap();
    let cpath = CString::new(path.to_str().unwrap()).unwrap();
    unsafe {
        assert!(hd_open(c"/nowhere/at/all.glb".as_ptr()).is_null());
        assert!(s(hd_last_error()).unwrap().contains("can't open"));

        let d = hd_open(cpath.as_ptr());
        assert!(!d.is_null(), "{:?}", s(hd_last_error()));
        assert!(dir.join("sheet.hdetail").exists(), "the built levels are saved next to the model");
        let nv = hd_vertices(d, std::ptr::null_mut()) as usize;
        let mut v = vec![0f32; nv * 12];
        hd_vertices(d, v.as_mut_ptr());
        assert!(v.chunks(12).all(|c| (c[8] * c[8] + c[9] * c[9] + c[10] * c[10] - 1.0).abs() < 1e-3), "unit tangents");

        let tris = |d: *mut HdDetail| {
            let mut c = 0u32;
            hd_selected(d, &mut c);
            c / 3
        };
        assert!(hd_select(d, 0.5, 0.5, 0.2, 1.0e6, 1.0));
        let near = tris(d);
        assert!(!hd_select(d, 0.5, 0.5, 0.2, 1.0e6, 1.0), "same view, nothing changed");
        assert!(hd_select(d, 0.5, 0.5, 200.0, 1000.0, 1.0));
        let far = tris(d);
        assert_eq!(near, (n * n * 2) as u32, "close up: every triangle");
        assert!(far * 4 < near, "far away: far fewer ({far} of {near})");
        assert!(s(hd_describe(d)).unwrap().contains("pieces"));
        hd_free(d);

        // Opening again uses the saved file.
        let d = hd_open(cpath.as_ptr());
        assert!(!d.is_null());
        hd_free(d);
    }
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn textures_compress_through_the_bridge() {
    let (w, h) = (8u32, 8u32);
    let src: Vec<u8> = (0..w * h).flat_map(|i| [(i * 4) as u8, (i * 3) as u8, 200, 255]).collect();
    let size = ht_compressed_size(1, w, h) as usize;
    assert_eq!(size, 32);
    assert_eq!(ht_compressed_size(9, w, h), 0);
    let mut out = vec![0u8; size];
    unsafe {
        assert!(ht_compress(1, 1, src.as_ptr(), w, h, w * 4, 2, std::ptr::null(), out.as_mut_ptr(), size as u64));
        assert!(!ht_compress(1, 1, src.as_ptr(), w, h, w * 4, 2, std::ptr::null(), out.as_mut_ptr(), 4), "too little room");
        let mut back = vec![0u8; (w * h * 16) as usize];
        assert!(ht_decompress(1, out.as_ptr(), size as u64, w, h, back.as_mut_ptr(), w * 16, true));
        let first = f32::from_le_bytes([back[8], back[9], back[10], back[11]]);
        assert!((first - 200.0 / 255.0).abs() < 0.05, "blue comes back: {first}");
    }
}
