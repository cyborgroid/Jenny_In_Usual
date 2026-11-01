/* libdollyforge 단위 테스트 (외부 프레임워크 없이) */
#include "dollyforge.h"
#include "dollyforge_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_layout(void) {
    int x, y, w, h;
    /* 마인크래프트 위키의 64x64 규격 값과 대조 */
    CHECK(df_face_rect(DF_MODEL_CLASSIC, DF_PART_HEAD, DF_LAYER_BASE, DF_FACE_FRONT, &x, &y, &w, &h));
    CHECK(x == 8 && y == 8 && w == 8 && h == 8);
    df_face_rect(DF_MODEL_CLASSIC, DF_PART_HEAD, DF_LAYER_OVERLAY, DF_FACE_BACK, &x, &y, &w, &h);
    CHECK(x == 56 && y == 8);
    df_face_rect(DF_MODEL_CLASSIC, DF_PART_BODY, DF_LAYER_BASE, DF_FACE_BACK, &x, &y, &w, &h);
    CHECK(x == 32 && y == 20 && w == 8 && h == 12);
    df_face_rect(DF_MODEL_SLIM, DF_PART_ARM_R, DF_LAYER_BASE, DF_FACE_BACK, &x, &y, &w, &h);
    CHECK(x == 51 && y == 20 && w == 3 && h == 12);
    df_face_rect(DF_MODEL_SLIM, DF_PART_ARM_L, DF_LAYER_OVERLAY, DF_FACE_TOP, &x, &y, &w, &h);
    CHECK(x == 52 && y == 48 && w == 3 && h == 4);
    df_face_rect(DF_MODEL_CLASSIC, DF_PART_LEG_L, DF_LAYER_OVERLAY, DF_FACE_FRONT, &x, &y, &w, &h);
    CHECK(x == 4 && y == 52 && w == 4 && h == 12);
    CHECK(df_part_from_name("arm_l") == DF_PART_ARM_L);
    CHECK(df_face_from_name("nope") == -1);
}

static void test_json(void) {
    char err[128];
    const char *text = "{ // 주석\n \"a\": [1, 2.5, -3e2], \"s\": \"\\u00e9\\n\", \"t\": true, \"n\": null, }";
    df_json *v = df_json_parse(text, strlen(text), err, sizeof err);
    CHECK(v != NULL);
    CHECK(df_json_len(df_json_get(v, "a")) == 3);
    CHECK(df_json_num(df_json_at(df_json_get(v, "a"), 2)) == -300);
    CHECK(!strcmp(df_json_str(df_json_get(v, "s")), "\xC3\xA9\n"));
    CHECK(df_json_bool(df_json_get(v, "t")));
    CHECK(df_json_kind(df_json_get(v, "n")) == DF_JSON_NULL);
    df_json_free(v);
    CHECK(df_json_parse("{\"a\":}", 6, err, sizeof err) == NULL);
    CHECK(strstr(err, "JSON") != NULL);
}

static void test_png_roundtrip(void) {
    uint8_t px[5 * 3 * 4];
    for (size_t i = 0; i < sizeof px; i++) px[i] = (uint8_t)(i * 37);
    uint8_t *png, *back;
    size_t len;
    int w, h;
    char err[128];
    CHECK(df_png_encode(px, 5, 3, &png, &len));
    CHECK(df_png_decode(png, len, &back, &w, &h, err, sizeof err));
    CHECK(w == 5 && h == 3 && !memcmp(px, back, sizeof px));
    df_free(png);
    df_free(back);
    CHECK(df_crc32((const uint8_t *)"123456789", 9, 0) == 0xCBF43926u);
}

static void test_forge(void) {
    char err[256];
    const char *recipe =
        "{ \"dollyforge\": 1, \"model\": \"slim\","
        "  \"palette\": { \"skin\": \"#F0C8A0\", \"shirt\": \"#7B3FB5\", \"alias\": \"shirt\" },"
        "  \"legend\": { \"S\": \"skin\", \"A\": \"alias\" },"
        "  \"ops\": ["
        "    { \"op\": \"fill\", \"part\": \"*\", \"faces\": \"all\", \"color\": \"skin\" },"
        "    { \"op\": \"band\", \"part\": \"arm_r\", \"y\": 0, \"h\": 2, \"color\": \"shirt\" },"
        "    { \"op\": \"pixels\", \"part\": \"head\", \"face\": \"front\", \"y\": 7, \"rows\": [\"A.A\"] },"
        "    { \"op\": \"mirror\", \"from\": \"arm_r\", \"to\": \"arm_l\" }"
        "  ] }";
    df_skin *s = df_forge_json(recipe, strlen(recipe), NULL, err, sizeof err);
    CHECK(s != NULL);
    if (!s) { fprintf(stderr, "  %s\n", err); return; }
    CHECK(df_skin_model(s) == DF_MODEL_SLIM);
    const uint8_t *px = df_skin_rgba(s);
    /* head front (8,8) 의 7번째 줄 첫 픽셀 = shirt (alias 경유) */
    const uint8_t *p = &px[((8 + 7) * 64 + 8) * 4];
    CHECK(p[0] == 0x7B && p[1] == 0x3F && p[2] == 0xB5 && p[3] == 255);
    /* 미러: 왼팔 정면 첫 줄도 shirt */
    int x, y, w, h;
    df_face_rect(DF_MODEL_SLIM, DF_PART_ARM_L, DF_LAYER_BASE, DF_FACE_FRONT, &x, &y, &w, &h);
    p = &px[(y * 64 + x) * 4];
    CHECK(p[0] == 0x7B && p[3] == 255);
    char report[1024];
    CHECK(df_validate(s, report, sizeof report) == 0);
    CHECK(df_detect_model(px) == DF_MODEL_SLIM);

    uint8_t flat[16 * 32 * 4];
    df_render_flat(s, 0, flat);
    CHECK(flat[(0 * 16 + 4) * 4 + 3] == 255);  /* 머리 */
    CHECK(flat[(8 * 16 + 0) * 4 + 3] == 0);    /* slim 팔 바깥 1열은 비어 있음 */

    df_skin *t = df_forge_json(recipe, strlen(recipe), NULL, err, sizeof err);
    int diff[DF_PART_COUNT];
    CHECK(df_diff(s, t, diff) == 0);
    df_skin_rgba(t)[((8 + 3) * 64 + 10) * 4] ^= 0xFF; /* 얼굴 한 픽셀 변조 */
    CHECK(df_diff(s, t, diff) == 1 && diff[DF_PART_HEAD] == 1);
    df_skin_free(t);
    df_skin_free(s);

    const char *bad = "{ \"ops\": [ { \"op\": \"fill\", \"part\": \"tail\", \"color\": \"#fff\" } ] }";
    CHECK(df_forge_json(bad, strlen(bad), NULL, err, sizeof err) == NULL);
    CHECK(strstr(err, "unknown part 'tail'") != NULL);
    const char *derived = "{ \"palette\": {\"a\": \"#804020\", \"b\": \"a*0.5\"},"
                          "  \"ops\": [ { \"op\": \"fill\", \"part\": \"head\", \"color\": \"b\" } ] }";
    s = df_forge_json(derived, strlen(derived), NULL, err, sizeof err);
    CHECK(s != NULL);
    if (s) {
        const uint8_t *q = &df_skin_rgba(s)[(8 * 64 + 8) * 4];
        CHECK(q[0] == 0x40 && q[1] == 0x20 && q[2] == 0x10 && q[3] == 255);
        df_skin_free(s);
    }
    const char *bad2 = "{ \"palette\": {\"a\": \"b\", \"b\": \"a\"}, \"ops\": [] }";
    CHECK(df_forge_json(bad2, strlen(bad2), NULL, err, sizeof err) == NULL);
    CHECK(strstr(err, "loop") != NULL);
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

/* extends 배열: 공유 조상은 한 번만, 순서는 [얼굴, 의상] → 자식 */
static void test_extends_array(void) {
    char err[256];
    write_text("build/t_root.dolly.json",
        "{ \"palette\": {\"a\": \"#FF0000\"}, \"ops\": [ {\"op\":\"fill\",\"part\":\"*\",\"color\":\"a\"} ] }");
    write_text("build/t_face.dolly.json",
        "{ \"extends\": \"t_root.dolly.json\", \"palette\": {\"a\": \"#00FF00\"} }");
    write_text("build/t_outfit.dolly.json",
        "{ \"extends\": \"./t_root.dolly.json\", \"ops\": [ {\"op\":\"fill\",\"part\":\"body\",\"color\":\"#0000FF\"} ] }");
    write_text("build/t_child.dolly.json",
        "{ \"extends\": [\"t_face.dolly.json\", \"sub/../t_outfit.dolly.json\"] }");
    df_skin *s = df_forge_file("build/t_child.dolly.json", err, sizeof err);
    CHECK(s != NULL);
    if (!s) { fprintf(stderr, "  %s\n", err); return; }
    const uint8_t *head = &df_skin_rgba(s)[(8 * 64 + 8) * 4], *body = &df_skin_rgba(s)[(20 * 64 + 20) * 4];
    CHECK(head[1] == 0xFF && head[0] == 0);   /* 얼굴 쪽 팔레트가 루트를 덮어씀 */
    CHECK(body[2] == 0xFF && body[0] == 0);   /* 의상은 루트 다음에 실행됨 (루트 중복 실행 없음) */
    df_skin_free(s);
    write_text("build/t_loop.dolly.json", "{ \"extends\": \"t_loop.dolly.json\" }");
    CHECK(df_forge_file("build/t_loop.dolly.json", err, sizeof err) == NULL);
}

static void test_validate_holes(void) {
    df_skin *s = df_skin_new(DF_MODEL_CLASSIC);
    char report[4096];
    CHECK(df_validate(s, report, sizeof report) > 0);
    CHECK(strstr(report, "ERROR base head/front") != NULL);
    df_skin_free(s);
}

int main(void) {
    test_layout();
    test_json();
    test_png_roundtrip();
    test_forge();
    test_validate_holes();
    test_extends_array();
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    printf("libdollyforge %s: all core tests passed\n", df_version());
    return 0;
}
