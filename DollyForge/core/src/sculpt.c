/*
 * 곡선 3D 캐릭터 (doll) — 부호 거리장(SDF)으로 만든 매끈한 몸과 곡선 메이크업.
 *
 * 체형은 03_Sources/3D_Models/JennyCartoon3D.jpg 실루엣에서 잰 치수(키 1.6m 환산)를
 * Catmull-Rom 단면 보간으로 잇는다. 그래서 허리→골반→허벅지 곡선이 레퍼런스 그대로 흐른다.
 * 메이크업(눈매·속눈썹 윙·홍채·눈썹 아치·볼 홍조·입술)은 얼굴 위의 곡선으로 정의된다.
 * 색은 스킨 레시피와 같은 팔레트를 쓴다 — 한 레시피가 곡선 3D 와 64x64 스킨을 함께 만든다.
 *
 * 좌표: 미터, y 위, 바닥 y=0, 캐릭터는 +z 를 바라본다, 캐릭터의 오른쪽 = -x.
 */
#define _POSIX_C_SOURCE 200809L
#include "dollyforge.h"
#include "internal.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------- 벡터 ---------------- */

typedef struct { float x, y, z; } v3;
static inline v3 V(float x, float y, float z) { v3 r = { x, y, z }; return r; }
static inline v3 add(v3 a, v3 b) { return V(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline v3 sub(v3 a, v3 b) { return V(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline v3 mul(v3 a, float s) { return V(a.x * s, a.y * s, a.z * s); }
static inline float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float len(v3 a) { return sqrtf(dot(a, a)); }
static inline v3 nrm(v3 a) { float l = len(a); return l > 0 ? mul(a, 1.0f / l) : a; }
static inline v3 cross(v3 a, v3 b) { return V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float mixf(float a, float b, float t) { return a + (b - a) * t; }
static inline v3 mixv(v3 a, v3 b, float t) { return V(mixf(a.x, b.x, t), mixf(a.y, b.y, t), mixf(a.z, b.z, t)); }
static inline float smoothstepf(float a, float b, float x) { float t = clampf((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }

/* 다항식 smooth min — 부위 사이를 이음매 없이 잇는다 */
static inline float smin(float a, float b, float k) {
    if (b - a >= k) return a;      /* 멀리 떨어진 값끼리는 그냥 min (부동소수 상쇄 방지) */
    if (a - b >= k) return b;
    float h = 0.5f + 0.5f * (b - a) / k;
    return a * h + b * (1 - h) - k * h * (1 - h);
}

/* ---------------- SDF 기본형 ---------------- */

static float sd_ellipsoid(v3 p, v3 r) {
    float k0 = len(V(p.x / r.x, p.y / r.y, p.z / r.z));
    float k1 = len(V(p.x / (r.x * r.x), p.y / (r.y * r.y), p.z / (r.z * r.z)));
    return k1 > 0 ? k0 * (k0 - 1) / k1 : -fminf(r.x, fminf(r.y, r.z));
}

/* 반지름이 a→b 로 변하는 둥근 원뿔 (팔·다리·머리카락 가닥) */
static float sd_round_cone(v3 p, v3 a, v3 b, float r1, float r2) {
    v3 ba = sub(b, a);
    float l2 = dot(ba, ba), rr = r1 - r2, a2 = l2 - rr * rr, il2 = 1.0f / l2;
    v3 pa = sub(p, a);
    float y = dot(pa, ba), z = y - l2;
    v3 xv = sub(mul(pa, l2), mul(ba, y));
    float x2 = dot(xv, xv), y2 = y * y * l2, z2 = z * z * l2;
    float k = (rr > 0 ? 1.0f : -1.0f) * rr * rr * x2;
    if ((z > 0 ? 1.0f : -1.0f) * a2 * z2 > k) return sqrtf(x2 + z2) * il2 - r2;
    if ((y > 0 ? 1.0f : -1.0f) * a2 * y2 < k) return sqrtf(x2 + y2) * il2 - r1;
    return (sqrtf(x2 * a2 * il2) + y * rr) * il2 - r1;
}

static float sd_round_box(v3 p, v3 b, float r) {
    v3 q = V(fabsf(p.x) - b.x + r, fabsf(p.y) - b.y + r, fabsf(p.z) - b.z + r);
    v3 m = V(fmaxf(q.x, 0), fmaxf(q.y, 0), fmaxf(q.z, 0));
    return len(m) + fminf(fmaxf(q.x, fmaxf(q.y, q.z)), 0) - r;
}

/* y 축을 감는 도넛을 축 방향 dir 로 세운 것 (소맷부리, 스트랩, 칼라) */
static float sd_ring(v3 p, v3 c, v3 axis, float R, float r) {
    v3 q = sub(p, c);
    float h = dot(q, axis);
    v3 radial = sub(q, mul(axis, h));
    float d = len(radial) - R;
    return sqrtf(d * d + h * h) - r;
}

/* ---------------- 단면 로프트 (몸통·코트 자락) ---------------- */

typedef struct { float y, a, b, c; } section; /* 높이, 좌우 반폭, 앞뒤 반폭, 앞뒤 중심 */

/* Catmull-Rom 으로 단면을 보간 — 랜드마크 사이 곡선이 부드럽게 이어진다 */
static section interp(const section *s, int n, float y) {
    if (y >= s[0].y) return s[0];
    if (y <= s[n - 1].y) return s[n - 1];
    int i = 0;
    while (i < n - 2 && y < s[i + 1].y) i++;
    const section *p0 = &s[i > 0 ? i - 1 : 0], *p1 = &s[i], *p2 = &s[i + 1], *p3 = &s[i + 2 < n ? i + 2 : n - 1];
    float t = (p1->y - y) / (p1->y - p2->y), t2 = t * t, t3 = t2 * t;
    section r;
    r.y = y;
#define CR(f) (0.5f * (2 * p1->f + (-p0->f + p2->f) * t + (2 * p0->f - 5 * p1->f + 4 * p2->f - p3->f) * t2 + (-p0->f + 3 * p1->f - 3 * p2->f + p3->f) * t3))
    r.a = CR(a); r.b = CR(b); r.c = CR(c);
#undef CR
    return r;
}

static float sd_loft(v3 p, const section *s, int n, float grow) {
    section q = interp(s, n, p.y);
    float a = q.a + grow, b = q.b + grow;
    float x = p.x, z = p.z - q.c;
    float k0 = sqrtf((x / a) * (x / a) + (z / b) * (z / b));
    float k1 = sqrtf((x / (a * a)) * (x / (a * a)) + (z / (b * b)) * (z / (b * b)));
    float dr = k1 > 0 ? k0 * (k0 - 1) / k1 : -fminf(a, b);
    float dy = fmaxf(p.y - s[0].y, s[n - 1].y - p.y);
    if (dy > 0) return sqrtf(fmaxf(dr, 0) * fmaxf(dr, 0) + dy * dy);
    return fmaxf(dr, dy);
}

/* ---------------- 재질 ---------------- */

enum {
    M_SKIN, M_HAIR, M_PETAL, M_PISTIL, M_TOP, M_COAT, M_EDGE, M_BELT, M_METAL, M_GLOVE,
    M_LEGS, M_BOOT, M_TRIM, M_SOLE, M_SHORTS, M_SOCKS, M_STRING, M_INNER, M_BAG, M_VEIL, M_COUNT
};

enum { OUTFIT_CYBER, OUTFIT_CASUAL, OUTFIT_DRESSUP, OUTFIT_WEDDING };
enum { HAIR_LONG, HAIR_BOB, HAIR_WAVY };

struct df_doll {
    v3 col[M_COUNT];                 /* 선형 RGB */
    v3 c_blush, c_mouth, c_brow, c_lash, c_eye, c_eye_light, c_eye_white, c_hair_dark;
    int outfit, hair;
    float flower_side;               /* -1: 캐릭터 오른쪽(보는 사람 왼쪽) */
    /* 체형 (1 = 레퍼런스 그대로) */
    float bust, waist, hips, thighs;
    /* 메이크업 */
    float eye_size, eye_open, eye_tilt, lash, brow_arch, blush, lip_curve, gloss;
    section torso[11], skirt[6], dress[16], gown[7];
    int ndress, ngown;
    int veil_on;
    struct df_doll *noveil;  /* 베일만 뺀 사본: 반투명 베일 뒤를 그릴 때 쓴다 */
    int ntorso, nskirt;
    v3 flower_c, flower_n, flower_u, flower_v;
};

/* ---------------- 몸 ---------------- */

static const v3 SHOULDER = { 0.168f, 1.215f, -0.010f }, ELBOW = { 0.218f, 0.985f, -0.022f }, WRIST = { 0.242f, 0.790f, 0.004f };
static const v3 HIP = { 0.085f, 0.800f, 0.0f }, KNEE = { 0.063f, 0.470f, 0.012f }, ANKLE = { 0.060f, 0.090f, -0.010f };

static inline v3 mx(v3 p) { return V(fabsf(p.x), p.y, p.z); } /* 좌우 대칭 */

typedef struct { float d; int m; } hit;
static inline void take(hit *h, float d, int m) { if (d < h->d) { h->d = d; h->m = m; } }

static float sd_head(v3 p) {
    float d = sd_ellipsoid(sub(p, V(0, 1.430f, 0)), V(0.110f, 0.128f, 0.108f));
    d = smin(d, sd_ellipsoid(sub(p, V(0, 1.366f, 0.026f)), V(0.090f, 0.060f, 0.084f)), 0.04f); /* 볼 */
    /* 광대 아래로 갸름하게 좁아지는 턱선 */
    float jaw = (fabsf(p.x) - (0.114f - 0.70f * fmaxf(0.0f, 1.385f - p.y))) * 0.82f;
    d = -smin(-d, -jaw, 0.025f);
    d = smin(d, sd_ellipsoid(sub(p, V(0, 1.360f, 0.105f)), V(0.010f, 0.013f, 0.012f)), 0.012f); /* 코 */
    return d;
}

static float sd_arm(v3 q) { /* q = 대칭 좌표 */
    float d = sd_round_cone(q, SHOULDER, ELBOW, 0.045f, 0.036f);
    return smin(d, sd_round_cone(q, ELBOW, WRIST, 0.036f, 0.027f), 0.02f);
}

static float sd_hand(v3 q) {
    float d = sd_ellipsoid(sub(q, add(WRIST, V(0.006f, -0.058f, 0.010f))), V(0.022f, 0.050f, 0.034f));
    d = smin(d, sd_round_cone(q, add(WRIST, V(-0.006f, -0.020f, 0.026f)), add(WRIST, V(-0.012f, -0.058f, 0.040f)), 0.010f, 0.008f), 0.01f);
    return smin(d, sd_round_cone(q, WRIST, add(WRIST, V(0, -0.02f, 0.008f)), 0.027f, 0.026f), 0.015f);
}

static float sd_leg(const struct df_doll *dl, v3 q) {
    float t = dl->thighs;
    float d = sd_round_cone(q, HIP, KNEE, 0.086f * t, 0.050f);
    d = smin(d, sd_ellipsoid(sub(q, V(0.083f, 0.670f, 0.004f)), V(0.079f * t, 0.145f, 0.077f * t)), 0.04f);
    d = smin(d, sd_round_cone(q, KNEE, ANKLE, 0.050f, 0.033f), 0.02f);
    d = smin(d, sd_ellipsoid(sub(q, V(0.063f, 0.375f, -0.012f)), V(0.051f, 0.105f, 0.052f)), 0.03f); /* 종아리 */
    return d;
}

/* ---------------- 머리카락 ---------------- */

static float sd_hair(const struct df_doll *dl, v3 p, int *flower_mat) {
    float s = dl->flower_side;          /* 가르마 쪽 */
    /* 정수리에 매끈하게 붙는 캡 */
    float cap = sd_ellipsoid(sub(p, V(0, 1.452f, -0.012f)), V(0.124f, 0.148f, 0.126f));
    /* 얼굴 창: 넓은 얼굴이 드러나도록. 위쪽 경계는 가르마에서 반대편으로 내려가는 앞머리 선 */
    float u = -s * p.x;                  /* 앞머리가 쓸려 가는 방향으로 + */
    float bang = u > -0.045f ? 1.506f - 0.30f * (u + 0.045f) : 1.506f + 0.30f * (-0.045f - u);
    float window = sd_ellipsoid(sub(p, V(0, 1.360f, 0.120f)), V(0.116f, 0.150f, 0.118f));
    window = fmaxf(window, p.y - bang);
    float d = -smin(-cap, window, 0.012f);
    /* 앞머리 끝이 이마 위로 살짝 떠서 결이 생기게 */
    d = smin(d, sd_round_cone(p, V(s * 0.040f, 1.555f, 0.090f), V(-s * 0.105f, 1.455f, 0.072f), 0.022f, 0.012f), 0.02f);
    if (dl->hair == HAIR_LONG) {
        for (int side = -1; side <= 1; side += 2) { /* 관자놀이에서 곧게 떨어져 어깨 앞을 지나 가슴까지 */
            /* 좌우로 얇고 앞뒤로 넓은 커튼: x 를 늘려 평가 (거리는 보수적으로 축소) */
            const float k = 1.9f;
            v3 c = V((p.x - side * 0.118f) * k + side * 0.118f, p.y, p.z);
            float cur = sd_round_cone(c, V(side * 0.118f, 1.470f, -0.010f), V(side * 0.124f, 1.320f, 0.030f), 0.052f, 0.044f);
            cur = smin(cur, sd_round_cone(c, V(side * 0.124f, 1.320f, 0.040f), V(side * 0.136f, 1.150f, 0.118f), 0.044f, 0.012f), 0.03f);
            d = smin(d, cur / k, 0.025f);
        }
        /* 등을 덮는 머리: 몸통 뒤를 따라 평평하게 */
        v3 q = sub(p, V(0, 0, -0.105f + (1.42f - p.y) * -0.06f));
        float back = sd_round_cone(V(q.x, q.y, q.z / 0.40f), V(0, 1.42f, 0), V(0, 1.05f, 0), 0.118f, 0.080f) * 0.40f;
        d = smin(d, back, 0.04f);
    } else if (dl->hair == HAIR_BOB) {
        for (int side = -1; side <= 1; side += 2) { /* 턱선에서 안으로 말리는 단발 */
            float cur = sd_ellipsoid(sub(p, V(side * 0.110f, 1.385f, -0.004f)), V(0.040f, 0.105f, 0.112f));
            cur = smin(cur, sd_ellipsoid(sub(p, V(side * 0.096f, 1.295f, 0.030f)), V(0.034f, 0.026f, 0.050f)), 0.03f);
            d = smin(d, cur, 0.03f);
        }
        d = smin(d, sd_ellipsoid(sub(p, V(0, 1.372f, -0.072f)), V(0.112f, 0.092f, 0.068f)), 0.03f);
        d = -smin(-d, window, 0.010f);
    } else {
        for (int side = -1; side <= 1; side += 2) { /* 어깨에 닿아 바깥으로 뻗는 웨이브 */
            float cur = sd_round_cone(p, V(side * 0.118f, 1.460f, 0.005f), V(side * 0.138f, 1.290f, 0.020f), 0.045f, 0.036f);
            cur = smin(cur, sd_ellipsoid(sub(p, V(side * 0.165f, 1.252f, 0.018f)), V(0.042f, 0.030f, 0.050f)), 0.03f);
            d = smin(d, cur, 0.03f);
        }
        d = smin(d, sd_ellipsoid(sub(p, V(0, 1.330f, -0.090f)), V(0.142f, 0.120f, 0.065f)), 0.04f);
        d = -smin(-d, window, 0.010f);
    }
    /* 꽃핀 */
    *flower_mat = -1;
    v3 f = sub(p, dl->flower_c);
    if (dot(f, f) < 0.06f * 0.06f) {
        float fn = dot(f, dl->flower_n), fu = dot(f, dl->flower_u), fv = dot(f, dl->flower_v);
        float center = len(V(fu, fv, fn - 0.004f)) - 0.012f;
        float petals = 10.0f;
        for (int i = 0; i < 6; i++) {
            float a = (float)i * 1.0471976f;
            float cu = cosf(a) * 0.022f, cv = sinf(a) * 0.022f;
            /* 꽃잎: 바깥으로 긴 납작한 타원 */
            float ru = cosf(a), rv = sinf(a);
            float lu = (fu - cu) * ru + (fv - cv) * rv, lv = -(fu - cu) * rv + (fv - cv) * ru;
            petals = fminf(petals, sd_ellipsoid(V(lu, lv, fn), V(0.017f, 0.011f, 0.006f)));
        }
        float fl = fminf(center, petals);
        if (fl < d) { *flower_mat = center < petals ? M_PISTIL : M_PETAL; d = fl; }
    }
    return d;
}

/* ---------------- 의상 ---------------- */

/* 코트 앞 여밈: 가슴에서는 열려 이너가 보이고 벨트에서 닫힌다 */
static float coat_opening(v3 p) {
    float w = p.y > 1.30f ? 0.072f : p.y < 1.035f ? -0.01f : mixf(-0.01f, 0.072f, smoothstepf(1.035f, 1.30f, p.y));
    w = fmaxf(w, p.y > 1.08f ? 0.030f + (p.y - 1.08f) * 0.19f : w);
    return fmaxf(fabsf(p.x) - w, -p.z);
}

static void map_cyber(const struct df_doll *dl, v3 p, hit *h, float body, int body_m) {
    v3 q = mx(p);
    /* 몸통 코트 (가슴은 열림) */
    if (p.y > 0.98f && p.y < 1.33f) {
        float coat = smin(sd_loft(p, dl->torso, dl->ntorso, 0.013f),
                          sd_ellipsoid(sub(q, V(0.150f, 1.216f, -0.006f)), V(0.070f, 0.072f, 0.072f)), 0.04f);
        coat = fmaxf(coat, p.y - 1.300f - fmaxf(0, fabsf(p.x) - 0.09f) * 0.0f);
        coat = fmaxf(coat, 0.990f - p.y);
        coat = fmaxf(coat, -coat_opening(p));
        take(h, coat, M_COAT);
    }
    /* 코트 자락: 벨트 아래로 골반을 따라 퍼진다 */
    if (p.y < 1.04f && p.y > 0.70f) {
        float sk = sd_loft(p, dl->skirt, dl->nskirt, 0);
        take(h, sk, fabsf(p.x) < 0.004f && p.z > 0 ? M_EDGE : M_COAT);
    }
    /* 소매 + 소맷부리 + 위팔 스트랩 */
    float sleeve = sd_round_cone(q, SHOULDER, ELBOW, 0.052f, 0.046f);
    sleeve = smin(sleeve, sd_round_cone(q, ELBOW, add(WRIST, V(0, 0.03f, 0)), 0.046f, 0.044f), 0.02f);
    v3 fa = nrm(sub(WRIST, ELBOW)), ua = nrm(sub(ELBOW, SHOULDER));
    float cuff = sd_ring(q, add(WRIST, V(0, 0.055f, -0.002f)), fa, 0.047f, 0.012f);
    float strap = sd_ring(q, mixv(SHOULDER, ELBOW, 0.55f), ua, 0.052f, 0.007f);
    take(h, smin(sleeve, body + 0.5f, 0.001f), M_COAT);
    take(h, cuff, M_EDGE);
    take(h, strap, M_BELT);
    take(h, sd_round_box(sub(q, add(mixv(SHOULDER, ELBOW, 0.55f), V(0.054f, 0, 0))), V(0.004f, 0.012f, 0.012f), 0.002f), M_METAL);
    take(h, sd_round_box(sub(q, add(WRIST, V(0.050f, 0.055f, 0))), V(0.004f, 0.011f, 0.011f), 0.002f), M_METAL);
    /* 벨트 + 버클 + 매달린 지퍼 장식 */
    float belt = fmaxf(sd_loft(p, dl->torso, dl->ntorso, 0.024f), fabsf(p.y - 1.030f) - 0.020f);
    take(h, belt, M_BELT);
    v3 bk = sub(p, V(-0.012f, 1.030f, 0.104f));
    float buckle = fmaxf(sd_round_box(bk, V(0.024f, 0.019f, 0.006f), 0.003f), -sd_round_box(bk, V(0.015f, 0.010f, 0.02f), 0.001f));
    take(h, buckle, M_METAL);
    take(h, sd_round_box(sub(p, V(-0.074f, 0.985f, 0.112f)), V(0.008f, 0.016f, 0.004f), 0.002f), M_METAL);
    /* 터틀넥 칼라 */
    take(h, sd_ring(p, V(0, 1.318f, 0.004f), V(0, 1, 0), 0.050f, 0.017f), M_TOP);
    /* 워커 부츠: 굵은 통 + 플랫폼 밑창 + 보라 트림 + 버클 */
    if (p.y < 0.42f) {
        float shaft = sd_round_cone(q, V(0.062f, 0.345f, -0.004f), V(0.061f, 0.11f, -0.006f), 0.072f, 0.068f);
        float foot = sd_round_box(sub(q, V(0.062f, 0.068f, 0.030f)), V(0.056f, 0.068f, 0.112f), 0.040f);
        float boot = smin(shaft, foot, 0.04f);
        float top_ring = sd_ring(q, V(0.062f, 0.338f, -0.004f), V(0, 1, 0), 0.072f, 0.013f);
        boot = smin(boot, top_ring, 0.01f);
        int bm = q.y < 0.030f ? M_SOLE : q.y < 0.048f ? M_TRIM : M_BOOT;
        /* 발 옆을 따라 뒤꿈치 쪽으로 올라가는 곡선 트림 (레퍼런스 부츠 그대로) */
        if (bm == M_BOOT && q.y < 0.17f && fabsf(q.y - (0.058f + 0.38f * clampf(0.10f - q.z, 0, 0.25f))) < 0.0075f) bm = M_TRIM;
        take(h, boot, bm);
        take(h, sd_round_box(sub(q, V(0.062f, 0.300f, 0.072f)), V(0.013f, 0.014f, 0.004f), 0.002f), M_METAL);
    }
    (void)body_m;
}

static void map_casual(const struct df_doll *dl, v3 p, hit *h, float body) {
    v3 q = mx(p);
    /* 넉넉한 후드티: 몸통을 크게 감싸고 앞은 지퍼를 열어 이너가 보인다 */
    if (p.y > 0.80f && p.y < 1.33f) {
        float hood = sd_loft(p, dl->torso, dl->ntorso, 0.030f);
        hood = fmaxf(hood, p.y - 1.300f);
        hood = fmaxf(hood, 0.855f - p.y);
        float open = fmaxf(fabsf(p.x) - (p.y > 1.12f ? 0.040f : 0.030f), -p.z);
        hood = fmaxf(hood, -open);
        take(h, hood, M_TOP);
        take(h, fmaxf(sd_loft(p, dl->torso, dl->ntorso, 0.036f), fabsf(p.y - 0.865f) - 0.014f), M_EDGE); /* 리브 밑단 */
    }
    /* 등에 누운 후드 */
    take(h, sd_ellipsoid(sub(p, V(0, 1.270f, -0.100f)), V(0.100f, 0.060f, 0.045f)), M_TOP);
    /* 후드 끈 */
    for (int side = -1; side <= 1; side += 2)
        take(h, sd_round_cone(p, V(side * 0.042f, 1.290f, 0.090f), V(side * 0.046f, 1.150f, 0.118f), 0.0045f, 0.004f), M_STRING);
    /* 오버핏 소매 + 리브 소맷부리 */
    float sleeve = sd_round_cone(q, SHOULDER, ELBOW, 0.064f, 0.058f);
    sleeve = smin(sleeve, sd_round_cone(q, ELBOW, add(WRIST, V(0, 0.035f, 0)), 0.058f, 0.050f), 0.03f);
    take(h, sleeve, M_TOP);
    take(h, sd_round_cone(q, add(WRIST, V(0, 0.040f, 0)), add(WRIST, V(0, 0.008f, 0)), 0.040f, 0.036f), M_EDGE);
    /* 반바지 */
    if (p.y > 0.60f && p.y < 0.90f) {
        float sh = smin(sd_loft(p, dl->torso, dl->ntorso, 0.012f), sd_round_cone(q, HIP, mixv(HIP, KNEE, 0.42f), 0.100f, 0.090f), 0.03f);
        sh = fmaxf(sh, p.y - 0.880f);
        take(h, sh, M_SHORTS);
    }
    /* 양말 + 발 */
    if (p.y < 0.30f) {
        float sock = sd_round_cone(q, V(0.060f, 0.27f, -0.008f), ANKLE, 0.040f, 0.037f);
        sock = smin(sock, sd_round_box(sub(q, V(0.060f, 0.035f, 0.035f)), V(0.038f, 0.035f, 0.090f), 0.032f), 0.03f);
        take(h, sock, M_SOCKS);
    }
    (void)body;
}

/* 실버 앵클부츠 (낮은 굽) — 드레스업·웨딩 공용 */
static void ankle_boots(v3 q, hit *h) {
    if (q.y > 0.26f) return;
    float shaft = sd_round_cone(q, V(0.060f, 0.20f, -0.006f), V(0.060f, 0.09f, -0.006f), 0.046f, 0.048f);
    float foot = sd_round_box(sub(q, V(0.060f, 0.045f, 0.030f)), V(0.042f, 0.045f, 0.098f), 0.034f);
    float heel = sd_round_box(sub(q, V(0.060f, 0.030f, -0.050f)), V(0.020f, 0.030f, 0.018f), 0.006f);
    take(h, smin(smin(shaft, foot, 0.03f), heel, 0.01f), q.y < 0.016f ? M_SOLE : M_BOOT);
}

static void map_dressup(const struct df_doll *dl, v3 p, hit *h) {
    v3 q = mx(p);
    /* 몸에 붙는 니트 미디 드레스: 가슴·허리·골반을 그대로 따라 종아리 중간에서 끝난다 */
    if (p.y > 0.29f && p.y < 1.33f) {
        float dr = fmaxf(sd_loft(p, dl->dress, dl->ndress, 0), p.y - 1.316f);
        take(h, dr, fabsf(p.x) < 0.003f && p.z > 0 && p.y < 0.9f ? M_INNER : M_TOP);
    }
    /* 크롭 재킷: 허리선에서 끝나고 앞이 넓게 열린다 */
    if (p.y > 1.00f && p.y < 1.33f) {
        float jk = smin(sd_loft(p, dl->torso, dl->ntorso, 0.016f),
                        sd_ellipsoid(sub(q, V(0.150f, 1.216f, -0.006f)), V(0.072f, 0.072f, 0.072f)), 0.04f);
        jk = fmaxf(jk, p.y - 1.300f);
        jk = fmaxf(jk, 1.035f - p.y);
        float w = 0.050f + (p.y - 1.035f) * 0.12f;
        jk = fmaxf(jk, -fmaxf(fabsf(p.x) - w, -p.z));
        take(h, jk, M_COAT);
        take(h, fmaxf(sd_loft(p, dl->torso, dl->ntorso, 0.021f), fabsf(p.y - 1.040f) - 0.006f), M_EDGE);
    }
    float sleeve = sd_round_cone(q, SHOULDER, ELBOW, 0.050f, 0.044f);
    sleeve = smin(sleeve, sd_round_cone(q, ELBOW, add(WRIST, V(0, 0.02f, 0)), 0.044f, 0.036f), 0.02f);
    take(h, sleeve, M_COAT);
    take(h, sd_ring(q, add(WRIST, V(0, 0.022f, 0)), nrm(sub(WRIST, ELBOW)), 0.036f, 0.007f), M_EDGE);
    ankle_boots(q, h);
    /* 보라 클러치 (캐릭터 왼손) + 은색 잠금쇠 */
    v3 bag = sub(p, V(WRIST.x + 0.040f, 0.745f, 0.030f));
    take(h, sd_round_box(bag, V(0.012f, 0.045f, 0.075f), 0.010f), M_BAG);
    take(h, sd_round_box(sub(bag, V(0.013f, 0.030f, 0)), V(0.003f, 0.006f, 0.016f), 0.002f), M_METAL);
}

static void map_wedding(const struct df_doll *dl, v3 p, hit *h) {
    v3 q = mx(p);
    /* 보디스: 몸의 곡선을 그대로 따른다 */
    if (p.y > 0.98f && p.y < 1.33f) {
        float bd = fmaxf(sd_loft(p, dl->torso, dl->ntorso, 0.007f), p.y - 1.316f);
        take(h, bd, M_TOP);
        take(h, fmaxf(sd_loft(p, dl->torso, dl->ntorso, 0.012f), fabsf(p.y - 1.040f) - 0.008f), M_EDGE); /* 라벤더 허리선 */
    }
    /* 허리에서 바닥까지 퍼지는 드레스 */
    if (p.y < 1.06f) {
        float sk = sd_loft(p, dl->gown, dl->ngown, 0);
        float fold = smoothstepf(0.95f, 0.15f, p.y);
        sk -= 0.022f * fold * sinf(7 * atan2f(p.x, p.z) + p.y * 3);      /* 흐르는 드레이프 주름 */
        sk *= 0.8f;
        float hem = 0.012f + 0.20f * smoothstepf(-0.05f, 0.30f, p.z);     /* 앞이 짧고 뒤가 긴 밑단 */
        float cut = hem - p.y;
        sk = fmaxf(sk, cut);
        take(h, sk, p.y < hem + 0.022f ? M_EDGE : M_TOP);
    }
    ankle_boots(q, h);
    float sleeve = sd_round_cone(q, SHOULDER, ELBOW, 0.048f, 0.042f);
    sleeve = smin(sleeve, sd_round_cone(q, ELBOW, add(WRIST, V(0, 0.015f, 0)), 0.042f, 0.038f), 0.02f);
    take(h, sleeve, M_TOP);
    take(h, sd_ring(q, add(WRIST, V(0, 0.018f, 0)), nrm(sub(WRIST, ELBOW)), 0.038f, 0.007f), M_EDGE);
    /* 베일: 머리 뒤에서 등으로 흘러내리는 얇은 막 */
    if (dl->veil_on && p.z < 0.06f && p.y > 1.00f) {
        float shell = fabsf(sd_ellipsoid(sub(p, V(0, 1.300f, -0.070f)), V(0.175f, 0.320f, 0.150f))) - 0.004f;
        shell = fmaxf(shell, p.z + 0.035f - fmaxf(0.0f, p.y - 1.40f) * 0.6f);
        shell = fmaxf(shell, 1.03f - p.y);
        take(h, shell, M_VEIL);
    }
    /* 클러치 */
    v3 bag = sub(p, V(WRIST.x + 0.040f, 0.745f, 0.030f));
    take(h, sd_round_box(bag, V(0.012f, 0.045f, 0.075f), 0.010f), M_BAG);
}

/* 전체 장면: 거리 + 재질 */
static hit map(const struct df_doll *dl, v3 p) {
    hit h = { 10.0f, M_SKIN };
    v3 q = mx(p);
    /* 바운딩 구로 먼 부위는 건너뛴다 */
    float bound_head = len(sub(p, V(0, 1.44f, 0))) - 0.26f;
    /* 몸 (피부 아래층) — 가장 가까운 부위가 재질을 정한다 */
    float dt = sd_loft(p, dl->torso, dl->ntorso, 0);
    float dsh = sd_ellipsoid(sub(q, V(0.150f, 1.212f, -0.006f)), V(0.060f, 0.058f, 0.060f));
    float dn = sd_round_cone(p, V(0, 1.270f, -0.004f), V(0, 1.360f, 0.004f), 0.046f, 0.043f);
    float da = q.y > 0.68f && q.y < 1.30f ? sd_arm(q) : 10.0f;
    float dhd = q.y > 0.66f && q.y < 0.86f ? sd_hand(q) : 10.0f;
    float dl_ = q.y < 0.86f ? sd_leg(dl, q) : 10.0f;
    float dhe = bound_head < 0.05f ? sd_head(p) : bound_head;
    float body = smin(dt, dsh, 0.05f);
    body = smin(body, dn, 0.03f);
    body = smin(body, da, 0.03f);
    body = smin(body, dhd, 0.012f);
    body = smin(body, dl_, 0.035f);
    body = smin(body, dhe, 0.02f);
    int bm;
    float best = fminf(fminf(dt, dsh), fminf(dn, da));
    if (dhe <= best && dhe <= dhd && dhe <= dl_) bm = M_SKIN;
    else if (dhd <= best && dhd <= dl_) bm = dl->outfit == OUTFIT_CYBER ? M_GLOVE : M_SKIN;
    else if (dl_ <= best) bm = M_LEGS;
    else bm = dl->outfit == OUTFIT_CASUAL ? M_INNER : M_TOP;
    /* 목 위쪽은 피부 */
    if (bm == M_TOP && p.y > 1.335f) bm = M_SKIN;
    take(&h, body, bm);

    switch (dl->outfit) {
    case OUTFIT_CYBER: map_cyber(dl, p, &h, body, bm); break;
    case OUTFIT_CASUAL: map_casual(dl, p, &h, body); break;
    case OUTFIT_DRESSUP: map_dressup(dl, p, &h); break;
    default: map_wedding(dl, p, &h); break;
    }

    if (bound_head < h.d + 0.02f) {
        int fm;
        float hair = sd_hair(dl, p, &fm);
        take(&h, hair, fm >= 0 ? fm : M_HAIR);
    }
    return h;
}

static inline float mapd(const struct df_doll *dl, v3 p) { return map(dl, p).d; }



static v3 normal_at(const struct df_doll *dl, v3 p) {
    const float e = 0.0006f;
    /* 테트라헤드론 기법: 평가 4번 */
    v3 k1 = V(1, -1, -1), k2 = V(-1, -1, 1), k3 = V(-1, 1, -1), k4 = V(1, 1, 1);
    v3 n = add(add(mul(k1, mapd(dl, add(p, mul(k1, e)))), mul(k2, mapd(dl, add(p, mul(k2, e))))),
               add(mul(k3, mapd(dl, add(p, mul(k3, e)))), mul(k4, mapd(dl, add(p, mul(k4, e))))));
    return nrm(n);
}

/* ---------------- 메이크업 (얼굴 위의 곡선) ---------------- */

static v3 face_makeup(const struct df_doll *dl, v3 p, v3 base, v3 n) {
    if (p.z < 0.02f || n.z < 0.15f || p.y < 1.28f || p.y > 1.50f) return base;
    v3 c = base;
    float fx = p.x, fy = p.y;
    /* 볼 홍조 */
    for (int s = -1; s <= 1; s += 2) {
        float dx = fx - s * 0.066f, dy = fy - 1.360f;
        float g = expf(-(dx * dx + dy * dy * 1.6f) / (2 * 0.021f * 0.021f)) * dl->blush;
        c = mixv(c, dl->c_blush, clampf(g, 0, 1) * 0.75f);
    }
    /* 입술: 살짝 올라간 미소 곡선 */
    {
        float mw = 0.024f, x = fx / mw;
        if (fabsf(x) < 1.1f) {
            float y0 = 1.334f + dl->lip_curve * 0.0055f * x * x;
            float t = 0.0019f * (1 - 0.5f * x * x);
            float d = fabsf(fy - y0);
            if (d < t) c = mixv(c, dl->c_mouth, smoothstepf(t, t * 0.4f, d));
            float lower = fy - (y0 - 0.0055f);
            if (fabsf(x) < 0.6f && fabsf(lower) < 0.0028f) c = mixv(c, mixv(c, dl->c_mouth, 0.35f), smoothstepf(0.0028f, 0.0f, fabsf(lower)));
        }
    }
    for (int s = -1; s <= 1; s += 2) {
        float ex = 0.0560f, ey = 1.396f;
        float w = 0.031f * dl->eye_size;
        float lx = (fx - s * ex) * s;   /* + 는 바깥(귀) 쪽 */
        float ly = fy - ey;
        float u = lx / w;
        /* 눈썹: 바깥으로 갈수록 가늘어지는 아치 */
        {
            float bx = (lx + 0.004f) / 0.034f;
            if (bx > -1 && bx < 1.15f) {
                float by = 0.036f + dl->brow_arch * 0.007f * (1 - bx * bx) - 0.003f * bx;
                float t = 0.0034f * (1.15f - 0.65f * fmaxf(bx, 0));
                float d = fabsf(ly - by);
                if (d < t) c = mixv(c, dl->c_brow, smoothstepf(t, t * 0.3f, d) * 0.95f);
            }
        }
        float top = dl->eye_open * 0.0200f * dl->eye_size * (1 - u * u) * (1 - 0.25f * u) + dl->eye_tilt * 0.010f * u;
        float bot = -0.0125f * dl->eye_size * (1 - u * u * u * u) + 0.003f * u;
        /* 쌍꺼풀 선 */
        if (fabsf(u) < 0.95f && fabsf(ly - (top + 0.0062f)) < 0.0011f) c = mixv(c, mul(base, 0.78f), 0.6f);
        if (fabsf(u) < 1.0f && ly < top && ly > bot) {
            /* 흰자 → 홍채(위 진하고 아래 밝게) → 동공 → 하이라이트 */
            v3 e = dl->c_eye_white;
            float ix = lx + 0.0028f, iy = ly - 0.0012f;
            float ri = 0.0165f * dl->eye_size;
            float r = sqrtf(ix * ix + iy * iy);
            if (r < ri) {
                float g = clampf((iy / ri) * 0.5f + 0.5f, 0, 1);
                e = mixv(dl->c_eye_light, dl->c_eye, g * 0.9f + 0.1f);
                if (r > ri * 0.86f) e = mul(dl->c_eye, 0.55f);
                if (r < ri * 0.42f) e = mixv(mul(dl->c_eye, 0.25f), V(0.02f, 0.01f, 0.02f), 0.6f);
            }
            /* 위 눈꺼풀 그늘 */
            e = mixv(e, mul(e, 0.6f), smoothstepf(top - 0.006f, top, ly) * 0.8f);
            float hx = ix + 0.0050f, hy = iy - 0.0060f;
            if (hx * hx + hy * hy < 0.0046f * 0.0046f * dl->eye_size * dl->eye_size) e = V(1, 1, 1);
            float hx2 = ix - 0.0040f, hy2 = iy + 0.0050f;
            if (hx2 * hx2 + hy2 * hy2 < 0.0016f * 0.0016f) e = mixv(e, V(1, 1, 1), 0.7f);
            /* 눈 테두리 안티앨리어싱 */
            float edge = fminf(fminf(top - ly, ly - bot), (1 - fabsf(u)) * w);
            c = mixv(c, e, smoothstepf(0, 0.0008f, edge));
        }
        /* 위 속눈썹: 두꺼운 곡선 + 바깥으로 올라가는 윙 */
        {
            float lt = 0.0036f * dl->lash * (0.6f + 0.9f * clampf(u, 0, 1));
            if (u > -1.02f && u < 1.0f) {
                float d = ly - top;
                if (d > -0.0004f && d < lt) c = mixv(c, dl->c_lash, smoothstepf(lt, lt * 0.5f, d));
            }
            if (u >= 0.85f && u < 1.0f + 0.55f * dl->lash) { /* 윙 */
                float wu = (u - 0.85f) / (0.55f * dl->lash);
                float wy = top + (u >= 1 ? 0 : 0) + dl->eye_tilt * 0.010f * u + 0.0065f * wu * wu;
                float wyb = dl->eye_tilt * 0.010f * 1.0f + 0.0065f * wu * wu + (u < 1 ? top - dl->eye_tilt * 0.010f * u : 0);
                float yref = u < 1 ? wy : wyb;
                float t = lt * (1 - wu);
                float d = ly - yref;
                if (d > -0.0003f && d < t) c = mixv(c, dl->c_lash, smoothstepf(t, t * 0.4f, d));
            }
            /* 아래 속눈썹 (바깥 절반) */
            if (u > 0.15f && u < 1.0f && fabsf(ly - bot) < 0.0007f) c = mixv(c, dl->c_lash, 0.45f);
        }
    }
    return c;
}

/* ---------------- 셰이딩 ---------------- */

static float soft_shadow(const struct df_doll *dl, v3 ro, v3 rd) {
    float res = 1, t = 0.004f;
    for (int i = 0; i < 40 && t < 1.5f; i++) {
        float h = mapd(dl, add(ro, mul(rd, t)));
        res = fminf(res, 10 * h / t);
        if (res < 0.02f) return 0.02f;
        t += clampf(h, 0.004f, 0.10f);
    }
    return clampf(res, 0, 1);
}

static float ambient_occlusion(const struct df_doll *dl, v3 p, v3 n) {
    float occ = 0, sca = 1;
    for (int i = 1; i <= 5; i++) {
        float h = 0.006f + 0.016f * (float)i;
        occ += (h - mapd(dl, add(p, mul(n, h)))) * sca;
        sca *= 0.72f;
    }
    return clampf(1 - 2.6f * occ, 0, 1);
}

static v3 shade(const struct df_doll *dl, v3 p, v3 n, v3 v, int m) {
    v3 base = dl->col[m];
    if (m == M_SKIN) base = face_makeup(dl, p, base, n);
    if (m == M_HAIR) {
        /* 머리결: 가닥을 따라 흐르는 미세한 명암 */
        float ang = atan2f(p.x, p.z);
        float strand = 0.5f + 0.5f * sinf(ang * 46 + p.y * 9) * sinf(ang * 17 - p.y * 4);
        base = mixv(base, dl->c_hair_dark, 0.30f * strand);
    }
    if (m == M_TOP && dl->outfit == OUTFIT_WEDDING && p.y > 1.05f && p.y < 1.30f && p.z > 0) {
        /* 레퍼런스 보디스: 가운데 꽃에서 허리로 V 자로 퍼지는 라벤더 꽃 자수 */
        float ax = fabsf(p.x);
        for (int i = 0; i < 7; i++) {
            float t = (float)i / 6.0f;
            float cx = 0.012f + 0.070f * t, cy = 1.255f - 0.190f * t + 0.05f * t * t;
            float dx = ax - cx, dy = p.y - cy, r = sqrtf(dx * dx + dy * dy);
            float petal = 0.0062f * (0.75f + 0.25f * cosf(5 * atan2f(dy, dx)));
            if (r < petal) base = mixv(base, r < 0.0022f ? mul(dl->col[M_EDGE], 0.7f) : dl->col[M_EDGE], 0.85f);
            /* 꽃 사이를 잇는 덩굴 */
            float vine = fabsf(ax - (0.012f + 0.070f * clampf((1.255f - p.y) / 0.19f, 0, 1))) ;
            if (i == 0 && vine < 0.0013f && p.y < 1.255f && p.y > 1.07f) base = mixv(base, dl->col[M_EDGE], 0.7f);
        }
    }
    if ((m == M_TOP || m == M_COAT || m == M_INNER) && dl->outfit == OUTFIT_DRESSUP) {
        /* 레퍼런스 드레스: 잔 비늘 무늬가 있는 메탈릭 니트 */
        float a = atan2f(p.x, p.z) * 0.10f, y = p.y;
        float cell = fabsf(sinf(a * 260 + (fmodf(floorf(y * 160), 2) * 1.57f))) * fabsf(sinf(y * 503));
        base = mul(base, 0.86f + 0.14f * cell);
    }
    if (m == M_LEGS && dl->outfit == OUTFIT_CYBER) {
        /* 비치는 타이츠: 정면을 향한 곳은 피부가 비쳐 밝고, 가장자리는 짙다 */
        float facing = clampf(dot(n, v), 0, 1);
        base = mixv(base, mixv(base, dl->col[M_SKIN], 0.55f), facing * facing * 0.55f);
    }
    v3 L1 = nrm(V(-0.42f, 0.70f, 0.58f)), L2 = nrm(V(0.70f, 0.25f, 0.55f)), L3 = nrm(V(0.35f, 0.45f, -0.80f));
    float sh = soft_shadow(dl, add(p, mul(n, 0.002f)), L1);
    float ao = ambient_occlusion(dl, p, n);
    float wrap = m == M_SKIN ? 0.45f : 0.20f;
    float d1 = clampf((dot(n, L1) + wrap) / (1 + wrap), 0, 1) * sh;
    float d2 = clampf(dot(n, L2), 0, 1);
    float rim = powf(1 - clampf(dot(n, v), 0, 1), 3.0f) * clampf(dot(n, L3) + 0.4f, 0, 1);
    float sky = 0.55f + 0.45f * n.y;
    v3 light = add(add(mul(V(1.00f, 0.95f, 0.90f), 1.25f * d1), mul(V(0.55f, 0.62f, 0.85f), 0.38f * d2)),
                   mul(V(0.42f, 0.36f, 0.50f), 0.55f * sky * ao));
    v3 c = V(base.x * light.x, base.y * light.y, base.z * light.z);
    if (m == M_SKIN) c = add(c, mul(V(0.30f, 0.06f, 0.04f), 0.10f * d1)); /* 피부 속 산란 느낌 */
    /* 하이라이트 */
    v3 hv = nrm(add(L1, v));
    float spec_pow = 24, spec_k = 0.06f;
    switch (m) {
    case M_HAIR: spec_pow = 60; spec_k = 0.45f * dl->gloss; break;
    case M_BOOT: case M_SOLE: case M_TRIM: spec_pow = 70; spec_k = 0.35f; break;
    case M_METAL: spec_pow = 90; spec_k = 1.2f; break;
    case M_BELT: spec_pow = 50; spec_k = 0.25f; break;
    case M_SKIN: spec_pow = 30; spec_k = 0.10f; break;
    case M_LEGS: spec_pow = 40; spec_k = dl->outfit == OUTFIT_CYBER ? 0.30f : 0.08f; break;
    case M_PETAL: case M_PISTIL: spec_pow = 40; spec_k = 0.25f; break;
    case M_BAG: spec_pow = 60; spec_k = 0.35f; break;
    case M_TOP: case M_COAT: case M_INNER:
        if (dl->outfit == OUTFIT_DRESSUP) { spec_pow = 28; spec_k = 0.55f; }
        else if (dl->outfit == OUTFIT_WEDDING) { spec_pow = 35; spec_k = 0.18f; }
        break;
    case M_VEIL: spec_pow = 20; spec_k = 0.05f; break;
    default: break;
    }
    float spec = powf(clampf(dot(n, hv), 0, 1), spec_pow) * spec_k * sh;
    if (m == M_HAIR) { /* 정수리 위 윤기 띠 */
        float band = expf(-powf((p.y - 1.545f - 0.03f * n.z) / 0.020f, 2)) * clampf(n.y + 0.3f, 0, 1);
        spec += band * 0.30f * dl->gloss;
    }
    /* 금속성 재질: 방의 조명이 비친다 (위는 밝고, 옆은 보라·청록 네온) */
    float metal = m == M_METAL ? 0.45f : (m == M_BOOT && dl->outfit != OUTFIT_CYBER) ? 0.35f
                : ((m == M_TOP || m == M_COAT || m == M_INNER) && dl->outfit == OUTFIT_DRESSUP) ? 0.30f : 0;
    if (metal > 0) {
        v3 rf = sub(mul(n, 2 * dot(n, v)), v);
        v3 env = mixv(V(0.10f, 0.08f, 0.12f), V(0.95f, 0.92f, 0.95f), smoothstepf(-0.2f, 0.8f, rf.y));
        env = add(env, mul(V(0.6f, 0.3f, 0.9f), smoothstepf(0.5f, 0.95f, rf.x) * 0.6f));
        env = add(env, mul(V(0.2f, 0.8f, 0.9f), smoothstepf(0.5f, 0.95f, -rf.x) * 0.5f));
        c = add(mul(c, 1 - metal), mul(V(env.x * (0.4f + base.x), env.y * (0.4f + base.y), env.z * (0.4f + base.z)), metal));
    }
    c = add(c, V(spec, spec, spec));
    c = add(c, mul(V(0.55f, 0.35f, 0.95f), rim * 0.45f)); /* 네온 림 라이트 (레퍼런스 방의 보라·청록 조명) */
    c = add(c, mul(V(0.25f, 0.75f, 0.85f), powf(1 - clampf(dot(n, v), 0, 1), 4.0f) * clampf(dot(n, nrm(V(-0.8f, 0.2f, -0.4f))), 0, 1) * 0.30f));
    return mul(c, 0.55f + 0.45f * ao);
}

/* ---------------- 렌더 ---------------- */

static inline float to_srgb(float x) {
    x = x / (1 + 0.35f * x) * 1.12f; /* 부드러운 톤매핑 */
    x = clampf(x, 0, 1);
    return x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1 / 2.4f) - 0.055f;
}

static inline float to_linear(uint8_t c) {
    float x = c / 255.0f;
    return x <= 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f);
}

typedef struct {
    const struct df_doll *dl;
    int w, h, ss, background, row0, step;
    v3 cam, fwd, right, up;
    float tan_half;
    uint8_t *out;
} render_job;

static void background_color(const render_job *j, float sx, float sy, float out[4]) {
    if (!j->background) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    float t = sy;
    v3 top = V(0.10f, 0.07f, 0.12f), bottom = V(0.20f, 0.12f, 0.12f);
    v3 c = mixv(top, bottom, t);
    /* 발밑 그림자 */
    float dx = (sx - 0.5f) * 2.2f, dy = (sy - 0.93f) * 12;
    c = mul(c, 1 - 0.55f * expf(-(dx * dx + dy * dy)));
    out[0] = c.x; out[1] = c.y; out[2] = c.z; out[3] = 1;
}

static void trace_pixel(const render_job *j, float px, float py, float acc[4]) {
    float sx = (2 * px / j->w - 1) * j->tan_half * j->w / j->h, sy = (1 - 2 * py / j->h) * j->tan_half;
    v3 rd = nrm(add(add(j->fwd, mul(j->right, sx)), mul(j->up, sy)));
    v3 ro = j->cam;
    /* 캐릭터를 감싸는 상자와 교차 구간만 진행 */
    float t0 = 0, t1 = 20;
    const float bmin[3] = { -0.40f, -0.01f, -0.32f }, bmax[3] = { 0.40f, 1.66f, 0.32f };
    const float o[3] = { ro.x, ro.y, ro.z }, d[3] = { rd.x, rd.y, rd.z };
    for (int a = 0; a < 3; a++) {
        float inv = 1.0f / d[a], ta = (bmin[a] - o[a]) * inv, tb = (bmax[a] - o[a]) * inv;
        if (ta > tb) { float tmp = ta; ta = tb; tb = tmp; }
        t0 = fmaxf(t0, ta); t1 = fminf(t1, tb);
    }
    float bg[4];
    background_color(j, px / j->w, py / j->h, bg);
    if (t0 < t1) {
        float t = t0;
        for (int i = 0; i < 220 && t < t1; i++) {
            v3 p = add(ro, mul(rd, t));
            hit hh = map(j->dl, p);
            if (hh.d < 0.0004f * (1 + t * 0.2f)) {
                v3 n = normal_at(j->dl, p);
                v3 c = shade(j->dl, p, n, mul(rd, -1), hh.m);
                if (hh.m == M_VEIL && j->dl->noveil) {
                    /* 베일은 45% 만 덮는다: 베일을 뺀 사본으로 뒤를 마저 그려 섞는다 */
                    const struct df_doll *nv = j->dl->noveil;
                    float tv = t + 0.006f;
                    int got = 0;
                    v3 cb = V(0, 0, 0);
                    for (int s2 = 0; s2 < 200 && tv < t1; s2++) {
                        v3 p2 = add(ro, mul(rd, tv));
                        hit h2 = map(nv, p2);
                        if (h2.d < 0.0004f * (1 + tv * 0.2f)) { cb = shade(nv, p2, normal_at(nv, p2), mul(rd, -1), h2.m); got = 1; break; }
                        tv += h2.d * 0.72f;
                    }
                    float a = 0.45f;
                    if (got) {
                        c = mixv(cb, c, a);
                        acc[0] += to_srgb(c.x); acc[1] += to_srgb(c.y); acc[2] += to_srgb(c.z); acc[3] += 1;
                    } else {
                        /* 뒤가 배경이면 베일을 반투명으로 */
                        float bgc[3] = { bg[0], bg[1], bg[2] };
                        if (bg[3] > 0) {
                            acc[0] += to_srgb(mixf(bgc[0], c.x, a)); acc[1] += to_srgb(mixf(bgc[1], c.y, a)); acc[2] += to_srgb(mixf(bgc[2], c.z, a)); acc[3] += 1;
                        } else {
                            acc[0] += to_srgb(c.x) * a; acc[1] += to_srgb(c.y) * a; acc[2] += to_srgb(c.z) * a; acc[3] += a;
                        }
                    }
                    return;
                }
                acc[0] += to_srgb(c.x); acc[1] += to_srgb(c.y); acc[2] += to_srgb(c.z); acc[3] += 1;
                return;
            }
            t += hh.d * 0.72f;
        }
    }
    if (bg[3] > 0) { acc[0] += to_srgb(bg[0]); acc[1] += to_srgb(bg[1]); acc[2] += to_srgb(bg[2]); acc[3] += 1; }
}

static void *render_rows(void *arg) {
    render_job *j = arg;
    for (int y = j->row0; y < j->h; y += j->step)
        for (int x = 0; x < j->w; x++) {
            float acc[4] = { 0, 0, 0, 0 };
            for (int sy = 0; sy < j->ss; sy++)
                for (int sx = 0; sx < j->ss; sx++)
                    trace_pixel(j, x + (sx + 0.5f) / j->ss, y + (sy + 0.5f) / j->ss, acc);
            uint8_t *o = &j->out[((size_t)y * j->w + x) * 4];
            float n = (float)(j->ss * j->ss);
            if (acc[3] > 0) {
                for (int c = 0; c < 3; c++) o[c] = (uint8_t)clampf(acc[c] / acc[3] * 255 + 0.5f, 0, 255);
                o[3] = (uint8_t)clampf(acc[3] / n * 255 + 0.5f, 0, 255);
            } else memset(o, 0, 4);
        }
    return NULL;
}

int df_doll_render(const df_doll *dl, int w, int h, double yaw_deg, double pitch_deg, double zoom, int flags, uint8_t *rgba) {
    if (!dl || w <= 0 || h <= 0 || w > 8192 || h > 8192) return 0;
    float yaw = (float)yaw_deg * 0.0174533f, pitch = (float)pitch_deg * 0.0174533f;
    float z = (float)(zoom < 1 ? 1 : zoom > 8 ? 8 : zoom);
    v3 target = V(0, mixf(0.80f, 1.42f, clampf((z - 1) / 2.5f, 0, 1)), 0);
    float dist = 4.2f;
    v3 cam = add(target, V(-sinf(yaw) * cosf(pitch) * dist, sinf(pitch) * dist, cosf(yaw) * cosf(pitch) * dist));
    render_job base;
    base.dl = dl; base.w = w; base.h = h;
    base.ss = flags & DF_RENDER_HQ ? 2 : 1;
    base.background = flags & DF_RENDER_BACKGROUND ? 1 : 0;
    base.cam = cam;
    base.fwd = nrm(sub(target, cam));
    base.right = nrm(cross(base.fwd, V(0, 1, 0)));
    base.up = cross(base.right, base.fwd);
    base.tan_half = 0.92f / dist / z;
    base.out = rgba;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int nt = ncpu < 1 ? 1 : ncpu > 64 ? 64 : (int)ncpu;
    render_job jobs[64];
    pthread_t th[64];
    int started = 0;
    for (int i = 0; i < nt; i++) {
        jobs[i] = base;
        jobs[i].row0 = i;
        jobs[i].step = nt;
        if (pthread_create(&th[i], NULL, render_rows, &jobs[i]) == 0) started++;
        else { render_rows(&jobs[i]); }
    }
    for (int i = 0; i < nt && i < started; i++) pthread_join(th[i], NULL);
    return 1;
}

/* ---------------- 레시피 → doll ---------------- */

static float num(const df_recipe *r, const char *sec, const char *key, float dflt) {
    const df_json *v = df__recipe_doll(r, sec, key);
    return df_json_kind(v) == DF_JSON_NUMBER ? (float)df_json_num(v) : dflt;
}

static const char *str(const df_recipe *r, const char *sec, const char *key, const char *dflt) {
    const char *s = df_json_str(df__recipe_doll(r, sec, key));
    return s ? s : dflt;
}

static v3 color(df_recipe *r, const char *key, const char *fallback, const char *dflt) {
    uint8_t c[4];
    if (!df__recipe_color(r, key, c) && !(fallback && df__recipe_color(r, fallback, c)) && !df__recipe_color(r, dflt, c))
        c[0] = c[1] = c[2] = 128;
    return V(to_linear(c[0]), to_linear(c[1]), to_linear(c[2]));
}

static df_doll *doll_from_recipe(df_recipe *r, char *err, size_t errlen) {
    df_doll *dl = calloc(1, sizeof *dl);
    if (!dl) { df__recipe_free(r); return NULL; }
    const char *outfit = str(r, "outfit", "type", "cyber");
    dl->outfit = !strcmp(outfit, "casual") ? OUTFIT_CASUAL : !strcmp(outfit, "cyber") ? OUTFIT_CYBER
               : !strcmp(outfit, "dressup") ? OUTFIT_DRESSUP : !strcmp(outfit, "wedding") ? OUTFIT_WEDDING : -1;
    if (dl->outfit < 0) {
        snprintf(err, errlen, "doll.outfit.type '%s' is not modeled in 3D (cyber, casual, dressup, wedding)", outfit);
        free(dl); df__recipe_free(r); return NULL;
    }
    const char *hair = str(r, "hair", "style", "long");
    dl->hair = !strcmp(hair, "bob") ? HAIR_BOB : !strcmp(hair, "wavy") ? HAIR_WAVY : HAIR_LONG;
    dl->flower_side = !strcmp(str(r, "flower", "side", "right"), "left") ? 1.0f : -1.0f;

    dl->bust = num(r, "figure", "bust", 1); dl->waist = num(r, "figure", "waist", 1);
    dl->hips = num(r, "figure", "hips", 1); dl->thighs = num(r, "figure", "thighs", 1);
    dl->eye_size = num(r, "makeup", "eye_size", 1); dl->eye_open = num(r, "makeup", "eye_open", 0.66f);
    dl->eye_tilt = num(r, "makeup", "eye_tilt", 0.35f); dl->lash = num(r, "makeup", "lash", 1);
    dl->brow_arch = num(r, "makeup", "brow_arch", 1); dl->blush = num(r, "makeup", "blush", 0.75f);
    dl->lip_curve = num(r, "makeup", "lip_curve", 1); dl->gloss = num(r, "makeup", "gloss", 1);

    dl->col[M_SKIN] = color(r, "skin", NULL, "#F2C4B0");
    dl->col[M_HAIR] = color(r, "hair", NULL, "#8E5A44");
    dl->c_hair_dark = color(r, "hair_dark", NULL, "#704334");
    dl->col[M_PETAL] = color(r, "flower_petal", NULL, "#A066C4");
    dl->col[M_PISTIL] = color(r, "flower_center", NULL, "#EBC35C");
    dl->c_blush = color(r, "blush", NULL, "#EC9CA2");
    dl->c_mouth = color(r, "mouth", NULL, "#C9767A");
    dl->c_brow = color(r, "brow", NULL, "#5C3628");
    dl->c_lash = color(r, "lash", NULL, "#2B1B24");
    dl->c_eye = color(r, "eye", NULL, "#A8317E");
    dl->c_eye_light = color(r, "eye_light", NULL, "#D460AE");
    dl->c_eye_white = color(r, "eye_white", NULL, "#FFF6F8");
    if (dl->outfit == OUTFIT_CYBER) {
        dl->col[M_TOP] = color(r, "turtleneck", NULL, "#6E3B98");
        dl->col[M_COAT] = color(r, "coat", NULL, "#2B272F");
        dl->col[M_EDGE] = color(r, "coat_edge", "coat", "#3C3642");
        dl->col[M_BELT] = color(r, "belt", NULL, "#17151A");
        dl->col[M_METAL] = color(r, "buckle", NULL, "#C4C0CC");
        dl->col[M_GLOVE] = color(r, "gloves", NULL, "#2C2135");
        dl->col[M_LEGS] = color(r, "tights", NULL, "#3D2A2B");
        dl->col[M_BOOT] = color(r, "boots", NULL, "#1B191F");
        dl->col[M_TRIM] = color(r, "boot_trim", NULL, "#7442AE");
        dl->col[M_SOLE] = color(r, "boot_sole", NULL, "#121015");
    } else if (dl->outfit == OUTFIT_DRESSUP) {
        dl->col[M_TOP] = color(r, "dress", NULL, "#8F8D96");
        dl->col[M_INNER] = color(r, "dress_dark", "dress", "#7A7880");
        dl->col[M_COAT] = color(r, "jacket", NULL, "#A9A7B1");
        dl->col[M_EDGE] = color(r, "jacket_edge", "jacket", "#7D7B83");
        dl->col[M_BOOT] = color(r, "boots", NULL, "#BDBBC4");
        dl->col[M_SOLE] = color(r, "boot_sole", NULL, "#4A4851");
        dl->col[M_BAG] = color(r, "bag", NULL, "#7A4BB0");
        dl->col[M_METAL] = color(r, "bag_clasp", NULL, "#D9D7DE");
        dl->col[M_LEGS] = dl->col[M_SKIN];
    } else if (dl->outfit == OUTFIT_WEDDING) {
        dl->col[M_TOP] = color(r, "gown", NULL, "#F3F0F6");
        dl->col[M_EDGE] = color(r, "lace", NULL, "#C7B2E2");
        dl->col[M_VEIL] = color(r, "veil", NULL, "#FAF8FC");
        dl->col[M_BAG] = color(r, "bag", "flower_petal", "#7A4BB0");
        dl->col[M_BOOT] = color(r, "boots", NULL, "#BDBBC4");
        dl->col[M_SOLE] = color(r, "boot_sole", NULL, "#55535C");
        dl->col[M_LEGS] = dl->col[M_SKIN];
    } else {
        dl->col[M_TOP] = color(r, "hoodie", NULL, "#6C3794");
        dl->col[M_EDGE] = color(r, "hoodie_rib", "hoodie", "#4A245F");
        dl->col[M_INNER] = color(r, "top", NULL, "#2C2430");
        dl->col[M_STRING] = color(r, "drawstring", NULL, "#EDE6F2");
        dl->col[M_SHORTS] = color(r, "shorts", NULL, "#9A979E");
        dl->col[M_SOCKS] = color(r, "socks", NULL, "#F3F0F4");
        dl->col[M_LEGS] = dl->col[M_SKIN];
    }

    /* 몸통 단면 — JennyCartoon3D.jpg 실루엣 실측 (키 1.6m 환산) */
    const section torso[] = {
        { 1.310f, 0.058f, 0.054f, 0.000f },
        { 1.255f, 0.148f, 0.084f, -0.005f },   /* 어깨 */
        { 1.190f, 0.146f, 0.098f, 0.012f },
        { 1.150f, 0.143f, 0.114f * dl->bust, 0.024f * dl->bust },  /* 가슴 */
        { 1.098f, 0.131f, 0.090f, 0.007f },    /* 가슴 아래 */
        { 1.040f, 0.118f * dl->waist, 0.078f * dl->waist, 0.0f }, /* 허리 (벨트) */
        { 0.975f, 0.138f * dl->hips, 0.086f, -0.004f },
        { 0.900f, 0.163f * dl->hips, 0.098f, -0.010f },  /* 골반 */
        { 0.820f, 0.171f * dl->hips, 0.100f, -0.008f },
        { 0.760f, 0.158f * dl->hips, 0.092f, 0.000f },
    };
    dl->ntorso = (int)(sizeof torso / sizeof torso[0]);
    memcpy(dl->torso, torso, sizeof torso);
    /* 코트 자락 — 벨트 아래에서 골반을 따라 퍼져 밑단(0.744m)에서 끝난다 */
    const section skirt[] = {
        { 1.012f, 0.136f * dl->waist, 0.096f, 0.000f },
        { 0.940f, 0.178f * dl->hips, 0.116f, -0.004f },
        { 0.860f, 0.200f * dl->hips, 0.128f, -0.006f },
        { 0.790f, 0.212f * dl->hips, 0.134f, -0.004f },
        { 0.744f, 0.217f * dl->hips, 0.137f, 0.000f },
    };
    dl->nskirt = (int)(sizeof skirt / sizeof skirt[0]);
    memcpy(dl->skirt, skirt, sizeof skirt);

    /* 미디 드레스: 골반을 감싸고 무릎 아래로 살짝 좁아졌다가 밑단에서 퍼진다 (JennyDressUp.png) */
    const section dress_low[] = {
        { 0.900f, 0.172f * dl->hips, 0.106f, -0.010f },
        { 0.820f, 0.186f * dl->hips, 0.122f, -0.006f },
        { 0.660f, 0.190f * dl->thighs, 0.120f, 0.002f },
        { 0.480f, 0.160f, 0.098f, 0.004f },
        { 0.300f, 0.158f, 0.098f, 0.000f },
    };
    dl->ndress = 0;
    for (int i = 0; i < dl->ntorso && dl->torso[i].y >= 0.97f; i++) {
        dl->dress[dl->ndress] = dl->torso[i];
        dl->dress[dl->ndress].a += 0.007f; dl->dress[dl->ndress].b += 0.007f;
        dl->ndress++;
    }
    memcpy(dl->dress + dl->ndress, dress_low, sizeof dress_low);
    dl->ndress += (int)(sizeof dress_low / sizeof dress_low[0]);
    /* 웨딩드레스 치마: 허리에서 바닥까지 A 라인 (JennyMarriage.png) */
    const section gown[] = {
        { 1.050f, 0.126f * dl->waist, 0.086f, 0.000f },
        { 0.930f, 0.185f * dl->hips, 0.120f, -0.006f },
        { 0.700f, 0.250f, 0.175f, 0.000f },
        { 0.400f, 0.300f, 0.225f, 0.010f },
        { 0.150f, 0.330f, 0.255f, 0.015f },
        { 0.010f, 0.345f, 0.265f, 0.015f },
    };
    dl->ngown = (int)(sizeof gown / sizeof gown[0]);
    memcpy(dl->gown, gown, sizeof gown);

    /* 꽃핀 — 가르마 옆, 머리 곡면에 붙어 바깥을 향한다 */
    v3 fc = V(dl->flower_side * 0.074f, 1.552f, 0.082f);
    v3 fnrm = nrm(sub(fc, V(0, 1.44f, -0.01f)));
    dl->flower_c = add(fc, mul(fnrm, 0.006f));
    dl->flower_n = fnrm;
    dl->flower_u = nrm(cross(fnrm, V(0, 1, 0)));
    dl->flower_v = cross(dl->flower_u, fnrm);
    if (dl->outfit == OUTFIT_WEDDING) {
        dl->veil_on = 1;
        dl->noveil = malloc(sizeof *dl);
        if (dl->noveil) { *dl->noveil = *dl; dl->noveil->veil_on = 0; dl->noveil->noveil = NULL; }
    }
    df__recipe_free(r);
    return dl;
}

df_doll *df_doll_from_recipe(const char *path, char *err, size_t errlen) {
    df_recipe *r = df__recipe_file(path, err, errlen);
    return r ? doll_from_recipe(r, err, errlen) : NULL;
}

df_doll *df_doll_from_json(const char *json, size_t len, const char *base_dir, char *err, size_t errlen) {
    df_recipe *r = df__recipe_json(json, len, base_dir, err, errlen);
    return r ? doll_from_recipe(r, err, errlen) : NULL;
}

void df_doll_free(df_doll *dl) {
    if (dl) free(dl->noveil);
    free(dl);
}
