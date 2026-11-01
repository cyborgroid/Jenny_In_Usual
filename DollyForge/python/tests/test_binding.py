"""Python 바인딩 + 레시피 회귀 테스트:  python3 -m unittest discover -s python/tests"""
import glob
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))

import dollyforge as df  # noqa: E402
from dollyforge import recipe as R  # noqa: E402

try:
    from PIL import Image
except ImportError:  # PIL 이 없으면 교차 검증만 건너뛴다
    Image = None

RECIPES = sorted(glob.glob(os.path.join(ROOT, "characters", "*", "*.dolly.json")))


class RecipeTests(unittest.TestCase):
    def test_every_recipe_forges_valid_slim_skin(self):
        self.assertTrue(RECIPES)
        for path in RECIPES:
            meta = R.load(path).get("meta", {})
            if meta.get("abstract"):
                continue
            with self.subTest(recipe=os.path.basename(path)):
                skin = df.forge(path)
                errors, report = skin.validate()
                self.assertEqual(errors, 0, report)
                self.assertTrue(skin.slim)
                self.assertEqual(meta.get("license"), "CC-BY-NC-ND-4.0")

    def test_formatter_roundtrip_keeps_pixels(self):
        path = os.path.join(ROOT, "characters", "jenny", "jenny_cyber.dolly.json")
        original = df.forge(path)
        text = R.dumps(R.load(path))
        again = df.forge_json(text, os.path.dirname(path))
        self.assertEqual(original.diff(again)[0], 0)

    def test_palette_override_is_a_makeup_variant(self):
        base = os.path.join(ROOT, "characters", "jenny")
        recipe = {"dollyforge": 1, "extends": "jenny_cyber.dolly.json", "palette": {"turtleneck": "#00FF00"}}
        variant = df.forge_json(json.dumps(recipe), base)
        total, parts = df.forge(os.path.join(base, "jenny_cyber.dolly.json")).diff(variant)
        self.assertGreater(parts["body"], 0)
        self.assertEqual(parts["head"], 0)

    def test_errors_are_reported(self):
        with self.assertRaises(df.DollyForgeError) as cm:
            df.forge_json('{"ops": [{"op": "fill", "part": "head", "color": "nope"}]}')
        self.assertIn("unknown color 'nope'", str(cm.exception))


class DollTests(unittest.TestCase):
    def test_every_look_renders_as_curved_3d(self):
        for path in RECIPES:
            if R.load(path).get("meta", {}).get("abstract"):
                continue
            with self.subTest(recipe=os.path.basename(path)):
                px = df.doll(path).render(60, 90, background=False)
                covered = sum(1 for i in range(3, len(px), 4) if px[i])
                self.assertGreater(covered, 60 * 90 * 0.12)   # 캐릭터가 화면을 채운다

    def test_makeup_override_changes_only_the_face(self):
        base = os.path.join(ROOT, "characters", "jenny")
        plain = df.doll(os.path.join(base, "jenny_cyber.dolly.json")).render(120, 180, background=False)
        variant = {"dollyforge": 1, "extends": "jenny_cyber.dolly.json", "doll": {"makeup": {"blush": 0.0}}}
        changed = df.doll_json(json.dumps(variant), base).render(120, 180, background=False)
        diff = [i // 4 // 120 for i in range(0, len(plain), 4) if plain[i:i + 3] != changed[i:i + 3]]
        self.assertTrue(diff)
        self.assertLess(max(diff), 180 * 0.25)               # 바뀐 픽셀은 머리 높이 안에만


@unittest.skipIf(Image is None, "Pillow not installed")
class PngInteropTests(unittest.TestCase):
    def test_c_encoder_is_readable_by_pillow(self):
        skin = df.forge(os.path.join(ROOT, "characters", "jenny", "jenny_casual.dolly.json"))
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "s.png")
            skin.save(p)
            self.assertEqual(Image.open(p).convert("RGBA").tobytes(), skin.rgba)

    def test_c_decoder_reads_pillow_png(self):
        skin = df.forge(os.path.join(ROOT, "characters", "jenny", "jenny_cyber.dolly.json"))
        img = Image.frombytes("RGBA", (64, 64), skin.rgba)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "pil.png")
            img.save(p, optimize=True)
            self.assertEqual(df.load(p).diff(skin)[0], 0)


if __name__ == "__main__":
    unittest.main()
