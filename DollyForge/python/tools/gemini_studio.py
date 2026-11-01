#!/usr/bin/env python3
"""Dolly Forge 편집 작업실 — Gemini(나노 바나나) 이미지 생성·편집과 사용자 직접 편집.

AI 가 한 번 그려 주고 끝나는 것이 아니라, 사용자가 세션 안에서 계속 고친다:

    gemini_studio.py doctor [--live]                         호환성 점검
    gemini_studio.py new  S --recipe R.dolly.json            엔진 렌더(현재 디자인)로 시작
    gemini_studio.py new  S --image my_gemini_image.png      Gemini 앱 등에서 만든 이미지로 시작
    gemini_studio.py edit S "코트 단추를 은색으로"            전체 편집 (Gemini 대화를 이어 감)
    gemini_studio.py edit S "꽃핀을 더 크게" --region 300,40,220,200
                                                             그 영역만 편집, 나머지 픽셀은 그대로
    gemini_studio.py edit S "..." --backend flux             로컬 FLUX (API 키 없이)
    gemini_studio.py adjust S --brightness 1.05 --saturation 1.1 --warmth 0.03
                                                             AI 없이 직접 조정
    gemini_studio.py undo|redo S / goto S N / log S / export S -o out.png

세션은 폴더 하나: session.json + 단계별 PNG. 모든 단계가 남아 언제든 되돌리고 갈라 갈 수 있다.
API 키는 Gemini CLI 와 같은 곳에서 찾는다: GEMINI_API_KEY / GOOGLE_API_KEY 환경 변수, ~/.gemini/.env.
"""
import argparse
import base64
import io
import json
import os
import re
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, HERE)

from PIL import Image, ImageEnhance, ImageFilter  # noqa: E402

DF_HOME = os.path.normpath(os.path.join(HERE, "..", ".."))
REPO = os.path.dirname(DF_HOME)
SESSIONS = os.path.join(DF_HOME, "build", "sessions")

# 2026-11 공식 문서 기준 이미지 모델 (앞쪽이 우선). doctor 가 실제 목록과 대조한다.
MODEL_PREFERENCE = [
    "gemini-nano-banana-2.1",      # Nano Banana 2.1 — 생성·편집 주력
    "gemini-3.1-flash-image",      # Nano Banana 2
    "gemini-3-pro-image",          # Nano Banana Pro — 복잡한 장면
    "gemini-3.1-flash-lite-image", # Nano Banana 2 Lite — 빠르고 저렴 (1K 까지)
    "gemini-2.5-flash-image",      # Nano Banana (구형)
]
DEFAULT_MODEL = os.environ.get("DOLLYFORGE_GEMINI_MODEL", MODEL_PREFERENCE[0])

GUARD = ("Keep the same characters, faces, hairstyle, outfit design, colors and pose unless the instruction asks to change them. "
         "Keep the stylized 3D animated look and lighting. The characters are adults; keep them fully clothed and the image "
         "wholesome and all-ages.")
MAX_REF_SIDE = 1024
MAX_INPUT_SIDE = 2048


# ---------------- API 키 · 클라이언트 ----------------

def find_api_key():
    """(키, 출처) — Gemini CLI 와 같은 순서"""
    for var in ("GEMINI_API_KEY", "GOOGLE_API_KEY"):
        if os.environ.get(var):
            return os.environ[var], f"env {var}"
    for path in (os.path.expanduser("~/.gemini/.env"), os.path.join(DF_HOME, ".env")):
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line.startswith("export "):
                        line = line[7:]
                    for var in ("GEMINI_API_KEY", "GOOGLE_API_KEY"):
                        if line.startswith(var + "="):
                            val = line.split("=", 1)[1].strip().strip('"').strip("'")
                            if val:
                                return val, path
    return None, None


def make_client():
    """google-genai 클라이언트. 테스트는 이 함수를 가짜로 바꿔 끼운다"""
    from google import genai
    key, _ = find_api_key()
    if not key:
        raise SystemExit("Gemini API key not found. Set GEMINI_API_KEY (or put it in ~/.gemini/.env, like Gemini CLI). "
                         "Or use --backend flux for local editing.")
    return genai.Client(api_key=key)


# ---------------- 이미지 유틸 ----------------

def png_bytes(img):
    buf = io.BytesIO()
    img.save(buf, "PNG")
    return buf.getvalue()


def limit(img, side):
    if max(img.size) <= side:
        return img
    s = side / max(img.size)
    return img.resize((max(1, round(img.width * s)), max(1, round(img.height * s))), Image.LANCZOS)


def aspect_label(w, h):
    """Gemini 가 받는 화면비 중 가장 가까운 것"""
    options = {"1:1": 1, "2:3": 2 / 3, "3:2": 1.5, "3:4": 0.75, "4:3": 4 / 3, "4:5": 0.8, "5:4": 1.25, "9:16": 9 / 16, "16:9": 16 / 9}
    r = w / h
    return min(options, key=lambda k: abs(options[k] - r))


def decode_image(data):
    raw = base64.b64decode(data) if isinstance(data, str) else bytes(data)
    return Image.open(io.BytesIO(raw)).convert("RGB")


# ---------------- Gemini 호출 ----------------

def gemini_image(client, model, instruction, image, refs=(), previous_id=None, size="1K"):
    """편집 결과 (PIL Image, interaction_id, 텍스트). Interactions API 우선, 없으면 generate_content."""
    aspect = aspect_label(*image.size) if image is not None else "2:3"
    text = f"{instruction}\n\n{GUARD}"
    if hasattr(client, "interactions"):
        content = [{"type": "text", "text": text}]
        if image is not None:
            content.append({"type": "image", "mime_type": "image/png",
                            "data": base64.b64encode(png_bytes(limit(image, MAX_INPUT_SIDE))).decode()})
        for r in refs:
            content.append({"type": "image", "mime_type": "image/png",
                            "data": base64.b64encode(png_bytes(limit(r, MAX_REF_SIDE))).decode()})
        kwargs = dict(model=model, input=content,
                      response_format={"type": "image", "mime_type": "image/png", "aspect_ratio": aspect, "image_size": size})
        if previous_id:
            kwargs["previous_interaction_id"] = previous_id
        it = client.interactions.create(**kwargs)
        out = getattr(it, "output_image", None)
        if out is None or not getattr(out, "data", None):
            reason = getattr(it, "output_text", None) or getattr(it, "status", "no image returned")
            raise RuntimeError(f"Gemini returned no image: {reason}")
        return decode_image(out.data), getattr(it, "id", None), getattr(it, "output_text", None)
    # 구버전 SDK (1.x): generate_content + response_modalities
    from google.genai import types
    parts = [text]
    if image is not None:
        parts.append(types.Part.from_bytes(data=png_bytes(limit(image, MAX_INPUT_SIDE)), mime_type="image/png"))
    parts += [types.Part.from_bytes(data=png_bytes(limit(r, MAX_REF_SIDE)), mime_type="image/png") for r in refs]
    resp = client.models.generate_content(
        model=model, contents=parts,
        config=types.GenerateContentConfig(response_modalities=["IMAGE", "TEXT"],
                                           image_config=types.ImageConfig(aspect_ratio=aspect)))
    for cand in resp.candidates or []:
        for part in cand.content.parts or []:
            if getattr(part, "inline_data", None) and part.inline_data.data:
                return decode_image(part.inline_data.data), None, None
    raise RuntimeError("Gemini returned no image")


def flux_image(instruction, image, strength, seed=7):
    """로컬 ComfyUI FLUX img2img (API 키 없이)"""
    import ai_finish
    import tempfile
    started = None
    if not ai_finish.server_up():
        started = ai_finish.start_server(os.path.join(DF_HOME, "build", "comfyui.log"))
    try:
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "in.png")
            image.save(p)
            data = ai_finish.run(ai_finish.workflow(ai_finish.upload(p), f"{instruction}, {ai_finish.SAFE}", seed, strength, 6))
        return Image.open(io.BytesIO(data)).convert("RGB").resize(image.size, Image.LANCZOS)
    finally:
        if started:
            started.terminate()


# ---------------- 세션 ----------------

class Session:
    def __init__(self, folder):
        self.folder = os.path.abspath(folder)
        with open(os.path.join(self.folder, "session.json"), encoding="utf-8") as f:
            self.data = json.load(f)

    @staticmethod
    def path_for(name):
        return name if os.path.isdir(name) or os.sep in name else os.path.join(SESSIONS, name)

    @classmethod
    def create(cls, folder, base, source, recipe=None, refs=()):
        if os.path.exists(os.path.join(folder, "session.json")):
            raise SystemExit(f"session already exists: {folder}")
        os.makedirs(folder, exist_ok=True)
        base.convert("RGB").save(os.path.join(folder, "000.png"))
        ref_names = []
        for i, r in enumerate(refs):
            name = f"ref_{i}.png"
            limit(Image.open(r).convert("RGB"), MAX_REF_SIDE).save(os.path.join(folder, name))
            ref_names.append({"file": name, "source": os.path.relpath(r, REPO) if r.startswith(REPO) else r})
        data = {"version": 1, "created": time.strftime("%Y-%m-%d %H:%M:%S"), "recipe": recipe, "references": ref_names,
                "size": list(base.size), "head": 0,
                "steps": [{"id": 0, "op": "base", "source": source, "image": "000.png", "parent": None}]}
        with open(os.path.join(folder, "session.json"), "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
        return cls(folder)

    def save(self):
        with open(os.path.join(self.folder, "session.json"), "w", encoding="utf-8") as f:
            json.dump(self.data, f, ensure_ascii=False, indent=2)

    @property
    def head(self):
        return self.data["steps"][self.data["head"]]

    def image(self, step=None):
        s = self.head if step is None else self.data["steps"][step]
        return Image.open(os.path.join(self.folder, s["image"])).convert("RGB")

    def refs(self):
        return [Image.open(os.path.join(self.folder, r["file"])).convert("RGB") for r in self.data["references"]]

    def add(self, img, **info):
        sid = len(self.data["steps"])
        name = f"{sid:03d}.png"
        img = img.convert("RGB")
        if list(img.size) != self.data["size"]:
            img = img.resize(tuple(self.data["size"]), Image.LANCZOS)
        img.save(os.path.join(self.folder, name))
        step = {"id": sid, "image": name, "parent": self.data["head"], "time": time.strftime("%H:%M:%S"), **info}
        self.data["steps"].append(step)
        self.data["head"] = sid
        self.save()
        return step

    def undo(self):
        parent = self.head.get("parent")
        if parent is None:
            raise SystemExit("nothing to undo")
        self.data["head"] = parent
        self.save()

    def redo(self):
        kids = [s["id"] for s in self.data["steps"] if s.get("parent") == self.data["head"]]
        if not kids:
            raise SystemExit("nothing to redo")
        self.data["head"] = kids[-1]
        self.save()

    def goto(self, sid):
        if not 0 <= sid < len(self.data["steps"]):
            raise SystemExit(f"no step {sid}")
        self.data["head"] = sid
        self.save()

    def state(self):
        return {"folder": self.folder, "head": self.data["head"], "image": os.path.join(self.folder, self.head["image"]),
                "size": self.data["size"], "references": [r["source"] for r in self.data["references"]],
                "steps": [{k: s.get(k) for k in ("id", "op", "instruction", "region", "backend", "model", "parent", "time", "source")}
                          for s in self.data["steps"]]}


def feather_paste(base, patch, box, feather=12):
    """patch 를 box 자리에 가장자리를 부드럽게 붙인다. box 밖의 픽셀은 바뀌지 않는다"""
    x, y, w, h = box
    patch = patch.resize((w, h), Image.LANCZOS)
    mask = Image.new("L", (w, h), 0)
    f = max(1, min(feather, w // 4, h // 4))
    inner = Image.new("L", (max(1, w - 2 * f), max(1, h - 2 * f)), 255)
    mask.paste(inner, (f, f))
    mask = mask.filter(ImageFilter.GaussianBlur(f / 2))
    out = base.copy()
    out.paste(patch, (x, y), mask)
    return out


def region_box(region, size):
    x, y, w, h = [int(v) for v in region.split(",")]
    W, H = size
    x, y = max(0, min(x, W - 1)), max(0, min(y, H - 1))
    w, h = max(8, min(w, W - x)), max(8, min(h, H - y))
    return x, y, w, h


def do_edit(sess, instruction, backend="gemini", model=None, region=None, use_refs=True, strength=0.55, size="1K", client=None):
    model = model or DEFAULT_MODEL
    cur = sess.image()
    head = sess.head
    refs = sess.refs() if use_refs else []
    info = {"op": "edit", "instruction": instruction, "backend": backend, "region": region}
    if region:
        box = region_box(region, cur.size)
        x, y, w, h = box
        crop = cur.crop((x, y, x + w, y + h))
        local = f"This is a cropped region of a larger image. {instruction}. Change only what is asked inside this crop."
        if backend == "gemini":
            client = client or make_client()
            patch, iid, text = gemini_image(client, model, local, crop, refs, None, size)
            info.update(model=model, interaction_id=iid, note=text)
        else:
            patch = flux_image(local, crop, strength)
        result = feather_paste(cur, patch, box)
        info["region"] = list(box)
    else:
        if backend == "gemini":
            client = client or make_client()
            # 머리 단계가 같은 모델의 Gemini 결과면 대화를 이어 간다 (그 결과 이미지가 곧 지금 이미지)
            prev = head.get("interaction_id") if head.get("backend") == "gemini" and head.get("model") == model and not head.get("region") else None
            result, iid, text = gemini_image(client, model, instruction, None if prev else cur, [] if prev else refs, prev, size)
            info.update(model=model, interaction_id=iid, note=text, continued=bool(prev))
        else:
            result = flux_image(instruction, cur, strength)
            info["strength"] = strength
    return sess.add(result, **info)


def do_adjust(sess, brightness=1.0, contrast=1.0, saturation=1.0, warmth=0.0, sharpness=1.0):
    img = sess.image()
    img = ImageEnhance.Brightness(img).enhance(brightness)
    img = ImageEnhance.Contrast(img).enhance(contrast)
    img = ImageEnhance.Color(img).enhance(saturation)
    img = ImageEnhance.Sharpness(img).enhance(sharpness)
    if warmth:
        r, g, b = img.split()
        r = r.point(lambda v: min(255, int(v * (1 + warmth))))
        b = b.point(lambda v: min(255, int(v * (1 - warmth))))
        img = Image.merge("RGB", (r, g, b))
    return sess.add(img, op="adjust", instruction=f"brightness {brightness} contrast {contrast} saturation {saturation} "
                                                  f"warmth {warmth} sharpness {sharpness}", backend="local")


def recipe_references(recipe):
    """레시피 meta.reference 의 3D 레퍼런스 이미지 (예: '03_Sources/3D_Models/JennySisters.png (왼쪽)')"""
    import ai_finish
    out = []
    for d in ai_finish.chain(recipe):
        ref = d.get("meta", {}).get("reference")
        m = re.match(r"\s*(\S+?\.(?:png|jpe?g|webp))", ref or "", re.I)   # 경로 뒤 설명("(왼쪽)" 등)은 버린다
        if m:
            path = os.path.join(REPO, m.group(1))
            if os.path.exists(path) and path not in out:
                out.append(path)
    return out


def engine_render(recipe):
    import dollyforge as df
    px = df.doll(recipe).render(832, 1248, yaw=12, pitch=-4, zoom=1, background=True, hq=True)
    return Image.frombytes("RGBA", (832, 1248), px).convert("RGB")


# ---------------- 호환성 점검 ----------------

def doctor(live=False):
    rep = {"python": sys.version.split()[0], "checks": []}

    def check(name, ok, detail):
        rep["checks"].append({"name": name, "ok": bool(ok), "detail": detail})

    try:
        import google.genai as g
        from google import genai
        check("google-genai SDK", True, g.__version__)
        c = genai.Client(api_key="compat-check")
        api = "interactions" if hasattr(c, "interactions") else "generate_content" if hasattr(c.models, "generate_content") else None
        check("image API surface", api is not None, api or "neither interactions nor generate_content")
    except ImportError:
        check("google-genai SDK", False, "not installed — run DollyForge/python/.venv/bin/pip install google-genai")
    key, where = find_api_key()
    check("API key", key is not None, where or "not found (GEMINI_API_KEY / GOOGLE_API_KEY / ~/.gemini/.env)")
    try:
        cli = shutil.which("gemini") or os.path.expanduser("~/.npm-global/bin/gemini")
        if os.path.exists(cli):
            import subprocess
            ver = subprocess.run([cli, "--version"], capture_output=True, text=True, timeout=20).stdout.strip()
            settings = os.path.expanduser("~/.gemini/settings.json")
            auth = json.load(open(settings)).get("security", {}).get("auth", {}).get("selectedType") if os.path.exists(settings) else None
            check("Gemini CLI", True, f"{ver}, auth={auth} (same key lookup)")
    except Exception as e:  # CLI 는 선택 사항
        check("Gemini CLI", True, f"present but not queried ({e.__class__.__name__})")
    for ref in ("JennyCartoon3D.jpg", "JennySisters.png", "JennyDressUp.png", "JennyMarriage.png", "JennyLovesWitchCraft.png"):
        p = os.path.join(REPO, "03_Sources", "3D_Models", ref)
        if os.path.exists(p):
            im = Image.open(p)
            check(f"reference {ref}", im.format in ("PNG", "JPEG", "WEBP"), f"{im.format} {im.size[0]}x{im.size[1]} → sent ≤{MAX_REF_SIDE}px PNG")
    try:
        import ai_finish
        check("local FLUX (ComfyUI)", os.path.exists(os.path.join(ai_finish.COMFY_DIR, "main.py")),
              f"{ai_finish.COMFY_DIR} ({'running' if ai_finish.server_up() else 'starts on demand'})")
    except Exception as e:
        check("local FLUX (ComfyUI)", False, str(e))
    if live and key:
        try:
            client = make_client()
            names = [m.name.split("/")[-1] for m in client.models.list()]
            image_models = [n for n in names if "image" in n or "banana" in n]
            usable = [m for m in MODEL_PREFERENCE if m in names]
            check("image models on this key", bool(usable), ", ".join(usable or image_models[:8]) or "none listed")
            if usable:
                img, _, _ = gemini_image(client, usable[0], "A small purple daisy flower hairpin on a wooden table, stylized 3D render.", None)
                check("live generation", img is not None, f"{usable[0]} → {img.size[0]}x{img.size[1]}")
        except Exception as e:
            check("live API", False, f"{e.__class__.__name__}: {str(e)[:200]}")
    return rep


# ---------------- CLI ----------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--json", action="store_true", help="결과를 JSON 으로 (Studio 연동용)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("doctor"); d.add_argument("--live", action="store_true")
    n = sub.add_parser("new"); n.add_argument("session"); g = n.add_mutually_exclusive_group(required=True)
    g.add_argument("--recipe"); g.add_argument("--image")
    n.add_argument("--ref", action="append", default=None, help="레퍼런스 이미지 (기본: 레시피의 3D 레퍼런스)")
    n.add_argument("--no-ref", action="store_true")
    e = sub.add_parser("edit"); e.add_argument("session"); e.add_argument("instruction")
    e.add_argument("--region", help="x,y,w,h (픽셀)"); e.add_argument("--backend", choices=["gemini", "flux"], default="gemini")
    e.add_argument("--model"); e.add_argument("--no-ref", action="store_true"); e.add_argument("--strength", type=float, default=0.55)
    e.add_argument("--size", choices=["1K", "2K", "4K"], default="1K")
    a = sub.add_parser("adjust"); a.add_argument("session")
    for k in ("brightness", "contrast", "saturation", "sharpness"):
        a.add_argument("--" + k, type=float, default=1.0)
    a.add_argument("--warmth", type=float, default=0.0)
    for c in ("undo", "redo", "log"):
        sub.add_parser(c).add_argument("session")
    go = sub.add_parser("goto"); go.add_argument("session"); go.add_argument("step", type=int)
    x = sub.add_parser("export"); x.add_argument("session"); x.add_argument("-o", "--out", required=True)
    args = ap.parse_args(argv)

    def emit(obj, text):
        print(json.dumps(obj, ensure_ascii=False) if args.json else text)

    if args.cmd == "doctor":
        rep = doctor(args.live)
        emit(rep, "\n".join(f"[{'ok' if c['ok'] else '!!'}] {c['name']}: {c['detail']}" for c in rep["checks"]))
        return 0 if all(c["ok"] for c in rep["checks"] if c["name"] != "API key") else 1
    folder = Session.path_for(args.session)
    if args.cmd == "new":
        if args.recipe:
            refs = [] if args.no_ref else (args.ref or recipe_references(args.recipe))
            s = Session.create(folder, engine_render(args.recipe), f"engine render {os.path.basename(args.recipe)}",
                               os.path.abspath(args.recipe), refs)
        else:
            refs = [] if args.no_ref else (args.ref or [])
            s = Session.create(folder, Image.open(args.image), f"image {os.path.basename(args.image)}", None, refs)
        emit(s.state(), f"session {s.folder} (refs: {len(s.data['references'])})")
        return 0
    s = Session(folder)
    if args.cmd == "edit":
        step = do_edit(s, args.instruction, args.backend, args.model, args.region, not args.no_ref, args.strength, args.size)
        emit(s.state(), f"step {step['id']}: {args.instruction}")
    elif args.cmd == "adjust":
        step = do_adjust(s, args.brightness, args.contrast, args.saturation, args.warmth, args.sharpness)
        emit(s.state(), f"step {step['id']}: {step['instruction']}")
    elif args.cmd in ("undo", "redo"):
        getattr(s, args.cmd)()
        emit(s.state(), f"head -> {s.data['head']}")
    elif args.cmd == "goto":
        s.goto(args.step)
        emit(s.state(), f"head -> {s.data['head']}")
    elif args.cmd == "log":
        emit(s.state(), "\n".join(f"{'*' if st['id'] == s.data['head'] else ' '} {st['id']:3d} {st['op']:6s} "
                                  f"{st.get('backend') or '':6s} {st.get('instruction') or st.get('source') or ''}"
                                  for st in s.data["steps"]))
    elif args.cmd == "export":
        s.image().save(args.out)
        emit({"out": args.out}, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
