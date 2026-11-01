"""편집 작업실 테스트 — 가짜 Gemini 클라이언트로 요청 형태와 편집 흐름을 검증 (네트워크·키 불필요)"""
import base64
import io
import os
import sys
import tempfile
import types
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, os.path.join(HERE, ".."))

from PIL import Image  # noqa: E402
import gemini_studio as gs  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))


class FakeInteractions:
    def __init__(self, color):
        self.calls, self.color, self.n = [], color, 0

    def create(self, **kwargs):
        self.calls.append(kwargs)
        self.n += 1
        aspect = kwargs["response_format"]["aspect_ratio"]
        size = (512, 768) if aspect == "2:3" else (512, 512)
        buf = io.BytesIO()
        Image.new("RGB", size, self.color).save(buf, "PNG")
        img = types.SimpleNamespace(data=base64.b64encode(buf.getvalue()).decode())
        return types.SimpleNamespace(id=f"int-{self.n}", output_image=img, output_text="done")


class FakeClient:
    def __init__(self, color=(10, 200, 30)):
        self.interactions = FakeInteractions(color)


class StudioTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Image.new("RGB", (832, 1248), (120, 60, 140))
        ref = os.path.join(self.tmp.name, "ref.png")
        Image.new("RGB", (2000, 3000), (1, 2, 3)).save(ref)
        self.sess = gs.Session.create(os.path.join(self.tmp.name, "s"), base, "test", None, [ref])

    def tearDown(self):
        self.tmp.cleanup()

    def test_whole_edit_request_and_conversation(self):
        client = FakeClient()
        gs.do_edit(self.sess, "make the buckle gold", client=client)
        call = client.interactions.calls[0]
        self.assertEqual(call["model"], gs.DEFAULT_MODEL)
        self.assertEqual(call["response_format"], {"type": "image", "mime_type": "image/png", "aspect_ratio": "2:3", "image_size": "1K"})
        kinds = [c["type"] for c in call["input"]]
        self.assertEqual(kinds, ["text", "image", "image"])          # 지시 + 현재 이미지 + 레퍼런스
        self.assertIn("all-ages", call["input"][0]["text"])
        ref_png = Image.open(io.BytesIO(base64.b64decode(call["input"][2]["data"])))
        self.assertLessEqual(max(ref_png.size), gs.MAX_REF_SIDE)     # 레퍼런스는 줄여서 보낸다
        self.assertEqual(self.sess.image().size, (832, 1248))         # 결과는 세션 캔버스 크기로
        # 두 번째 편집은 대화를 이어 간다: 이미지를 다시 보내지 않는다
        gs.do_edit(self.sess, "now make it silver", client=client)
        call2 = client.interactions.calls[1]
        self.assertEqual(call2["previous_interaction_id"], "int-1")
        self.assertEqual([c["type"] for c in call2["input"]], ["text"])
        # 되돌린 뒤에는 이어 가지 않고 이미지를 다시 보낸다
        self.sess.undo(); self.sess.undo()
        gs.do_edit(self.sess, "different idea", client=client)
        self.assertNotIn("previous_interaction_id", client.interactions.calls[2])

    def test_region_edit_keeps_outside_pixels(self):
        before = self.sess.image()
        gs.do_edit(self.sess, "bigger flower", region="100,100,200,160", client=FakeClient((255, 0, 0)))
        after = self.sess.image()
        self.assertEqual(self.sess.head["region"], [100, 100, 200, 160])
        for p in [(0, 0), (99, 150), (400, 400), (150, 99), (831, 1247)]:
            self.assertEqual(after.getpixel(p), before.getpixel(p))   # 영역 밖은 그대로
        self.assertEqual(after.getpixel((200, 180)), (255, 0, 0))     # 영역 안은 바뀜

    def test_undo_redo_goto_and_branch(self):
        gs.do_adjust(self.sess, brightness=1.2)
        self.assertNotEqual(self.sess.image().getpixel((5, 5)), (120, 60, 140))
        self.sess.undo()
        self.assertEqual(self.sess.image().getpixel((5, 5)), (120, 60, 140))
        self.sess.redo()
        self.assertEqual(self.sess.data["head"], 1)
        self.sess.goto(0)
        gs.do_adjust(self.sess, warmth=0.1)                           # 0 에서 새 갈래
        self.assertEqual(self.sess.head["parent"], 0)
        self.assertEqual(len(self.sess.state()["steps"]), 3)

    def test_api_key_lookup_matches_gemini_cli(self):
        with tempfile.TemporaryDirectory() as home:
            os.makedirs(os.path.join(home, ".gemini"))
            with open(os.path.join(home, ".gemini", ".env"), "w") as f:
                f.write("# comment\nexport GEMINI_API_KEY=\"abc123\"\n")
            saved = {k: os.environ.pop(k, None) for k in ("GEMINI_API_KEY", "GOOGLE_API_KEY", "HOME")}
            os.environ["HOME"] = home
            try:
                key, where = gs.find_api_key()
                self.assertEqual(key, "abc123")
                self.assertTrue(where.endswith(".gemini/.env"))
            finally:
                for k, v in saved.items():
                    if v is not None:
                        os.environ[k] = v

    def test_recipe_references_resolve(self):
        refs = gs.recipe_references(os.path.join(ROOT, "characters", "eyle_teal", "eyle_teal_cyber.dolly.json"))
        self.assertTrue(any(r.endswith("JennySisters.png") for r in refs))
        self.assertTrue(all(os.path.exists(r) for r in refs))


if __name__ == "__main__":
    unittest.main()
