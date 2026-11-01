/*
 * libdollyforge — Dolly Forge 의 C 코어.
 *
 * 마인크래프트 64x64 스킨의 UV 레이아웃, PNG 입출력, 레시피(.dolly.json) 실행,
 * 검증을 담당한다. C++ CLI / Java / Python / C# / Rust 도구는 모두 이 라이브러리
 * (또는 같은 규격)를 기준으로 동작한다. 외부 의존성 없음 (C99).
 */
#ifndef DOLLYFORGE_H
#define DOLLYFORGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define DF_API __declspec(dllexport)
#else
#  define DF_API __attribute__((visibility("default")))
#endif

#define DF_SKIN_W 64
#define DF_SKIN_H 64
#define DF_SKIN_BYTES (DF_SKIN_W * DF_SKIN_H * 4)

/* 팔 두께: classic(Steve, 4px) / slim(Alex, 3px) */
typedef enum { DF_MODEL_CLASSIC = 0, DF_MODEL_SLIM = 1 } df_model;

typedef enum {
    DF_PART_HEAD = 0, DF_PART_BODY, DF_PART_ARM_R, DF_PART_ARM_L,
    DF_PART_LEG_R, DF_PART_LEG_L, DF_PART_COUNT
} df_part;

/* base = 피부/옷 1층(불투명이어야 함), overlay = 2층(모자·재킷·소매·바지 레이어) */
typedef enum { DF_LAYER_BASE = 0, DF_LAYER_OVERLAY = 1, DF_LAYER_COUNT } df_layer;

typedef enum {
    DF_FACE_TOP = 0, DF_FACE_BOTTOM, DF_FACE_RIGHT, DF_FACE_FRONT,
    DF_FACE_LEFT, DF_FACE_BACK, DF_FACE_COUNT
} df_face;

typedef struct df_skin df_skin;

DF_API const char *df_version(void);

/* ---- 스킨 ---- */
DF_API df_skin *df_skin_new(df_model model);           /* 전부 투명 */
DF_API void df_skin_free(df_skin *skin);
DF_API df_model df_skin_model(const df_skin *skin);
DF_API void df_skin_set_model(df_skin *skin, df_model model);
DF_API uint8_t *df_skin_rgba(df_skin *skin);           /* 64*64*4, 행 우선 RGBA */
DF_API const uint8_t *df_skin_rgba_const(const df_skin *skin);

DF_API int df_skin_save_png(const df_skin *skin, const char *path, char *err, size_t errlen);
/* 64x64 와 구형 64x32(자동 변환) 를 읽는다. 모델은 픽셀로 추정한다. */
DF_API df_skin *df_skin_load_png(const char *path, char *err, size_t errlen);
DF_API df_model df_detect_model(const uint8_t *rgba64);

/* ---- UV 레이아웃 ---- */
/* part/layer/face 의 텍스처 사각형. 성공 시 1 */
DF_API int df_face_rect(df_model model, df_part part, df_layer layer, df_face face,
                        int *x, int *y, int *w, int *h);
/* 파트 박스 크기(픽셀 단위 너비·높이·깊이) */
DF_API void df_part_size(df_model model, df_part part, int *w, int *h, int *d);
DF_API const char *df_part_name(df_part part);
DF_API const char *df_face_name(df_face face);
DF_API int df_part_from_name(const char *name);       /* 실패 시 -1 */
DF_API int df_face_from_name(const char *name);       /* 실패 시 -1 */

/* ---- 레시피 ---- */
/* .dolly.json 을 실행해 스킨을 만든다. extends 는 파일 기준 상대 경로. */
DF_API df_skin *df_forge_file(const char *recipe_path, char *err, size_t errlen);
/* base_dir: extends 를 풀 기준 디렉터리 (NULL 이면 현재 디렉터리) */
DF_API df_skin *df_forge_json(const char *json, size_t len, const char *base_dir,
                              char *err, size_t errlen);

/* ---- 검증 ---- */
/* 결과를 줄 단위 "ERROR ..." / "WARN ..." 로 buf 에 기록. 반환값 = 오류 수 */
DF_API int df_validate(const df_skin *skin, char *buf, size_t buflen);

/* ---- 미리보기 ---- */
/* 정면(view=0) / 후면(view=1) 을 펼친 16x32 RGBA 그림 (overlay 합성 포함) */
DF_API void df_render_flat(const df_skin *skin, int view, uint8_t *out16x32);

/* ---- 비교 (무단 변형 감지) ---- */
/* 파트별로 달라진 픽셀 수를 diff[part] 에 기록. 반환값 = 전체 다른 픽셀 수 */
DF_API int df_diff(const df_skin *a, const df_skin *b, int diff[DF_PART_COUNT]);

/* ---- 곡선 3D (doll) ----
 * 레시피의 "doll" 섹션(체형·메이크업·머리·의상)과 같은 팔레트로 매끈한 3D 캐릭터를 만든다.
 * 체형 기본값은 03_Sources/3D_Models 레퍼런스 실측값 그대로. */
typedef struct df_doll df_doll;
#define DF_RENDER_BACKGROUND 1   /* 스튜디오 배경 (없으면 투명) */
#define DF_RENDER_HQ 2           /* 2x2 슈퍼샘플링 */
#define DF_RENDER_TALL 4         /* 모자 높이까지 담는 고정 프레임 (여러 캐릭터를 같은 축척으로) */
DF_API df_doll *df_doll_from_recipe(const char *recipe_path, char *err, size_t errlen);
DF_API df_doll *df_doll_from_json(const char *json, size_t len, const char *base_dir, char *err, size_t errlen);
DF_API void df_doll_free(df_doll *doll);
/* yaw: 0 = 정면, + 면 캐릭터 오른쪽 옆모습이 보인다. pitch: 카메라 높이각.
 * zoom: 1 = 전신, 3.5 = 얼굴 클로즈업. rgba = w*h*4 */
DF_API int df_doll_render(const df_doll *doll, int w, int h, double yaw_deg, double pitch_deg, double zoom, int flags, uint8_t *rgba);

/* 곡선 3D 를 삼각형 메시로 (cell: 격자 크기 m, 작을수록 촘촘).
 * OBJ: obj_path 옆에 같은 이름의 .mtl 과 .png(256x256 팔레트 텍스처)도 쓴다 — Blender · Blockbench 용.
 * Figura: dir 안에 avatar.json + model.bbmodel(머리·몸·팔·다리 그룹) + script.lua — Java Edition 의 Figura 모드용. */
DF_API int df_doll_export_obj(const df_doll *doll, const char *obj_path, double cell_m, char *err, size_t errlen);
DF_API int df_doll_export_figura(const df_doll *doll, const char *dir, const char *name, double cell_m, char *err, size_t errlen);

/* ---- 유틸 ---- */
DF_API uint32_t df_crc32(const uint8_t *data, size_t len, uint32_t crc);
DF_API int df_png_encode(const uint8_t *rgba, int w, int h, uint8_t **out, size_t *outlen);
DF_API int df_png_decode(const uint8_t *data, size_t len, uint8_t **rgba, int *w, int *h,
                         char *err, size_t errlen);
DF_API void df_free(void *p);

#ifdef __cplusplus
}
#endif
#endif
