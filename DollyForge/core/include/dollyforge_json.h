/*
 * libdollyforge 의 작은 JSON 리더. 레시피뿐 아니라 C++ 패키저, Rust 가드도
 * 같은 파서를 써서 모든 도구가 한 가지 해석을 공유한다.
 */
#ifndef DOLLYFORGE_JSON_H
#define DOLLYFORGE_JSON_H

#include "dollyforge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DF_JSON_NULL = 0, DF_JSON_BOOL, DF_JSON_NUMBER, DF_JSON_STRING,
    DF_JSON_ARRAY, DF_JSON_OBJECT
} df_json_type;

typedef struct df_json df_json;

DF_API df_json *df_json_parse(const char *text, size_t len, char *err, size_t errlen);
DF_API void df_json_free(df_json *v);

DF_API df_json_type df_json_kind(const df_json *v);
DF_API const char *df_json_str(const df_json *v);     /* 문자열이 아니면 NULL */
DF_API double df_json_num(const df_json *v);          /* 숫자가 아니면 0 */
DF_API int df_json_bool(const df_json *v);
DF_API size_t df_json_len(const df_json *v);          /* 배열·객체 원소 수 */
DF_API df_json *df_json_at(const df_json *v, size_t i);
DF_API const char *df_json_key_at(const df_json *v, size_t i); /* 객체 i 번째 키 */
DF_API df_json *df_json_get(const df_json *v, const char *key); /* 없으면 NULL */

#ifdef __cplusplus
}
#endif
#endif
