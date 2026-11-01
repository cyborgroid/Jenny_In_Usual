//! dolly-guard — 공식 스킨 서명·무단 변형 감지·레시피 라이선스 검사
//!
//!   dolly-guard sign   --out integrity.json <skin.png>...
//!   dolly-guard verify --manifest integrity.json <skin.png>...
//!   dolly-guard lint   <recipe.dolly.json>...
mod ffi;
mod sha256;

use ffi::{Json, Skin, PART_COUNT};
use sha256::{hex, sha256};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

/// 배포 에셋이 따라야 하는 라이선스 (05_Distribution/LICENSE.md)
const LICENSE: &str = "CC-BY-NC-ND-4.0";
const RATING: &str = "all-ages";

fn json_str(s: &str) -> String {
    let mut o = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            '\n' => o.push_str("\\n"),
            c if (c as u32) < 0x20 => o.push_str(&format!("\\u{:04x}", c as u32)),
            c => o.push(c),
        }
    }
    o.push('"');
    o
}

fn file_name(p: &str) -> String {
    Path::new(p).file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| p.into())
}

/// manifest 기준 상대 경로 (같은 디렉터리 트리 안이면)
fn relative_to(path: &Path, base: &Path) -> String {
    let abs = |p: &Path| std::fs::canonicalize(p).unwrap_or_else(|_| p.to_path_buf());
    let (p, b) = (abs(path), abs(base));
    let pc: Vec<_> = p.components().collect();
    let bc: Vec<_> = b.components().collect();
    let common = pc.iter().zip(&bc).take_while(|(a, b)| a == b).count();
    let mut rel = PathBuf::new();
    for _ in common..bc.len() {
        rel.push("..");
    }
    for c in &pc[common..] {
        rel.push(c);
    }
    rel.to_string_lossy().replace('\\', "/")
}

struct Entry {
    name: String,
    path: String,
    sha256_file: String,
    sha256_pixels: String,
}

fn sign(out: &str, skins: &[String]) -> Result<(), String> {
    let base = Path::new(out).parent().filter(|p| !p.as_os_str().is_empty()).unwrap_or(Path::new("."));
    let mut entries = Vec::new();
    for path in skins {
        let skin = Skin::load(path).map_err(|e| format!("{path}: {e}"))?;
        let (errors, report) = skin.validate();
        if errors > 0 {
            return Err(format!("{path} is not a valid skin, refusing to sign:\n{report}"));
        }
        let bytes = std::fs::read(path).map_err(|e| format!("{path}: {e}"))?;
        entries.push((
            Entry {
                name: file_name(path),
                path: relative_to(Path::new(path), base),
                sha256_file: hex(&sha256(&bytes)),
                sha256_pixels: hex(&sha256(&skin.canonical_pixels())),
            },
            skin.slim(),
        ));
    }
    let mut json = String::new();
    json.push_str("{\n  \"tool\": \"dolly-guard 0.1.0\",\n");
    json.push_str(&format!("  \"license\": {},\n", json_str(LICENSE)));
    json.push_str("  \"note\": \"sha256_pixels 는 디코딩한 RGBA(투명 픽셀 정규화) + 모델 바이트의 해시. 다시 저장해도 그림이 같으면 일치한다.\",\n");
    json.push_str("  \"skins\": [\n");
    for (i, (e, slim)) in entries.iter().enumerate() {
        json.push_str(&format!(
            "    {{ \"name\": {}, \"path\": {}, \"model\": {}, \"sha256_file\": {}, \"sha256_pixels\": {} }}{}\n",
            json_str(&e.name),
            json_str(&e.path),
            json_str(if *slim { "slim" } else { "classic" }),
            json_str(&e.sha256_file),
            json_str(&e.sha256_pixels),
            if i + 1 < entries.len() { "," } else { "" }
        ));
    }
    json.push_str("  ]\n}\n");
    std::fs::write(out, json).map_err(|e| format!("{out}: {e}"))?;
    println!("signed {} skin(s) -> {out}", entries.len());
    Ok(())
}

fn load_manifest(path: &str) -> Result<(PathBuf, Vec<Entry>), String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("{path}: {e}"))?;
    let doc = Json::parse(&text).map_err(|e| format!("{path}: {e}"))?;
    let skins = doc.root().get("skins").ok_or(format!("{path}: missing 'skins'"))?;
    let mut out = Vec::new();
    for s in skins.items() {
        let field = |k: &str| s.get(k).and_then(|v| v.str()).map(String::from).ok_or(format!("{path}: skin entry missing '{k}'"));
        out.push(Entry {
            name: field("name")?,
            path: field("path")?,
            sha256_file: field("sha256_file")?,
            sha256_pixels: field("sha256_pixels")?,
        });
    }
    let base = Path::new(path).parent().filter(|p| !p.as_os_str().is_empty()).unwrap_or(Path::new(".")).to_path_buf();
    Ok((base, out))
}

/// 반환: 공식 스킨과 동일하면 true
fn verify_one(path: &str, base: &Path, manifest: &[Entry]) -> Result<bool, String> {
    let bytes = std::fs::read(path).map_err(|e| format!("{path}: {e}"))?;
    let skin = Skin::load(path).map_err(|e| format!("{path}: {e}"))?;
    let file_hash = hex(&sha256(&bytes));
    let pixel_hash = hex(&sha256(&skin.canonical_pixels()));
    if let Some(e) = manifest.iter().find(|e| e.sha256_file == file_hash) {
        println!("{path}: OFFICIAL — identical file to {}", e.name);
        return Ok(true);
    }
    if let Some(e) = manifest.iter().find(|e| e.sha256_pixels == pixel_hash) {
        println!("{path}: OFFICIAL — same pixels as {} (re-saved file)", e.name);
        return Ok(true);
    }
    // 가장 가까운 공식 스킨과 비교해 어디가 바뀌었는지 보여준다
    let mut best: Option<(i32, [i32; PART_COUNT], &Entry)> = None;
    for e in manifest {
        let official = match Skin::load(&base.join(&e.path).to_string_lossy()) {
            Ok(s) => s,
            Err(_) => continue,
        };
        let (total, parts) = official.diff(&skin);
        if best.as_ref().map_or(true, |b| total < b.0) {
            best = Some((total, parts, e));
        }
    }
    match best {
        Some((total, parts, e)) if total < 64 * 64 / 2 => {
            let changed: Vec<String> = (0..PART_COUNT)
                .filter(|&p| parts[p] > 0)
                .map(|p| format!("{} {}px", ffi::part_name(p), parts[p]))
                .collect();
            println!("{path}: MODIFIED — derived from {} with {total} pixel(s) changed ({})", e.name, changed.join(", "));
            println!("  CC BY-NC-ND 4.0 does not allow distributing modified versions of official skins.");
        }
        _ => println!("{path}: UNKNOWN — not an official Jenny In Usual skin"),
    }
    Ok(false)
}

fn verify(manifest_path: &str, skins: &[String]) -> Result<bool, String> {
    let (base, manifest) = load_manifest(manifest_path)?;
    let mut all_ok = true;
    for s in skins {
        all_ok &= verify_one(s, &base, &manifest)?;
    }
    Ok(all_ok)
}

/// 레시피 메타데이터·라이선스·연령 등급 + 실제로 유효한 스킨이 나오는지 검사
fn lint(recipes: &[String]) -> Result<bool, String> {
    let mut ok = true;
    for path in recipes {
        let text = std::fs::read_to_string(path).map_err(|e| format!("{path}: {e}"))?;
        let doc = Json::parse(&text).map_err(|e| format!("{path}: {e}"))?;
        let root = doc.root();
        let mut problems = Vec::new();
        let meta = root.get("meta").filter(|m| m.is_object());
        let meta_str = |k: &str| meta.and_then(|m| m.get(k)).and_then(|v| v.str()).map(String::from);
        let is_abstract = meta.and_then(|m| m.get("abstract")).map_or(false, |v| v.truthy());
        if meta.is_none() {
            problems.push("missing 'meta' object".to_string());
        }
        for key in ["id", "author", "license", "rating"] {
            if meta.is_some() && meta_str(key).is_none() {
                problems.push(format!("meta.{key} is required"));
            }
        }
        if let Some(l) = meta_str("license") {
            if l != LICENSE {
                problems.push(format!("meta.license is '{l}', distributed assets must be '{LICENSE}'"));
            }
        }
        if let Some(r) = meta_str("rating") {
            if r != RATING {
                problems.push(format!("meta.rating is '{r}', Project Haven assets must be '{RATING}'"));
            }
        }
        if !is_abstract {
            let title = meta.and_then(|m| m.get("title")).and_then(|t| t.get("en_US")).and_then(|v| v.str());
            if title.is_none() {
                problems.push("meta.title.en_US is required (used as the in-game skin name)".into());
            }
            match Skin::forge(path) {
                Ok(skin) => {
                    let (errors, report) = skin.validate();
                    if errors > 0 {
                        problems.push(format!("forged skin is invalid:\n{report}"));
                    }
                }
                Err(e) => problems.push(format!("forge failed: {e}")),
            }
        }
        if problems.is_empty() {
            println!("{path}: ok{}", if is_abstract { " (abstract base)" } else { "" });
        } else {
            ok = false;
            println!("{path}:");
            for p in problems {
                println!("  - {p}");
            }
        }
    }
    Ok(ok)
}

fn usage() -> ExitCode {
    eprintln!("usage: dolly-guard sign --out <integrity.json> <skin.png>...");
    eprintln!("       dolly-guard verify --manifest <integrity.json> <skin.png>...");
    eprintln!("       dolly-guard lint <recipe.dolly.json>...");
    ExitCode::from(2)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(cmd) = args.first() else { return usage() };
    let mut flag = None;
    let mut rest = Vec::new();
    let mut it = args[1..].iter();
    while let Some(a) = it.next() {
        if a == "--out" || a == "--manifest" {
            flag = it.next().cloned();
        } else {
            rest.push(a.clone());
        }
    }
    let result = match (cmd.as_str(), flag) {
        ("sign", Some(out)) if !rest.is_empty() => sign(&out, &rest).map(|_| true),
        ("verify", Some(m)) if !rest.is_empty() => verify(&m, &rest),
        ("lint", None) if !rest.is_empty() => lint(&rest),
        _ => return usage(),
    };
    match result {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::from(1),
        Err(e) => {
            eprintln!("dolly-guard: {e}");
            ExitCode::from(1)
        }
    }
}
