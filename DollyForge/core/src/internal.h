/* libdollyforge 내부 API — 공개 헤더에 넣지 않는다 */
#ifndef DOLLYFORGE_INTERNAL_H
#define DOLLYFORGE_INTERNAL_H

#include "dollyforge_json.h"

/* extends 체인을 펼치고 팔레트를 합친 레시피 */
typedef struct df_recipe df_recipe;

df_recipe *df__recipe_file(const char *path, char *err, size_t errlen);
df_recipe *df__recipe_json(const char *json, size_t len, const char *base_dir, char *err, size_t errlen);
void df__recipe_free(df_recipe *r);
/* 팔레트 키 / #hex / 파생색 해석. 실패하면 0 */
int df__recipe_color(df_recipe *r, const char *spec, uint8_t rgba[4]);
/* "doll": { section: { key: ... } } — 체인 뒤쪽(자식)부터 찾는다. key 가 NULL 이면 섹션 자체 */
const df_json *df__recipe_doll(const df_recipe *r, const char *section, const char *key);

#endif
