#!/usr/bin/env python3
"""Dolly Forge 전체 파이프라인.

  C      core      → libdollyforge (+ 단위 테스트)
  C++    cpp       → dolly-forge CLI: 레시피 → 스킨 PNG, Bedrock .mcpack
  Java   java      → 3D 미리보기 / 라인업
  Rust   rust      → dolly-guard: 레시피 린트, 무결성 서명·검증
  Python python    → 레퍼런스 팔레트 추출, 바인딩 테스트, 이 스크립트
  C#     csharp    → Dolly Forge Studio (메이크업 GUI) 빌드 + 스크린샷

    python3 DollyForge/build.py            # 전부
    python3 DollyForge/build.py --no-studio
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys

HOME = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HOME)
BUILD = os.path.join(HOME, "build")
SKINS = os.path.join(REPO, "03_Sources", "Minecraft_Skins")
DIST = os.path.join(REPO, "05_Distribution")
CONCEPTS = os.path.join(REPO, "01_Concepts")
COLLECTION = os.path.join(HOME, "characters", "jenny", "collection.json")


def step(title):
    print(f"\n== {title}", flush=True)


def run(*cmd, cwd=HOME, env=None):
    print("  $", " ".join(os.path.relpath(c, HOME) if os.path.isabs(c) and c.startswith(REPO) else c for c in cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True, env=env)


def have(tool):
    return shutil.which(tool) is not None


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def lineup_3d(tiles, out):
    """곡선 3D 렌더를 한 장으로 (Pillow)"""
    from PIL import Image, ImageDraw, ImageFont
    imgs = [Image.open(p).convert("RGB").resize((300, 450)) for p, _ in tiles]
    sheet = Image.new("RGB", (300 * len(imgs), 490), (36, 26, 40))
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("DejaVuSans-Bold.ttf", 16)
    except OSError:
        font = ImageFont.load_default()
    for i, (img, (_, title)) in enumerate(zip(imgs, tiles)):
        sheet.paste(img, (i * 300, 0))
        w = draw.textlength(title, font=font)
        draw.text((i * 300 + (300 - w) / 2, 460), title, fill=(236, 226, 244), font=font)
    sheet.save(out)
    print("  3D lineup ->", os.path.relpath(out, REPO))


def zip_dir(folders, out):
    """같은 입력이면 같은 zip (날짜 고정) — git 기록이 불필요하게 바뀌지 않게"""
    import zipfile
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for folder in folders:
            for f in sorted(os.listdir(folder)):
                info = zipfile.ZipInfo(os.path.basename(folder) + "/" + f, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                with open(os.path.join(folder, f), "rb") as src:
                    z.writestr(info, src.read())
    print("  zip ->", os.path.relpath(out, REPO), "(%.1f MB)" % (os.path.getsize(out) / 1e6))


def recipe_path(entry):
    """collection.json 의 recipe 는 collection.json 이 있는 폴더 기준 상대 경로"""
    return os.path.normpath(os.path.join(os.path.dirname(COLLECTION), entry["recipe"]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-studio", action="store_true", help="C# Studio 빌드를 건너뜀")
    args = ap.parse_args()

    step("C core + C++ CLI (CMake)")
    run("cmake", "-S", HOME, "-B", BUILD, "-DCMAKE_BUILD_TYPE=Release")
    run("cmake", "--build", BUILD, "-j")
    run("ctest", "--test-dir", BUILD, "--output-on-failure")
    cli = os.path.join(BUILD, "dolly-forge")

    step("Python: 레퍼런스 렌더 → 팔레트 (01_Concepts)")
    run(sys.executable, "python/tools/extract_palette.py", "characters/jenny/reference_probes.json",
        "--json", os.path.join(CONCEPTS, "reference_palette.json"),
        "--swatch", os.path.join(CONCEPTS, "reference_palette.png"))

    step("C++: 레시피 → 스킨 PNG (03_Sources/Minecraft_Skins)")
    col = load_json(COLLECTION)
    os.makedirs(SKINS, exist_ok=True)
    skin_files = []
    for s in col["skins"]:
        out = os.path.join(SKINS, s["file"])
        run(cli, "forge", recipe_path(s), "-o", out)
        skin_files.append(out)

    step("Rust: dolly-guard 린트 + 무결성 서명")
    guard = os.path.join(HOME, "rust", "dolly-guard", "target", "release", "dolly-guard")
    run("cargo", "build", "--release", "--offline", "--quiet", cwd=os.path.join(HOME, "rust", "dolly-guard"))
    recipes = sorted(os.path.relpath(p, HOME) for p in glob.glob(os.path.join(HOME, "characters", "*", "*.dolly.json")))
    run(guard, "lint", *recipes)
    integrity = os.path.join(DIST, "integrity.json")
    run(guard, "sign", "--out", integrity, *skin_files)
    run(guard, "verify", "--manifest", integrity, *skin_files)

    step("Java: 3D 미리보기 (05_Distribution/previews)")
    previews = os.path.join(DIST, "previews")
    os.makedirs(previews, exist_ok=True)
    classes = os.path.join(BUILD, "java")
    run("javac", "-d", classes, "java/src/dollyforge/DollyPreview.java")
    titles = []
    for s, path in zip(col["skins"], skin_files):
        meta = load_json(recipe_path(s))["meta"]
        run("java", "-cp", classes, "dollyforge.DollyPreview", "render", path,
            os.path.join(previews, s["file"].replace("_Skin.png", "_Preview.png")), "--scale", "12")
        titles.append(f"{path}:{meta['title']['en_US']}")
    run("java", "-cp", classes, "dollyforge.DollyPreview", "lineup", os.path.join(previews, "Lineup.png"), *titles, "--scale", "8")

    step("C: 곡선 3D 렌더 (05_Distribution/previews/3D)")
    shots = os.path.join(previews, "3D")
    os.makedirs(shots, exist_ok=True)
    tiles = []
    for s, path in zip(col["skins"], skin_files):
        name = s["file"].replace("_Skin.png", "")
        full = os.path.join(shots, name + "_3D.png")
        face = os.path.join(shots, name + "_3D_Face.png")
        run(cli, "sculpt", recipe_path(s), "-o", full, "--size", "600x900", "--yaw", "16", "--hq")
        run(cli, "sculpt", recipe_path(s), "-o", face, "--size", "600x600", "--yaw", "8", "--zoom", "4", "--hq")
        tiles.append((full, load_json(recipe_path(s))["meta"]["title"]["en_US"]))
    lineup_3d(tiles, os.path.join(previews, "Lineup_3D.png"))

    step("C: 곡선 3D 메시 내보내기 (OBJ zip · Figura 아바타)")
    export = os.path.join(BUILD, "export")
    figura = os.path.join(DIST, "figura")
    objs = []
    for s in col["skins"]:
        name = s["file"].replace("_Skin.png", "")
        title = load_json(recipe_path(s))["meta"]["title"]["en_US"]
        obj = os.path.join(export, name, name + ".obj")
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        run(cli, "export-3d", recipe_path(s), "--obj", obj, "--figura", os.path.join(figura, name), "--name", title)
        objs.append(os.path.dirname(obj))
    zip_dir(objs, os.path.join(DIST, "3d", "JennyInUsual_OBJ.zip"))

    step("C++: Bedrock 스킨팩 (05_Distribution/bedrock)")
    bedrock = os.path.join(DIST, "bedrock")
    os.makedirs(bedrock, exist_ok=True)
    pack = {k: col[k] for k in ("id", "name", "description", "version", "header_uuid", "module_uuid")}
    pack["skins"] = []
    for s, path in zip(col["skins"], skin_files):
        meta = load_json(recipe_path(s))["meta"]
        pack["skins"].append({"id": meta["id"], "texture": os.path.relpath(path, bedrock), "name": meta["title"]})
    pack_json = os.path.join(bedrock, "pack.json")
    with open(pack_json, "w", encoding="utf-8") as f:
        f.write("// build.py 가 DollyForge/characters/jenny/collection.json 으로 생성한 파일. 직접 고치지 말 것.\n")
        json.dump(pack, f, ensure_ascii=False, indent=2)
        f.write("\n")
    run(cli, "pack-bedrock", pack_json, "-o", os.path.join(bedrock, col["id"] + ".mcpack"))

    step("Python: 바인딩 + 레시피 회귀 테스트")
    run(sys.executable, "-m", "unittest", "discover", "-s", "python/tests")

    if not args.no_studio and have("dotnet"):
        step("C#: Dolly Forge Studio")
        studio = os.path.join(HOME, "csharp", "DollyForge.Studio")
        run("dotnet", "build", "-c", "Release", "--nologo", "-v", "quiet", cwd=studio)
        dll = os.path.join(studio, "bin", "Release", "net10.0", "dolly-forge-studio.dll")
        run("dotnet", dll, "--screenshot", os.path.join(previews, "Studio.png"), "characters/jenny/jenny_cyber.dolly.json")
    elif not args.no_studio:
        print("\n(dotnet 이 없어 Studio 빌드를 건너뜀)")

    print("\nDone. 스킨:", os.path.relpath(SKINS, REPO), "| 배포:", os.path.relpath(DIST, REPO))


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as e:
        print(f"\nFAILED: {e}", file=sys.stderr)
        sys.exit(1)
