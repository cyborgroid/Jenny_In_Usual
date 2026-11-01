//! libdollyforge C API 바인딩과 안전한 래퍼
use std::ffi::{c_char, c_int, CStr, CString};

pub const PART_COUNT: usize = 6;

#[repr(C)]
pub struct RawSkin {
    _private: [u8; 0],
}
#[repr(C)]
pub struct RawJson {
    _private: [u8; 0],
}

extern "C" {
    fn df_skin_load_png(path: *const c_char, err: *mut c_char, errlen: usize) -> *mut RawSkin;
    fn df_forge_file(path: *const c_char, err: *mut c_char, errlen: usize) -> *mut RawSkin;
    fn df_skin_free(s: *mut RawSkin);
    fn df_skin_model(s: *const RawSkin) -> c_int;
    fn df_skin_rgba_const(s: *const RawSkin) -> *const u8;
    fn df_validate(s: *const RawSkin, buf: *mut c_char, buflen: usize) -> c_int;
    fn df_diff(a: *const RawSkin, b: *const RawSkin, diff: *mut c_int) -> c_int;
    fn df_part_name(part: c_int) -> *const c_char;

    fn df_json_parse(text: *const c_char, len: usize, err: *mut c_char, errlen: usize) -> *mut RawJson;
    fn df_json_free(v: *mut RawJson);
    fn df_json_kind(v: *const RawJson) -> c_int;
    fn df_json_str(v: *const RawJson) -> *const c_char;
    fn df_json_len(v: *const RawJson) -> usize;
    fn df_json_at(v: *const RawJson, i: usize) -> *mut RawJson;
    fn df_json_get(v: *const RawJson, key: *const c_char) -> *mut RawJson;
    fn df_json_bool(v: *const RawJson) -> c_int;
}

fn err_string(buf: &[u8]) -> String {
    let end = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    String::from_utf8_lossy(&buf[..end]).into_owned()
}

fn cpath(path: &str) -> Result<CString, String> {
    CString::new(path).map_err(|_| format!("path contains NUL: {path}"))
}

/// C 코어가 소유한 64x64 스킨
pub struct Skin(*mut RawSkin);

impl Skin {
    pub fn load(path: &str) -> Result<Skin, String> {
        let mut err = [0u8; 256];
        let p = cpath(path)?;
        let raw = unsafe { df_skin_load_png(p.as_ptr(), err.as_mut_ptr().cast(), err.len()) };
        if raw.is_null() { Err(err_string(&err)) } else { Ok(Skin(raw)) }
    }

    pub fn forge(recipe: &str) -> Result<Skin, String> {
        let mut err = [0u8; 512];
        let p = cpath(recipe)?;
        let raw = unsafe { df_forge_file(p.as_ptr(), err.as_mut_ptr().cast(), err.len()) };
        if raw.is_null() { Err(err_string(&err)) } else { Ok(Skin(raw)) }
    }

    pub fn slim(&self) -> bool {
        unsafe { df_skin_model(self.0) == 1 }
    }

    pub fn rgba(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(df_skin_rgba_const(self.0), 64 * 64 * 4) }
    }

    /// (오류 수, 보고서)
    pub fn validate(&self) -> (i32, String) {
        let mut buf = vec![0u8; 8192];
        let n = unsafe { df_validate(self.0, buf.as_mut_ptr().cast(), buf.len()) };
        (n, err_string(&buf))
    }

    /// (전체 다른 픽셀 수, 파트별)
    pub fn diff(&self, other: &Skin) -> (i32, [i32; PART_COUNT]) {
        let mut parts = [0 as c_int; PART_COUNT];
        let total = unsafe { df_diff(self.0, other.0, parts.as_mut_ptr()) };
        (total, parts)
    }

    /// 투명 픽셀의 RGB 를 0 으로 정규화한 RGBA + 모델 바이트.
    /// 같은 그림을 다른 프로그램으로 다시 저장해도 같은 값이 나온다.
    pub fn canonical_pixels(&self) -> Vec<u8> {
        let mut v = self.rgba().to_vec();
        for px in v.chunks_mut(4) {
            if px[3] == 0 {
                px.copy_from_slice(&[0, 0, 0, 0]);
            }
        }
        v.push(self.slim() as u8);
        v
    }
}

impl Drop for Skin {
    fn drop(&mut self) {
        unsafe { df_skin_free(self.0) }
    }
}

pub fn part_name(i: usize) -> String {
    unsafe { CStr::from_ptr(df_part_name(i as c_int)).to_string_lossy().into_owned() }
}

/// C 코어의 JSON 파서로 읽은 문서 (모든 도구가 같은 해석을 공유)
pub struct Json {
    root: *mut RawJson,
}

#[derive(Clone, Copy)]
pub struct Node<'a> {
    raw: *const RawJson,
    _doc: std::marker::PhantomData<&'a Json>,
}

impl Json {
    pub fn parse(text: &str) -> Result<Json, String> {
        let mut err = [0u8; 256];
        let raw = unsafe { df_json_parse(text.as_ptr().cast(), text.len(), err.as_mut_ptr().cast(), err.len()) };
        if raw.is_null() { Err(err_string(&err)) } else { Ok(Json { root: raw }) }
    }

    pub fn root(&self) -> Node<'_> {
        Node { raw: self.root, _doc: std::marker::PhantomData }
    }
}

impl Drop for Json {
    fn drop(&mut self) {
        unsafe { df_json_free(self.root) }
    }
}

impl<'a> Node<'a> {
    fn wrap(raw: *const RawJson) -> Option<Node<'a>> {
        if raw.is_null() { None } else { Some(Node { raw, _doc: std::marker::PhantomData }) }
    }

    pub fn get(&self, key: &str) -> Option<Node<'a>> {
        let k = CString::new(key).ok()?;
        Self::wrap(unsafe { df_json_get(self.raw, k.as_ptr()) })
    }

    pub fn str(&self) -> Option<&'a str> {
        let p = unsafe { df_json_str(self.raw) };
        if p.is_null() { None } else { unsafe { CStr::from_ptr(p) }.to_str().ok() }
    }

    pub fn is_object(&self) -> bool {
        unsafe { df_json_kind(self.raw) == 5 }
    }

    pub fn truthy(&self) -> bool {
        unsafe { df_json_bool(self.raw) != 0 }
    }

    pub fn items(&self) -> Vec<Node<'a>> {
        let n = unsafe { df_json_len(self.raw) };
        (0..n).filter_map(|i| Self::wrap(unsafe { df_json_at(self.raw, i) })).collect()
    }
}
