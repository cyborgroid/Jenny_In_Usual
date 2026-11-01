/*
 * 레시피(.dolly.json) 실행기.
 *
 * {
 *   "dollyforge": 1, "model": "slim", "extends": "base.dolly.json",
 *   "palette": { "hair": "#8A5A3B", "trim": "hair" },
 *   "legend":  { "H": "hair", "_": "clear" },
 *   "ops": [ { "op": "fill", "part": "head", "faces": "all", "color": "skin" }, ... ]
 * }
 *
 * extends 는 파일 하나 또는 배열(예: [캐릭터 얼굴, 의상]). 부모들이 먼저 실행되고,
 * palette/legend 는 나중에 오는 쪽(결국 자식)이 덮어쓴다.
 * 그래서 자식 레시피에서 팔레트만 바꾸면 같은 옷의 "메이크업 변형"이 된다.
 */
#include "dollyforge_json.h"
#include "internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CHAIN 16
#define MAX_PALETTE 256

typedef struct {
    const char *name;
    const char *value; /* "#hex" 또는 다른 팔레트 키 */
} pal_entry;

typedef struct {
    df_skin *skin;
    pal_entry pal[MAX_PALETTE];
    int npal;
    const char *legend[256];
    char *err;
    size_t errlen;
    const char *where; /* 오류 메시지용: 현재 레시피 파일 */
    int op_index;
    const char *op_name;
} forge_ctx;

static int ferr(forge_ctx *c, const char *fmt, const char *arg) {
    if (c->err && c->errlen && !c->err[0]) {
        char msg[256];
        snprintf(msg, sizeof msg, fmt, arg ? arg : "");
        if (c->op_name)
            snprintf(c->err, c->errlen, "%s: ops[%d] (%s): %s", c->where, c->op_index, c->op_name, msg);
        else
            snprintf(c->err, c->errlen, "%s: %s", c->where, msg);
    }
    return 0;
}

static int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    ch = (char)tolower((unsigned char)ch);
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

static int parse_hex(const char *s, uint8_t rgba[4]) {
    size_t n = strlen(s);
    int v[8];
    if (s[0] != '#' || !(n == 4 || n == 7 || n == 9)) return 0;
    for (size_t i = 1; i < n; i++) if ((v[i - 1] = hexval(s[i])) < 0) return 0;
    if (n == 4) {
        for (int i = 0; i < 3; i++) rgba[i] = (uint8_t)(v[i] * 17);
        rgba[3] = 255;
    } else {
        for (int i = 0; i < 3; i++) rgba[i] = (uint8_t)(v[i * 2] * 16 + v[i * 2 + 1]);
        rgba[3] = n == 9 ? (uint8_t)(v[6] * 16 + v[7]) : 255;
    }
    return 1;
}

/*
 * 색 지정 해석: "#hex", "clear", 팔레트 키(별칭), "키*배율" (명도 파생색).
 * 예: "hair_dark": "hair*0.79" — 메이크업에서 hair 만 바꿔도 그림자색이 따라온다.
 */
static int resolve_depth(forge_ctx *c, const char *spec, uint8_t rgba[4], int depth) {
    if (depth > 8) return ferr(c, "palette alias loop near '%s'", spec);
    if (spec[0] == '#') {
        if (parse_hex(spec, rgba)) return 1;
        return ferr(c, "bad color '%s' (use #RGB, #RRGGBB or #RRGGBBAA)", spec);
    }
    if (!strcmp(spec, "clear") || !strcmp(spec, "transparent")) { memset(rgba, 0, 4); return 1; }
    const char *star = strchr(spec, '*');
    if (star) {
        char name[64], *end;
        size_t n = (size_t)(star - spec);
        double k = strtod(star + 1, &end);
        if (n == 0 || n >= sizeof name || *end || k < 0 || k > 4)
            return ferr(c, "bad derived color '%s' (use name*factor, factor 0..4)", spec);
        memcpy(name, spec, n);
        name[n] = 0;
        if (!resolve_depth(c, name, rgba, depth + 1)) return 0;
        for (int ch = 0; ch < 3; ch++) {
            double v = rgba[ch] * k + 0.5;
            rgba[ch] = (uint8_t)(v > 255 ? 255 : v);
        }
        return 1;
    }
    for (int i = c->npal; i-- > 0;)
        if (!strcmp(c->pal[i].name, spec)) return resolve_depth(c, c->pal[i].value, rgba, depth + 1);
    return ferr(c, "unknown color '%s' (not in palette)", spec);
}

static int resolve_color(forge_ctx *c, const char *spec, uint8_t rgba[4]) {
    return resolve_depth(c, spec, rgba, 0);
}

/* "head" / "*" / "arms" / "legs" / ["head","body"] → 비트마스크 */
static int parts_mask(forge_ctx *c, const df_json *v) {
    if (!v) return ferr(c, "missing 'part'%s", NULL);
    if (df_json_kind(v) == DF_JSON_ARRAY) {
        int m = 0;
        for (size_t i = 0; i < df_json_len(v); i++) {
            int k = parts_mask(c, df_json_at(v, i));
            if (!k) return 0;
            m |= k;
        }
        return m;
    }
    const char *s = df_json_str(v);
    if (!s) return ferr(c, "'part' must be a string or array%s", NULL);
    if (!strcmp(s, "*") || !strcmp(s, "all")) return (1 << DF_PART_COUNT) - 1;
    if (!strcmp(s, "arms")) return 1 << DF_PART_ARM_R | 1 << DF_PART_ARM_L;
    if (!strcmp(s, "legs")) return 1 << DF_PART_LEG_R | 1 << DF_PART_LEG_L;
    int p = df_part_from_name(s);
    if (p < 0) return ferr(c, "unknown part '%s' (head, body, arm_r, arm_l, leg_r, leg_l, arms, legs, *)", s);
    return 1 << p;
}

static int faces_mask(forge_ctx *c, const df_json *v, int dflt) {
    if (!v) return dflt;
    if (df_json_kind(v) == DF_JSON_ARRAY) {
        int m = 0;
        for (size_t i = 0; i < df_json_len(v); i++) {
            int k = faces_mask(c, df_json_at(v, i), 0);
            if (!k) return 0;
            m |= k;
        }
        return m;
    }
    const char *s = df_json_str(v);
    if (!s) return ferr(c, "'face(s)' must be a string or array%s", NULL);
    if (!strcmp(s, "all")) return (1 << DF_FACE_COUNT) - 1;
    if (!strcmp(s, "sides")) return 1 << DF_FACE_RIGHT | 1 << DF_FACE_FRONT | 1 << DF_FACE_LEFT | 1 << DF_FACE_BACK;
    int f = df_face_from_name(s);
    if (f < 0) return ferr(c, "unknown face '%s' (top, bottom, right, front, left, back, sides, all)", s);
    return 1 << f;
}

static int layer_of(forge_ctx *c, const df_json *op, int *layer) {
    const char *s = df_json_str(df_json_get(op, "layer"));
    if (!s || !strcmp(s, "base")) { *layer = DF_LAYER_BASE; return 1; }
    if (!strcmp(s, "overlay")) { *layer = DF_LAYER_OVERLAY; return 1; }
    return ferr(c, "unknown layer '%s' (base, overlay)", s);
}

static int get_int(const df_json *op, const char *key, int dflt) {
    const df_json *v = df_json_get(op, key);
    return df_json_kind(v) == DF_JSON_NUMBER ? (int)df_json_num(v) : dflt;
}

static void put_px(df_skin *s, int x, int y, const uint8_t rgba[4]) {
    if (x < 0 || y < 0 || x >= DF_SKIN_W || y >= DF_SKIN_H) return;
    memcpy(&df_skin_rgba(s)[(y * DF_SKIN_W + x) * 4], rgba, 4);
}

/* 면 안쪽 좌표 (fx,fy) 사각형을 칠한다. 면 밖으로 나가는 부분은 잘린다 */
static void paint_rect(forge_ctx *c, int part, int layer, int face, int fx, int fy, int fw, int fh, const uint8_t rgba[4]) {
    int x, y, w, h;
    df_face_rect(df_skin_model(c->skin), (df_part)part, (df_layer)layer, (df_face)face, &x, &y, &w, &h);
    for (int j = fy < 0 ? 0 : fy; j < fy + fh && j < h; j++)
        for (int i = fx < 0 ? 0 : fx; i < fx + fw && i < w; i++)
            put_px(c->skin, x + i, y + j, rgba);
}

static const char *op_color(forge_ctx *c, const df_json *op, uint8_t rgba[4]) {
    const char *spec = df_json_str(df_json_get(op, "color"));
    if (!spec) { ferr(c, "missing 'color'%s", NULL); return NULL; }
    return resolve_color(c, spec, rgba) ? spec : NULL;
}

static int op_fill(forge_ctx *c, const df_json *op) {
    uint8_t rgba[4];
    int layer, parts = parts_mask(c, df_json_get(op, "part"));
    int faces = faces_mask(c, df_json_get(op, "faces") ? df_json_get(op, "faces") : df_json_get(op, "face"),
                           (1 << DF_FACE_COUNT) - 1);
    if (!parts || !faces || !layer_of(c, op, &layer) || !op_color(c, op, rgba)) return 0;
    for (int p = 0; p < DF_PART_COUNT; p++)
        for (int f = 0; f < DF_FACE_COUNT; f++)
            if (parts >> p & 1 && faces >> f & 1) paint_rect(c, p, layer, f, 0, 0, 64, 64, rgba);
    return 1;
}

static int op_rect(forge_ctx *c, const df_json *op) {
    uint8_t rgba[4];
    int layer, parts = parts_mask(c, df_json_get(op, "part"));
    int faces = faces_mask(c, df_json_get(op, "faces") ? df_json_get(op, "faces") : df_json_get(op, "face"), 0);
    if (!faces) return ferr(c, "missing 'face'%s", NULL);
    if (!parts || !layer_of(c, op, &layer) || !op_color(c, op, rgba)) return 0;
    int x = get_int(op, "x", 0), y = get_int(op, "y", 0), w = get_int(op, "w", 1), h = get_int(op, "h", 1);
    for (int p = 0; p < DF_PART_COUNT; p++)
        for (int f = 0; f < DF_FACE_COUNT; f++)
            if (parts >> p & 1 && faces >> f & 1) paint_rect(c, p, layer, f, x, y, w, h, rgba);
    return 1;
}

/* 측면 4개를 한 바퀴 두르는 띠 (벨트, 부츠, 소맷부리) */
static int op_band(forge_ctx *c, const df_json *op) {
    uint8_t rgba[4];
    int layer, parts = parts_mask(c, df_json_get(op, "part"));
    int sides = 1 << DF_FACE_RIGHT | 1 << DF_FACE_FRONT | 1 << DF_FACE_LEFT | 1 << DF_FACE_BACK;
    int faces = faces_mask(c, df_json_get(op, "faces"), sides);
    if (!parts || !faces || !layer_of(c, op, &layer) || !op_color(c, op, rgba)) return 0;
    int y = get_int(op, "y", 0), h = get_int(op, "h", 1);
    for (int p = 0; p < DF_PART_COUNT; p++)
        for (int f = 0; f < DF_FACE_COUNT; f++)
            if (parts >> p & 1 && faces >> f & 1 & (sides >> f)) paint_rect(c, p, layer, f, 0, y, 64, h, rgba);
    return 1;
}

/* 픽셀 아트: rows 의 각 글자를 legend 로 색에 대응. '.' 과 ' ' 은 건너뜀 */
static int op_pixels(forge_ctx *c, const df_json *op) {
    int layer, parts = parts_mask(c, df_json_get(op, "part"));
    int faces = faces_mask(c, df_json_get(op, "faces") ? df_json_get(op, "faces") : df_json_get(op, "face"), 0);
    const df_json *rows = df_json_get(op, "rows");
    if (!faces) return ferr(c, "missing 'face'%s", NULL);
    if (!parts || !layer_of(c, op, &layer)) return 0;
    if (df_json_kind(rows) != DF_JSON_ARRAY) return ferr(c, "'rows' must be an array of strings%s", NULL);
    int ox = get_int(op, "x", 0), oy = get_int(op, "y", 0);
    /* flip: 대칭 면(예: 왼쪽 옆머리)을 같은 그림으로 좌우 반전해 칠한다 */
    int flip = df_json_bool(df_json_get(op, "flip"));
    for (int p = 0; p < DF_PART_COUNT; p++)
        for (int f = 0; f < DF_FACE_COUNT; f++) {
            if (!(parts >> p & 1 && faces >> f & 1)) continue;
            int fx, fy, fw, fh;
            df_face_rect(df_skin_model(c->skin), (df_part)p, (df_layer)layer, (df_face)f, &fx, &fy, &fw, &fh);
            for (size_t r = 0; r < df_json_len(rows); r++) {
                const char *row = df_json_str(df_json_at(rows, r));
                if (!row) return ferr(c, "rows must be strings%s", NULL);
                int len = (int)strlen(row);
                for (int i = 0; i < len; i++) {
                    unsigned char ch = (unsigned char)row[i];
                    if (ch == '.' || ch == ' ') continue;
                    const char *spec = c->legend[ch];
                    if (!spec) {
                        char k[2] = { (char)ch, 0 };
                        return ferr(c, "character '%s' is not in legend", k);
                    }
                    uint8_t rgba[4];
                    if (!resolve_color(c, spec, rgba)) return 0;
                    int lx = flip ? (ox + len - 1 - i) : ox + i, ly = oy + (int)r;
                    if (lx < 0 || ly < 0 || lx >= fw || ly >= fh) continue;
                    put_px(c->skin, fx + lx, fy + ly, rgba);
                }
            }
        }
    return 1;
}

/* 결정적 해시 노이즈로 명도를 살짝 흔든다 (평평한 단색 면을 천/머리결처럼) */
static uint32_t hash3(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static int op_shade(forge_ctx *c, const df_json *op) {
    int layer, parts = parts_mask(c, df_json_get(op, "part"));
    int faces = faces_mask(c, df_json_get(op, "faces"), (1 << DF_FACE_COUNT) - 1);
    if (!parts || !faces || !layer_of(c, op, &layer)) return 0;
    double amount = df_json_num(df_json_get(op, "amount"));
    if (amount <= 0 || amount > 0.5) return ferr(c, "'amount' must be in (0, 0.5]%s", NULL);
    uint32_t seed = (uint32_t)get_int(op, "seed", 1);
    /* only: 이 색과 같은 픽셀만 (예: 코트만 결을 넣고 단추는 그대로) */
    const char *only_spec = df_json_str(df_json_get(op, "only"));
    uint8_t only[4];
    if (only_spec && !resolve_color(c, only_spec, only)) return 0;
    uint8_t *px = df_skin_rgba(c->skin);
    for (int p = 0; p < DF_PART_COUNT; p++)
        for (int f = 0; f < DF_FACE_COUNT; f++) {
            if (!(parts >> p & 1 && faces >> f & 1)) continue;
            int x, y, w, h;
            df_face_rect(df_skin_model(c->skin), (df_part)p, (df_layer)layer, (df_face)f, &x, &y, &w, &h);
            for (int j = y; j < y + h; j++)
                for (int i = x; i < x + w; i++) {
                    uint8_t *q = &px[(j * DF_SKIN_W + i) * 4];
                    if (!q[3] || (only_spec && memcmp(q, only, 4))) continue;
                    double k = 1.0 + amount * ((hash3((uint32_t)i, (uint32_t)j, seed) & 0xFFFF) / 32767.5 - 1.0);
                    for (int ch = 0; ch < 3; ch++) {
                        double v = q[ch] * k;
                        q[ch] = (uint8_t)(v > 255 ? 255 : v + 0.5);
                    }
                }
        }
    return 1;
}

/* 한쪽 팔/다리를 반대쪽에 좌우 반전 복사 */
static int op_mirror(forge_ctx *c, const df_json *op) {
    const char *from = df_json_str(df_json_get(op, "from")), *to = df_json_str(df_json_get(op, "to"));
    int pf = from ? df_part_from_name(from) : -1, pt = to ? df_part_from_name(to) : -1;
    if (pf < 0 || pt < 0) return ferr(c, "mirror needs valid 'from' and 'to' parts%s", NULL);
    int fw, fh, fd, tw, th, td;
    df_part_size(df_skin_model(c->skin), (df_part)pf, &fw, &fh, &fd);
    df_part_size(df_skin_model(c->skin), (df_part)pt, &tw, &th, &td);
    if (fw != tw || fh != th || fd != td) return ferr(c, "mirror parts must have the same size%s", NULL);
    uint8_t *px = df_skin_rgba(c->skin);
    for (int l = 0; l < DF_LAYER_COUNT; l++)
        for (int f = 0; f < DF_FACE_COUNT; f++) {
            int sf = f == DF_FACE_RIGHT ? DF_FACE_LEFT : f == DF_FACE_LEFT ? DF_FACE_RIGHT : f;
            int sx, sy, sw, sh, dx, dy, dw, dh;
            df_face_rect(df_skin_model(c->skin), (df_part)pf, (df_layer)l, (df_face)sf, &sx, &sy, &sw, &sh);
            df_face_rect(df_skin_model(c->skin), (df_part)pt, (df_layer)l, (df_face)f, &dx, &dy, &dw, &dh);
            for (int j = 0; j < dh; j++)
                for (int i = 0; i < dw; i++)
                    memcpy(&px[((dy + j) * DF_SKIN_W + dx + i) * 4], &px[((sy + j) * DF_SKIN_W + sx + sw - 1 - i) * 4], 4);
        }
    return 1;
}

static int run_ops(forge_ctx *c, const df_json *ops) {
    for (size_t i = 0; i < df_json_len(ops); i++) {
        const df_json *op = df_json_at(ops, i);
        c->op_index = (int)i;
        c->op_name = df_json_str(df_json_get(op, "op"));
        if (!c->op_name) { c->op_name = "?"; return ferr(c, "missing 'op'%s", NULL); }
        int ok;
        if (!strcmp(c->op_name, "fill")) ok = op_fill(c, op);
        else if (!strcmp(c->op_name, "rect")) ok = op_rect(c, op);
        else if (!strcmp(c->op_name, "band")) ok = op_band(c, op);
        else if (!strcmp(c->op_name, "pixels")) ok = op_pixels(c, op);
        else if (!strcmp(c->op_name, "shade")) ok = op_shade(c, op);
        else if (!strcmp(c->op_name, "mirror")) ok = op_mirror(c, op);
        else if (!strcmp(c->op_name, "note")) ok = 1; /* 사람용 주석 */
        else ok = ferr(c, "unknown op '%s' (fill, rect, band, pixels, shade, mirror, note)", c->op_name);
        if (!ok) return 0;
    }
    c->op_name = NULL;
    return 1;
}

/* ---- 파일 / extends 체인 ---- */

/* strdup 은 C99 표준이 아니다 (POSIX). 엄격한 -std=c99 빌드에서도 안전하도록 직접 구현 */
static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 16 * 1024 * 1024) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) { buf[n] = 0; *len = (size_t)n; }
    return buf;
}

static char *dir_of(const char *path) {
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bs = strrchr(path, '\\');
    if (bs > slash) slash = bs;
#endif
    size_t n = slash ? (size_t)(slash - path) : 1;
    char *d = malloc(n + 1);
    if (!d) return NULL;
    if (slash) memcpy(d, path, n); else d[0] = '.';
    d[n] = 0;
    return d;
}

static char *join(const char *dir, const char *rel) {
    if (rel[0] == '/' || !dir) return dup_str(rel);
    size_t n = strlen(dir) + strlen(rel) + 2;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s/%s", dir, rel);
    return p;
}

/* "a/b/../c/./d" → "a/c/d". 같은 부모를 다른 경로로 가리켜도 한 번만 실행하기 위해 쓴다 */
static char *normalize_path(const char *p) {
    char *buf = dup_str(p), *out = malloc(strlen(p) + 2);
    const char *segs[256];
    int ns = 0;
    if (!buf || !out) { free(buf); free(out); return NULL; }
    for (char *tok = buf, *next; tok; tok = next) {
        next = strchr(tok, '/');
        if (next) *next++ = 0;
        if (!*tok || !strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..") && ns > 0 && strcmp(segs[ns - 1], "..")) { ns--; continue; }
        if (ns < 256) segs[ns++] = tok;
    }
    char *o = out;
    if (p[0] == '/') *o++ = '/';
    for (int i = 0; i < ns; i++) {
        size_t n = strlen(segs[i]);
        if (i) *o++ = '/';
        memcpy(o, segs[i], n);
        o += n;
    }
    if (o == out) *o++ = '.';
    *o = 0;
    free(buf);
    return out;
}

typedef struct {
    df_json *docs[MAX_CHAIN];
    char *names[MAX_CHAIN];
    int n;
} chain;

static void chain_free(chain *ch) {
    for (int i = 0; i < ch->n; i++) { df_json_free(ch->docs[i]); free(ch->names[i]); }
}

static int in_chain(const chain *ch, const char *name) {
    for (int i = 0; i < ch->n; i++) if (!strcmp(ch->names[i], name)) return 1;
    return 0;
}

/*
 * extends 그래프를 깊이 우선으로 펼쳐 실행 순서를 만든다: 부모들(적힌 순서대로) → 자신.
 * "extends": "a.dolly.json" 또는 ["얼굴.dolly.json", "의상.dolly.json"].
 * 여러 부모가 같은 조상을 공유하면 그 조상은 처음 한 번만 들어간다.
 * doc 의 소유권을 가져간다.
 */
static int visit(forge_ctx *c, chain *ch, df_json *doc, const char *name, const char *dir, int depth) {
    c->where = name;
    if (depth >= MAX_CHAIN) { df_json_free(doc); return ferr(c, "extends nested too deep (cycle?)%s", NULL); }
    if (df_json_kind(doc) != DF_JSON_OBJECT) { df_json_free(doc); return ferr(c, "recipe must be a JSON object%s", NULL); }
    const df_json *ver = df_json_get(doc, "dollyforge");
    if (ver && df_json_num(ver) != 1) {
        df_json_free(doc);
        return ferr(c, "unsupported recipe version (expected \"dollyforge\": 1)%s", NULL);
    }
    const df_json *ext = df_json_get(doc, "extends");
    int is_array = df_json_kind(ext) == DF_JSON_ARRAY;
    if (ext && !is_array && df_json_kind(ext) != DF_JSON_STRING) {
        df_json_free(doc);
        return ferr(c, "'extends' must be a file name or an array of file names%s", NULL);
    }
    size_t nparents = is_array ? df_json_len(ext) : ext ? 1 : 0;
    for (size_t i = 0; i < nparents; i++) {
        const char *rel = df_json_str(is_array ? df_json_at(ext, i) : ext);
        if (!rel) { df_json_free(doc); return ferr(c, "'extends' entries must be strings%s", NULL); }
        char *joined = join(dir, rel), *path = joined ? normalize_path(joined) : NULL;
        free(joined);
        if (!path) { df_json_free(doc); return 0; }
        if (in_chain(ch, path)) { free(path); continue; }
        size_t len;
        char *text = read_file(path, &len);
        if (!text) { int r = ferr(c, "cannot read extends '%s'", rel); free(path); df_json_free(doc); return r; }
        char jerr[160];
        df_json *parent = df_json_parse(text, len, jerr, sizeof jerr);
        free(text);
        if (!parent) { c->where = path; ferr(c, "%s", jerr); free(path); df_json_free(doc); return 0; }
        char *pdir = dir_of(path);
        int ok = visit(c, ch, parent, path, pdir, depth + 1);
        free(pdir);
        free(path);
        if (!ok) { df_json_free(doc); return 0; }
        c->where = name;
    }
    if (ch->n == MAX_CHAIN) { df_json_free(doc); return ferr(c, "more than 16 recipes in the extends graph%s", NULL); }
    ch->docs[ch->n] = doc;
    ch->names[ch->n] = dup_str(name);
    ch->n++;
    c->where = ch->names[ch->n - 1];
    return 1;
}

/* 체인 전체의 model/palette/legend 를 합치고 팔레트가 모두 해석되는지 확인 */
static int merge_chain(chain *ch, forge_ctx *c, df_model *model) {
    *model = DF_MODEL_CLASSIC;
    for (int i = 0; i < ch->n; i++) {
        c->where = ch->names[i];
        const char *m = df_json_str(df_json_get(ch->docs[i], "model"));
        if (m && !strcmp(m, "slim")) *model = DF_MODEL_SLIM;
        else if (m && !strcmp(m, "classic")) *model = DF_MODEL_CLASSIC;
        else if (m) return ferr(c, "unknown model '%s' (classic, slim)", m);
        const df_json *pal = df_json_get(ch->docs[i], "palette");
        for (size_t k = 0; k < df_json_len(pal); k++) {
            if (!strncmp(df_json_key_at(pal, k), "//", 2)) continue; /* "// 메모": 주석 키 */
            const char *v = df_json_str(df_json_at(pal, k));
            if (!v) return ferr(c, "palette '%s' must be a string", df_json_key_at(pal, k));
            if (c->npal == MAX_PALETTE) return ferr(c, "palette larger than 256 entries%s", NULL);
            c->pal[c->npal].name = df_json_key_at(pal, k);
            c->pal[c->npal].value = v;
            c->npal++;
        }
        const df_json *leg = df_json_get(ch->docs[i], "legend");
        for (size_t k = 0; k < df_json_len(leg); k++) {
            const char *key = df_json_key_at(leg, k), *v = df_json_str(df_json_at(leg, k));
            if (strlen(key) != 1 || !v) return ferr(c, "legend key '%s' must be one character mapped to a color", key);
            c->legend[(unsigned char)key[0]] = v;
        }
    }
    /* 팔레트가 전부 해석되는지 먼저 확인 (안 쓰는 색의 오타도 잡는다) */
    for (int i = 0; i < c->npal; i++) {
        uint8_t tmp[4];
        c->where = "palette";
        if (!resolve_color(c, c->pal[i].value, tmp)) return 0;
    }
    return 1;
}

static df_skin *forge(chain *ch, forge_ctx *c) {
    df_model model;
    if (!merge_chain(ch, c, &model)) return NULL;
    c->skin = df_skin_new(model);
    if (!c->skin) return NULL;
    for (int i = 0; i < ch->n; i++) {
        c->where = ch->names[i];
        if (!run_ops(c, df_json_get(ch->docs[i], "ops"))) { df_skin_free(c->skin); return NULL; }
    }
    return c->skin;
}

static df_skin *forge_doc(df_json *doc, const char *name, const char *dir, char *err, size_t errlen) {
    forge_ctx *c = calloc(1, sizeof *c);
    chain ch = { 0 };
    if (!c) { df_json_free(doc); return NULL; }
    c->err = err;
    c->errlen = errlen;
    char *norm = normalize_path(name);
    df_skin *skin = norm && visit(c, &ch, doc, norm, dir, 0) ? forge(&ch, c) : NULL;
    if (!norm) df_json_free(doc);
    free(norm);
    chain_free(&ch);
    free(c);
    return skin;
}

df_skin *df_forge_json(const char *json, size_t len, const char *base_dir, char *err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    df_json *doc = df_json_parse(json, len, err, errlen);
    if (!doc) return NULL;
    return forge_doc(doc, "<recipe>", base_dir ? base_dir : ".", err, errlen);
}

df_skin *df_forge_file(const char *path, char *err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    size_t len;
    char *text = read_file(path, &len);
    if (!text) { snprintf(err, errlen, "cannot read %s", path); return NULL; }
    char jerr[160];
    df_json *doc = df_json_parse(text, len, jerr, sizeof jerr);
    free(text);
    if (!doc) { snprintf(err, errlen, "%s: %s", path, jerr); return NULL; }
    char *dir = dir_of(path);
    df_skin *skin = forge_doc(doc, path, dir, err, errlen);
    free(dir);
    return skin;
}

/* ---- 내부 API (internal.h): 곡선 3D 엔진이 같은 레시피·팔레트를 쓴다 ---- */

struct df_recipe {
    forge_ctx *c;
    chain ch;
};

static df_recipe *recipe_from_doc(df_json *doc, const char *name, const char *dir, char *err, size_t errlen) {
    df_recipe *r = calloc(1, sizeof *r);
    if (!r || !(r->c = calloc(1, sizeof *r->c))) { free(r); df_json_free(doc); return NULL; }
    r->c->err = err;
    r->c->errlen = errlen;
    char *norm = normalize_path(name);
    df_model model;
    int ok = norm && visit(r->c, &r->ch, doc, norm, dir, 0) && merge_chain(&r->ch, r->c, &model);
    if (!norm) df_json_free(doc);
    free(norm);
    if (!ok) { df__recipe_free(r); return NULL; }
    return r;
}

df_recipe *df__recipe_file(const char *path, char *err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    size_t len;
    char *text = read_file(path, &len);
    if (!text) { snprintf(err, errlen, "cannot read %s", path); return NULL; }
    char jerr[160];
    df_json *doc = df_json_parse(text, len, jerr, sizeof jerr);
    free(text);
    if (!doc) { snprintf(err, errlen, "%s: %s", path, jerr); return NULL; }
    char *dir = dir_of(path);
    df_recipe *r = recipe_from_doc(doc, path, dir, err, errlen);
    free(dir);
    return r;
}

df_recipe *df__recipe_json(const char *json, size_t len, const char *base_dir, char *err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    df_json *doc = df_json_parse(json, len, err, errlen);
    if (!doc) return NULL;
    return recipe_from_doc(doc, "<recipe>", base_dir ? base_dir : ".", err, errlen);
}

void df__recipe_free(df_recipe *r) {
    if (!r) return;
    chain_free(&r->ch);
    free(r->c);
    free(r);
}

int df__recipe_color(df_recipe *r, const char *spec, uint8_t rgba[4]) {
    char *saved = r->c->err;
    r->c->err = NULL; /* 조회 실패는 호출자가 기본값으로 처리 */
    int ok = resolve_color(r->c, spec, rgba);
    r->c->err = saved;
    return ok;
}

const df_json *df__recipe_doll(const df_recipe *r, const char *section, const char *key) {
    for (int i = r->ch.n; i-- > 0;) {
        const df_json *sec = df_json_get(df_json_get(r->ch.docs[i], "doll"), section);
        const df_json *v = key ? df_json_get(sec, key) : sec;
        if (v) return v;
    }
    return NULL;
}
