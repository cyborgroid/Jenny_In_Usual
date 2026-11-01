"""Dolly Forge Python 바인딩 — libdollyforge(C 코어)를 ctypes 로 연다.

    import dollyforge as df
    skin = df.forge("characters/jenny/jenny_cyber.dolly.json")
    print(skin.slim, skin.validate())
    skin.save("out.png")
"""
import ctypes
import os

__all__ = ["Skin", "Doll", "forge", "forge_json", "load", "doll", "doll_json", "library_path", "PARTS"]

PARTS = ("head", "body", "arm_r", "arm_l", "leg_r", "leg_l")
_HERE = os.path.dirname(os.path.abspath(__file__))


def library_path():
    """DOLLYFORGE_LIB 환경 변수 → DollyForge/build/ 순서로 찾는다."""
    env = os.environ.get("DOLLYFORGE_LIB")
    if env:
        return env
    for name in ("libdollyforge.so", "libdollyforge.dylib", "dollyforge.dll"):
        p = os.path.normpath(os.path.join(_HERE, "..", "..", "build", name))
        if os.path.exists(p):
            return p
    raise OSError("libdollyforge not found: build it with `cmake -S DollyForge -B DollyForge/build && "
                  "cmake --build DollyForge/build` or set DOLLYFORGE_LIB")


_lib = ctypes.CDLL(library_path())
_p = ctypes.c_void_p
_lib.df_version.restype = ctypes.c_char_p
_lib.df_skin_load_png.restype = _p
_lib.df_skin_load_png.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
_lib.df_forge_file.restype = _p
_lib.df_forge_file.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
_lib.df_forge_json.restype = _p
_lib.df_forge_json.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
_lib.df_skin_free.argtypes = [_p]
_lib.df_skin_model.argtypes = [_p]
_lib.df_skin_rgba_const.restype = ctypes.POINTER(ctypes.c_uint8)
_lib.df_skin_rgba_const.argtypes = [_p]
_lib.df_skin_save_png.argtypes = [_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
_lib.df_validate.argtypes = [_p, ctypes.c_char_p, ctypes.c_size_t]
_lib.df_render_flat.argtypes = [_p, ctypes.c_int, ctypes.POINTER(ctypes.c_uint8)]
_lib.df_diff.argtypes = [_p, _p, ctypes.POINTER(ctypes.c_int)]

_lib.df_doll_from_recipe.restype = _p
_lib.df_doll_from_recipe.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
_lib.df_doll_from_json.restype = _p
_lib.df_doll_from_json.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
_lib.df_doll_free.argtypes = [_p]
_lib.df_doll_render.argtypes = [_p, ctypes.c_int, ctypes.c_int, ctypes.c_double, ctypes.c_double, ctypes.c_double,
                                ctypes.c_int, ctypes.POINTER(ctypes.c_uint8)]
RENDER_BACKGROUND, RENDER_HQ = 1, 2

version = _lib.df_version().decode()


class DollyForgeError(Exception):
    pass


class Skin:
    """C 코어가 소유한 64x64 스킨 핸들"""

    def __init__(self, handle):
        if not handle:
            raise ValueError("null skin handle")
        self._h = handle

    def __del__(self):
        h, self._h = getattr(self, "_h", None), None
        if h:
            _lib.df_skin_free(h)

    @property
    def slim(self):
        return _lib.df_skin_model(self._h) == 1

    @property
    def rgba(self):
        return ctypes.string_at(_lib.df_skin_rgba_const(self._h), 64 * 64 * 4)

    def save(self, path):
        err = ctypes.create_string_buffer(256)
        if not _lib.df_skin_save_png(self._h, os.fsencode(path), err, len(err)):
            raise DollyForgeError(err.value.decode())

    def validate(self):
        """(오류 수, ["ERROR ...", "WARN ..."])"""
        buf = ctypes.create_string_buffer(8192)
        n = _lib.df_validate(self._h, buf, len(buf))
        return n, [l for l in buf.value.decode().splitlines() if l]

    def render_flat(self, view="front"):
        """정면/후면 펼침 그림 (16x32 RGBA bytes)"""
        out = (ctypes.c_uint8 * (16 * 32 * 4))()
        _lib.df_render_flat(self._h, 1 if view == "back" else 0, out)
        return bytes(out)

    def diff(self, other):
        parts = (ctypes.c_int * len(PARTS))()
        total = _lib.df_diff(self._h, other._h, parts)
        return total, dict(zip(PARTS, parts))


def _check(handle, err):
    if not handle:
        raise DollyForgeError(err.value.decode("utf-8", "replace"))
    return Skin(handle)


def forge(recipe_path):
    err = ctypes.create_string_buffer(512)
    return _check(_lib.df_forge_file(os.fsencode(recipe_path), err, len(err)), err)


def forge_json(text, base_dir="."):
    data = text.encode("utf-8")
    err = ctypes.create_string_buffer(512)
    return _check(_lib.df_forge_json(data, len(data), os.fsencode(base_dir), err, len(err)), err)


def load(png_path):
    err = ctypes.create_string_buffer(256)
    return _check(_lib.df_skin_load_png(os.fsencode(png_path), err, len(err)), err)


class Doll:
    """곡선 3D 캐릭터 (레시피의 doll 섹션 + 같은 팔레트)"""

    def __init__(self, handle):
        if not handle:
            raise ValueError("null doll handle")
        self._h = handle

    def __del__(self):
        h, self._h = getattr(self, "_h", None), None
        if h:
            _lib.df_doll_free(h)

    def render(self, width=600, height=900, yaw=22.0, pitch=6.0, zoom=1.0, background=True, hq=False):
        """RGBA bytes (width*height*4)"""
        out = (ctypes.c_uint8 * (width * height * 4))()
        flags = (RENDER_BACKGROUND if background else 0) | (RENDER_HQ if hq else 0)
        _lib.df_doll_render(self._h, width, height, yaw, pitch, zoom, flags, out)
        return bytes(out)


def doll(recipe_path):
    err = ctypes.create_string_buffer(512)
    h = _lib.df_doll_from_recipe(os.fsencode(recipe_path), err, len(err))
    if not h:
        raise DollyForgeError(err.value.decode("utf-8", "replace"))
    return Doll(h)


def doll_json(text, base_dir="."):
    data = text.encode("utf-8")
    err = ctypes.create_string_buffer(512)
    h = _lib.df_doll_from_json(data, len(data), os.fsencode(base_dir), err, len(err))
    if not h:
        raise DollyForgeError(err.value.decode("utf-8", "replace"))
    return Doll(h)
