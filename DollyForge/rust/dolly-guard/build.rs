// C 코어(libdollyforge)를 시스템 C 컴파일러로 직접 빌드해 정적으로 링크한다.
// 외부 crate 없이 오프라인에서도 빌드된다.
use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let core = manifest.join("../../core");
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let cc = env::var("CC").unwrap_or_else(|_| "cc".into());
    let mut objects = Vec::new();
    for name in ["json", "png", "skin", "forge"] {
        let src = core.join("src").join(format!("{name}.c"));
        let obj = out.join(format!("{name}.o"));
        let status = Command::new(&cc)
            .args(["-std=c99", "-O2", "-fPIC", "-Werror=implicit-function-declaration", "-c"])
            .arg("-I").arg(core.join("include"))
            .arg(&src).arg("-o").arg(&obj)
            .status()
            .expect("failed to run C compiler");
        assert!(status.success(), "compiling {} failed", src.display());
        println!("cargo:rerun-if-changed={}", src.display());
        objects.push(obj);
    }
    let lib = out.join("libdollyforge.a");
    let _ = std::fs::remove_file(&lib);
    let status = Command::new("ar").arg("crs").arg(&lib).args(&objects).status().expect("failed to run ar");
    assert!(status.success(), "ar failed");
    println!("cargo:rerun-if-changed={}", core.join("include").display());
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=dollyforge");
}
