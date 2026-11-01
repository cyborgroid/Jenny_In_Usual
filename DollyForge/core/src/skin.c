/* 스킨 데이터, UV 레이아웃, 검증, 미리보기, 비교 */
#include "dollyforge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct df_skin {
    df_model model;
    uint8_t rgba[DF_SKIN_BYTES];
};

const char *df_version(void) { return "0.1.0"; }

/* 파트별 base/overlay 텍스처 원점 (Java 1.8+ / Bedrock 공통 64x64 규격) */
static const int ORIGIN[DF_PART_COUNT][DF_LAYER_COUNT][2] = {
    [DF_PART_HEAD]  = { { 0, 0 },   { 32, 0 } },
    [DF_PART_BODY]  = { { 16, 16 }, { 16, 32 } },
    [DF_PART_ARM_R] = { { 40, 16 }, { 40, 32 } },
    [DF_PART_ARM_L] = { { 32, 48 }, { 48, 48 } },
    [DF_PART_LEG_R] = { { 0, 16 },  { 0, 32 } },
    [DF_PART_LEG_L] = { { 16, 48 }, { 0, 48 } },
};

static const char *PART_NAMES[DF_PART_COUNT] = { "head", "body", "arm_r", "arm_l", "leg_r", "leg_l" };
static const char *FACE_NAMES[DF_FACE_COUNT] = { "top", "bottom", "right", "front", "left", "back" };

const char *df_part_name(df_part part) { return part >= 0 && part < DF_PART_COUNT ? PART_NAMES[part] : "?"; }
const char *df_face_name(df_face face) { return face >= 0 && face < DF_FACE_COUNT ? FACE_NAMES[face] : "?"; }

int df_part_from_name(const char *name) {
    for (int i = 0; i < DF_PART_COUNT; i++) if (!strcmp(name, PART_NAMES[i])) return i;
    return -1;
}

int df_face_from_name(const char *name) {
    for (int i = 0; i < DF_FACE_COUNT; i++) if (!strcmp(name, FACE_NAMES[i])) return i;
    return -1;
}

void df_part_size(df_model model, df_part part, int *w, int *h, int *d) {
    switch (part) {
    case DF_PART_HEAD: *w = 8; *h = 8; *d = 8; break;
    case DF_PART_BODY: *w = 8; *h = 12; *d = 4; break;
    case DF_PART_ARM_R:
    case DF_PART_ARM_L: *w = model == DF_MODEL_SLIM ? 3 : 4; *h = 12; *d = 4; break;
    default: *w = 4; *h = 12; *d = 4; break;
    }
}

int df_face_rect(df_model model, df_part part, df_layer layer, df_face face, int *x, int *y, int *w, int *h) {
    if (part < 0 || part >= DF_PART_COUNT || layer < 0 || layer >= DF_LAYER_COUNT || face < 0 || face >= DF_FACE_COUNT)
        return 0;
    int bw, bh, bd;
    df_part_size(model, part, &bw, &bh, &bd);
    int u = ORIGIN[part][layer][0], v = ORIGIN[part][layer][1];
    /* 박스 전개도: 위에 top/bottom, 아래에 right/front/left/back */
    switch (face) {
    case DF_FACE_TOP:    *x = u + bd;          *y = v;      *w = bw; *h = bd; break;
    case DF_FACE_BOTTOM: *x = u + bd + bw;     *y = v;      *w = bw; *h = bd; break;
    case DF_FACE_RIGHT:  *x = u;               *y = v + bd; *w = bd; *h = bh; break;
    case DF_FACE_FRONT:  *x = u + bd;          *y = v + bd; *w = bw; *h = bh; break;
    case DF_FACE_LEFT:   *x = u + bd + bw;     *y = v + bd; *w = bd; *h = bh; break;
    case DF_FACE_BACK:   *x = u + 2 * bd + bw; *y = v + bd; *w = bw; *h = bh; break;
    default: return 0;
    }
    return 1;
}

/* ---- 스킨 ---- */

df_skin *df_skin_new(df_model model) {
    df_skin *s = calloc(1, sizeof *s);
    if (s) s->model = model;
    return s;
}

void df_skin_free(df_skin *skin) { free(skin); }
df_model df_skin_model(const df_skin *skin) { return skin->model; }
void df_skin_set_model(df_skin *skin, df_model model) { skin->model = model; }
uint8_t *df_skin_rgba(df_skin *skin) { return skin->rgba; }
const uint8_t *df_skin_rgba_const(const df_skin *skin) { return skin->rgba; }

df_model df_detect_model(const uint8_t *rgba) {
    /* slim 팔은 오른팔 영역의 x=54..55(y 20..31) 와 x=50..51(y 16..19) 를 쓰지 않는다 */
    for (int y = 16; y < 32; y++)
        for (int x = (y < 20 ? 50 : 54); x < (y < 20 ? 52 : 56); x++)
            if (rgba[(y * DF_SKIN_W + x) * 4 + 3]) return DF_MODEL_CLASSIC;
    return DF_MODEL_SLIM;
}

int df_skin_save_png(const df_skin *skin, const char *path, char *err, size_t errlen) {
    uint8_t *png;
    size_t len;
    if (!df_png_encode(skin->rgba, DF_SKIN_W, DF_SKIN_H, &png, &len)) {
        snprintf(err, errlen, "PNG encode failed");
        return 0;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { free(png); snprintf(err, errlen, "cannot write %s", path); return 0; }
    int ok = fwrite(png, 1, len, f) == len;
    ok &= fclose(f) == 0;
    free(png);
    if (!ok) snprintf(err, errlen, "write failed: %s", path);
    return ok;
}

/* 구형 64x32 스킨: 왼팔·왼다리를 오른쪽에서 좌우 반전해 채운다 */
static void copy_mirrored(df_skin *s, df_part from, df_part to) {
    for (int f = 0; f < DF_FACE_COUNT; f++) {
        int src = f == DF_FACE_RIGHT ? DF_FACE_LEFT : f == DF_FACE_LEFT ? DF_FACE_RIGHT : f;
        int sx, sy, sw, sh, dx, dy, dw, dh;
        df_face_rect(s->model, from, DF_LAYER_BASE, (df_face)src, &sx, &sy, &sw, &sh);
        df_face_rect(s->model, to, DF_LAYER_BASE, (df_face)f, &dx, &dy, &dw, &dh);
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++)
                memcpy(&s->rgba[((dy + y) * DF_SKIN_W + dx + x) * 4],
                       &s->rgba[((sy + y) * DF_SKIN_W + sx + sw - 1 - x) * 4], 4);
    }
}

df_skin *df_skin_load_png(const char *path, char *err, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "cannot open %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64 * 1024 * 1024) { fclose(f); snprintf(err, errlen, "bad file size: %s", path); return NULL; }
    uint8_t *data = malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(data); snprintf(err, errlen, "read failed: %s", path); return NULL; }
    fclose(f);
    uint8_t *rgba;
    int w, h;
    int ok = df_png_decode(data, (size_t)n, &rgba, &w, &h, err, errlen);
    free(data);
    if (!ok) return NULL;
    if (w != 64 || (h != 64 && h != 32)) {
        free(rgba);
        snprintf(err, errlen, "skin must be 64x64 or 64x32, got %dx%d", w, h);
        return NULL;
    }
    df_skin *s = df_skin_new(DF_MODEL_CLASSIC);
    if (!s) { free(rgba); return NULL; }
    memcpy(s->rgba, rgba, (size_t)w * h * 4);
    free(rgba);
    if (h == 32) {
        copy_mirrored(s, DF_PART_ARM_R, DF_PART_ARM_L);
        copy_mirrored(s, DF_PART_LEG_R, DF_PART_LEG_L);
    } else {
        s->model = df_detect_model(s->rgba);
    }
    return s;
}

/* ---- 영역 맵: 각 픽셀이 어떤 파트/레이어에 속하는지 ---- */

static void region_map(df_model model, signed char part_of[DF_SKIN_H][DF_SKIN_W], signed char layer_of[DF_SKIN_H][DF_SKIN_W]) {
    memset(part_of, -1, DF_SKIN_W * DF_SKIN_H);
    memset(layer_of, -1, DF_SKIN_W * DF_SKIN_H);
    for (int p = 0; p < DF_PART_COUNT; p++)
        for (int l = 0; l < DF_LAYER_COUNT; l++)
            for (int f = 0; f < DF_FACE_COUNT; f++) {
                int x, y, w, h;
                df_face_rect(model, (df_part)p, (df_layer)l, (df_face)f, &x, &y, &w, &h);
                for (int j = y; j < y + h; j++)
                    for (int i = x; i < x + w; i++) { part_of[j][i] = (signed char)p; layer_of[j][i] = (signed char)l; }
            }
}

static size_t appendf(char *buf, size_t buflen, size_t used, const char *fmt, const char *a, const char *b, int n) {
    if (!buf || used >= buflen) return used;
    int k = snprintf(buf + used, buflen - used, fmt, a, b, n);
    return k < 0 ? used : used + (size_t)k;
}

int df_validate(const df_skin *skin, char *buf, size_t buflen) {
    int errors = 0;
    size_t used = 0;
    if (buf && buflen) buf[0] = 0;
    for (int p = 0; p < DF_PART_COUNT; p++)
        for (int f = 0; f < DF_FACE_COUNT; f++) {
            int x, y, w, h, holes = 0, partial = 0, empty_overlay = 0;
            df_face_rect(skin->model, (df_part)p, DF_LAYER_BASE, (df_face)f, &x, &y, &w, &h);
            for (int j = y; j < y + h; j++)
                for (int i = x; i < x + w; i++)
                    if (skin->rgba[(j * DF_SKIN_W + i) * 4 + 3] != 255) holes++;
            if (holes) {
                used = appendf(buf, buflen, used, "ERROR base %s/%s: %d transparent pixel(s) (shows as holes or black in game)\n",
                               PART_NAMES[p], FACE_NAMES[f], holes);
                errors++;
            }
            df_face_rect(skin->model, (df_part)p, DF_LAYER_OVERLAY, (df_face)f, &x, &y, &w, &h);
            for (int j = y; j < y + h; j++)
                for (int i = x; i < x + w; i++) {
                    uint8_t a = skin->rgba[(j * DF_SKIN_W + i) * 4 + 3];
                    if (a && a != 255) partial++;
                    if (!a) empty_overlay++;
                }
            (void)empty_overlay;
            if (partial)
                used = appendf(buf, buflen, used, "WARN overlay %s/%s: %d semi-transparent pixel(s) (editions render these differently)\n",
                               PART_NAMES[p], FACE_NAMES[f], partial);
        }
    signed char part_of[DF_SKIN_H][DF_SKIN_W], layer_of[DF_SKIN_H][DF_SKIN_W];
    region_map(skin->model, part_of, layer_of);
    int stray = 0;
    for (int j = 0; j < DF_SKIN_H; j++)
        for (int i = 0; i < DF_SKIN_W; i++)
            if (part_of[j][i] < 0 && skin->rgba[(j * DF_SKIN_W + i) * 4 + 3]) stray++;
    if (stray)
        used = appendf(buf, buflen, used, "WARN %s%s%d pixel(s) painted outside the skin layout (ignored by the game)\n", "", "", stray);
    return errors;
}

/* ---- 미리보기 ---- */

static void blit_face(const df_skin *s, df_part part, df_layer layer, df_face face, uint8_t *out, int ox, int oy) {
    int x, y, w, h;
    df_face_rect(s->model, part, layer, face, &x, &y, &w, &h);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            const uint8_t *src = &s->rgba[((y + j) * DF_SKIN_W + x + i) * 4];
            uint8_t *dst = &out[((oy + j) * 16 + ox + i) * 4];
            int a = src[3];
            if (!a) continue;
            for (int c = 0; c < 3; c++) dst[c] = (uint8_t)((src[c] * a + dst[c] * (255 - a)) / 255);
            dst[3] = (uint8_t)(a + dst[3] * (255 - a) / 255);
        }
}

void df_render_flat(const df_skin *skin, int view, uint8_t *out) {
    memset(out, 0, 16 * 32 * 4);
    int aw, ah, ad;
    df_part_size(skin->model, DF_PART_ARM_R, &aw, &ah, &ad);
    df_face face = view ? DF_FACE_BACK : DF_FACE_FRONT;
    /* 정면에서는 캐릭터의 오른팔·오른다리가 보는 사람의 왼쪽에 온다 */
    df_part left_arm = view ? DF_PART_ARM_L : DF_PART_ARM_R, right_arm = view ? DF_PART_ARM_R : DF_PART_ARM_L;
    df_part left_leg = view ? DF_PART_LEG_L : DF_PART_LEG_R, right_leg = view ? DF_PART_LEG_R : DF_PART_LEG_L;
    for (int l = 0; l < DF_LAYER_COUNT; l++) {
        blit_face(skin, DF_PART_HEAD, (df_layer)l, face, out, 4, 0);
        blit_face(skin, DF_PART_BODY, (df_layer)l, face, out, 4, 8);
        blit_face(skin, left_arm, (df_layer)l, face, out, 4 - aw, 8);
        blit_face(skin, right_arm, (df_layer)l, face, out, 12, 8);
        blit_face(skin, left_leg, (df_layer)l, face, out, 4, 20);
        blit_face(skin, right_leg, (df_layer)l, face, out, 8, 20);
    }
}

/* ---- 비교 ---- */

int df_diff(const df_skin *a, const df_skin *b, int diff[DF_PART_COUNT]) {
    signed char part_of[DF_SKIN_H][DF_SKIN_W], layer_of[DF_SKIN_H][DF_SKIN_W];
    region_map(a->model, part_of, layer_of);
    int total = 0;
    for (int p = 0; p < DF_PART_COUNT; p++) diff[p] = 0;
    for (int j = 0; j < DF_SKIN_H; j++)
        for (int i = 0; i < DF_SKIN_W; i++) {
            const uint8_t *pa = &a->rgba[(j * DF_SKIN_W + i) * 4], *pb = &b->rgba[(j * DF_SKIN_W + i) * 4];
            /* 완전히 투명한 픽셀은 RGB 값이 달라도 같은 것으로 본다 */
            int same = pa[3] == 0 && pb[3] == 0 ? 1 : !memcmp(pa, pb, 4);
            if (same) continue;
            total++;
            if (part_of[j][i] >= 0) diff[part_of[j][i]]++;
        }
    if (a->model != b->model) total += 1; /* 팔 두께가 다르면 같은 스킨이 아니다 */
    return total;
}
