#!/usr/bin/env python3
"""AI 마감 렌더: Dolly Forge 곡선 3D 렌더를 구도·디자인 그대로 두고, 로컬 ComfyUI(FLUX img2img)로
질감과 디테일을 입혀 레퍼런스 3D 이미지 수준으로 마감한다.

    python3 tools/ai_finish.py ../characters/jenny/jenny_cyber.dolly.json -o out.png
    python3 tools/ai_finish.py <recipe> -o out.png --strength 0.5 --seed 7

- 구조는 엔진 렌더가 정한다(옷 모양·색·포즈). strength 가 낮을수록 엔진 결과에 가깝다.
- 프롬프트는 레시피 extends 체인의 doll.prompt 를 이어 붙인다 + 전 연령 조건.
- ComfyUI 가 꺼져 있으면 ~/ComfyUI 에서 띄우고, 끝나면 다시 끈다 (--keep-server 로 유지).
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import dollyforge as df  # noqa: E402

COMFY_DIR = os.path.expanduser(os.environ.get("COMFYUI_DIR", "~/ComfyUI"))
COMFY = os.environ.get("COMFYUI_URL", "http://127.0.0.1:8188")
CHECKPOINT = os.environ.get("DOLLYFORGE_CHECKPOINT", "flux1-schnell-fp8.safetensors")

SCENE = ("standing in a cozy wooden room with black circuit board wall panels and cyan and magenta neon light strips, "
         "night city skyline window, glossy reflective wooden floor, stylized 3D animated movie render, "
         "soft cinematic studio lighting, subsurface skin, glossy detailed hair strands, detailed fabric, high detail")
SAFE = "adult, fully clothed, modest outfit, wholesome, all ages"


def chain(path, seen=None):
    """레시피 extends 체인을 C 코어와 같은 순서로 (부모들 → 자신)"""
    seen = set() if seen is None else seen
    path = os.path.abspath(path)
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    out = []
    ext = doc.get("extends")
    for rel in ([ext] if isinstance(ext, str) else ext or []):
        p = os.path.abspath(os.path.join(os.path.dirname(path), rel))
        if p not in seen:
            seen.add(p)
            out += chain(p, seen)
    out.append(doc)
    return out


def prompt_for(recipe):
    """doll.prompt 의 character / pose / outfit 슬롯을 팔레트처럼 합친다 (뒤에 오는 레시피가 덮어씀)"""
    slots = {}
    for d in chain(recipe):
        slots.update(d.get("doll", {}).get("prompt", {}) or {})
    parts = [slots.get(k) for k in ("character", "outfit", "pose") if slots.get(k)]
    if not parts:
        parts = [chain(recipe)[-1].get("meta", {}).get("title", {}).get("en_US", "stylized 3D character")]
    return ", ".join(parts + [SCENE, SAFE])


def http(method, path, data=None, headers=None, timeout=30):
    req = urllib.request.Request(COMFY + path, data=data, method=method, headers=headers or {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def server_up():
    try:
        http("GET", "/system_stats", timeout=2)
        return True
    except Exception:
        return False


def start_server(log_path):
    py = os.path.join(COMFY_DIR, ".venv", "bin", "python")
    if not os.path.exists(py):
        raise SystemExit(f"ComfyUI not found at {COMFY_DIR} (set COMFYUI_DIR)")
    log = open(log_path, "ab")
    proc = subprocess.Popen([py, "main.py", "--listen", "127.0.0.1", "--port", "8188"], cwd=COMFY_DIR,
                            stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    for _ in range(180):
        if server_up():
            return proc
        if proc.poll() is not None:
            raise SystemExit(f"ComfyUI exited early, see {log_path}")
        time.sleep(1)
    proc.terminate()
    raise SystemExit("ComfyUI did not start in 180 s")


def upload(png_path):
    boundary = uuid.uuid4().hex
    name = os.path.basename(png_path)
    with open(png_path, "rb") as f:
        body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"image\"; filename=\"{name}\"\r\n"
                f"Content-Type: image/png\r\n\r\n").encode() + f.read() + \
               f"\r\n--{boundary}\r\nContent-Disposition: form-data; name=\"overwrite\"\r\n\r\ntrue\r\n--{boundary}--\r\n".encode()
    r = json.loads(http("POST", "/upload/image", body, {"Content-Type": f"multipart/form-data; boundary={boundary}"}))
    return r["name"]


def workflow(image_name, prompt, seed, strength, steps):
    return {
        "1": {"class_type": "CheckpointLoaderSimple", "inputs": {"ckpt_name": CHECKPOINT}},
        "2": {"class_type": "LoadImage", "inputs": {"image": image_name}},
        "3": {"class_type": "VAEEncode", "inputs": {"pixels": ["2", 0], "vae": ["1", 2]}},
        "4": {"class_type": "CLIPTextEncode", "inputs": {"text": prompt, "clip": ["1", 1]}},
        "5": {"class_type": "CLIPTextEncode", "inputs": {"text": "", "clip": ["1", 1]}},
        "6": {"class_type": "KSampler", "inputs": {"model": ["1", 0], "seed": seed, "steps": steps, "cfg": 1.0,
                                                    "sampler_name": "euler", "scheduler": "simple",
                                                    "positive": ["4", 0], "negative": ["5", 0],
                                                    "latent_image": ["3", 0], "denoise": strength}},
        "7": {"class_type": "VAEDecode", "inputs": {"samples": ["6", 0], "vae": ["1", 2]}},
        "8": {"class_type": "SaveImage", "inputs": {"images": ["7", 0], "filename_prefix": "dollyforge_finish"}},
    }


def run(wf):
    pid = json.loads(http("POST", "/prompt", json.dumps({"prompt": wf}).encode(), {"Content-Type": "application/json"}))["prompt_id"]
    for _ in range(600):
        hist = json.loads(http("GET", f"/history/{pid}"))
        if pid in hist:
            entry = hist[pid]
            if entry.get("status", {}).get("status_str") == "error":
                raise SystemExit("ComfyUI error: " + json.dumps(entry.get("status"))[:800])
            for node in entry.get("outputs", {}).values():
                for img in node.get("images", []):
                    q = urllib.parse.urlencode({"filename": img["filename"], "subfolder": img["subfolder"], "type": img["type"]})
                    return http("GET", "/view?" + q, timeout=60)
        time.sleep(0.5)
    raise SystemExit("timed out waiting for ComfyUI")


def finish(recipes_out, strength=0.72, steps=6, keep_server=False, log_path="/tmp/dollyforge_comfyui.log", base_dir=None):
    """recipes_out: [(recipe_path, out_png)]. 엔진 렌더 → AI 마감"""
    started = None
    if not server_up():
        print("starting ComfyUI ...", flush=True)
        started = start_server(log_path)
    try:
        for recipe, out in recipes_out:
            tmp = os.path.join(base_dir or os.path.dirname(os.path.abspath(out)), "_engine_" + os.path.basename(out))
            px = df.doll(recipe).render(832, 1248, yaw=12, pitch=-4, zoom=1, background=True, hq=True)
            from PIL import Image
            Image.frombytes("RGBA", (832, 1248), px).convert("RGB").save(tmp)
            seed = int(hashlib.sha256(os.path.basename(recipe).encode()).hexdigest()[:8], 16)
            t = time.time()
            data = run(workflow(upload(tmp), prompt_for(recipe), seed, strength, steps))
            with open(out, "wb") as f:
                f.write(data)
            os.remove(tmp)
            print(f"  {os.path.basename(recipe)} -> {out} ({time.time() - t:.1f}s)", flush=True)
    finally:
        if started and not keep_server:
            started.terminate()
            try:
                started.wait(20)
            except subprocess.TimeoutExpired:
                started.kill()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("recipe")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--strength", type=float, default=0.72, help="0.3 = 엔진 그대로에 가깝게, 0.7 = 더 많이 다시 그림")
    ap.add_argument("--steps", type=int, default=6)
    ap.add_argument("--keep-server", action="store_true")
    ap.add_argument("--print-prompt", action="store_true")
    a = ap.parse_args()
    if a.print_prompt:
        print(prompt_for(a.recipe))
        return
    finish([(a.recipe, a.out)], a.strength, a.steps, a.keep_server)


if __name__ == "__main__":
    main()
