/* 재귀 하강 JSON 파서 (RFC 8259, UTF-8 그대로 보존, \uXXXX 는 UTF-8 로 변환) */
#include "dollyforge_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct df_json {
    df_json_type type;
    double num;
    char *str;          /* STRING */
    size_t n, cap;      /* ARRAY / OBJECT */
    df_json **items;
    char **keys;        /* OBJECT 만 */
};

typedef struct {
    const char *s, *end;
    char *err;
    size_t errlen;
    int depth;
} parser;

static void fail(parser *p, const char *msg) {
    if (p->err && p->errlen && !p->err[0]) snprintf(p->err, p->errlen, "JSON: %s", msg);
}

static void skip_ws(parser *p) {
    for (;;) {
        while (p->s < p->end && (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r')) p->s++;
        /* 레시피 편의를 위해 // 주석을 허용한다 (표준 밖) */
        if (p->s + 1 < p->end && p->s[0] == '/' && p->s[1] == '/') {
            while (p->s < p->end && *p->s != '\n') p->s++;
            continue;
        }
        return;
    }
}

static df_json *new_val(df_json_type t) {
    df_json *v = calloc(1, sizeof *v);
    if (v) v->type = t;
    return v;
}

static int push(df_json *arr, char *key, df_json *item) {
    if (arr->n == arr->cap) {
        size_t cap = arr->cap ? arr->cap * 2 : 8;
        df_json **items = realloc(arr->items, cap * sizeof *items);
        if (!items) return 0;
        arr->items = items;
        if (arr->type == DF_JSON_OBJECT) {
            char **keys = realloc(arr->keys, cap * sizeof *keys);
            if (!keys) return 0;
            arr->keys = keys;
        }
        arr->cap = cap;
    }
    if (arr->type == DF_JSON_OBJECT) arr->keys[arr->n] = key;
    arr->items[arr->n++] = item;
    return 1;
}

static void put_utf8(char **o, unsigned cp) {
    char *b = *o;
    if (cp < 0x80) *b++ = (char)cp;
    else if (cp < 0x800) { *b++ = (char)(0xC0 | cp >> 6); *b++ = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        *b++ = (char)(0xE0 | cp >> 12); *b++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *b++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *b++ = (char)(0xF0 | cp >> 18); *b++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *b++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *b++ = (char)(0x80 | (cp & 0x3F));
    }
    *o = b;
}

static int hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static char *parse_string(parser *p) {
    if (p->s >= p->end || *p->s != '"') { fail(p, "expected string"); return NULL; }
    p->s++;
    const char *start = p->s;
    while (p->s < p->end && *p->s != '"') { if (*p->s == '\\') p->s++; p->s++; }
    if (p->s >= p->end) { fail(p, "unterminated string"); return NULL; }
    /* 이스케이프를 풀면 길이는 줄어들기만 한다 (\u → 최대 4바이트 ≤ 6글자) */
    char *out = malloc((size_t)(p->s - start) + 1), *o = out;
    if (!out) return NULL;
    for (const char *c = start; c < p->s; c++) {
        if (*c != '\\') { *o++ = *c; continue; }
        c++;
        switch (*c) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'u': {
            unsigned cp;
            if (c + 4 >= p->s || !hex4(c + 1, &cp)) { free(out); fail(p, "bad \\u escape"); return NULL; }
            c += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && c + 6 < p->s && c[1] == '\\' && c[2] == 'u') {
                unsigned lo;
                if (hex4(c + 3, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    c += 6;
                }
            }
            put_utf8(&o, cp);
            break;
        }
        default: *o++ = *c;
        }
    }
    *o = 0;
    p->s++;
    return out;
}

static df_json *parse_value(parser *p);

static df_json *parse_container(parser *p, int object) {
    df_json *v = new_val(object ? DF_JSON_OBJECT : DF_JSON_ARRAY);
    if (!v) return NULL;
    p->s++;
    skip_ws(p);
    if (p->s < p->end && *p->s == (object ? '}' : ']')) { p->s++; return v; }
    for (;;) {
        char *key = NULL;
        skip_ws(p);
        if (object) {
            key = parse_string(p);
            if (!key) goto bad;
            skip_ws(p);
            if (p->s >= p->end || *p->s != ':') { free(key); fail(p, "expected ':'"); goto bad; }
            p->s++;
        }
        df_json *item = parse_value(p);
        if (!item) { free(key); goto bad; }
        if (!push(v, key, item)) { free(key); df_json_free(item); goto bad; }
        skip_ws(p);
        if (p->s < p->end && *p->s == ',') {
            p->s++;
            skip_ws(p);
            /* 끝 쉼표 허용 */
            if (p->s < p->end && *p->s == (object ? '}' : ']')) { p->s++; return v; }
            continue;
        }
        if (p->s < p->end && *p->s == (object ? '}' : ']')) { p->s++; return v; }
        fail(p, object ? "expected ',' or '}'" : "expected ',' or ']'");
        goto bad;
    }
bad:
    df_json_free(v);
    return NULL;
}

static df_json *parse_value(parser *p) {
    skip_ws(p);
    if (p->s >= p->end) { fail(p, "unexpected end"); return NULL; }
    if (++p->depth > 64) { fail(p, "nesting too deep"); return NULL; }
    df_json *v = NULL;
    char c = *p->s;
    if (c == '{' || c == '[') v = parse_container(p, c == '{');
    else if (c == '"') {
        char *s = parse_string(p);
        if (s && (v = new_val(DF_JSON_STRING))) v->str = s;
        else free(s);
    } else if (c == 't' && p->end - p->s >= 4 && !memcmp(p->s, "true", 4)) {
        p->s += 4; if ((v = new_val(DF_JSON_BOOL))) v->num = 1;
    } else if (c == 'f' && p->end - p->s >= 5 && !memcmp(p->s, "false", 5)) {
        p->s += 5; v = new_val(DF_JSON_BOOL);
    } else if (c == 'n' && p->end - p->s >= 4 && !memcmp(p->s, "null", 4)) {
        p->s += 4; v = new_val(DF_JSON_NULL);
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char buf[64];
        size_t n = 0;
        while (p->s < p->end && n < sizeof buf - 1 && strchr("+-0123456789.eE", *p->s)) buf[n++] = *p->s++;
        buf[n] = 0;
        char *endp;
        double d = strtod(buf, &endp);
        if (*endp) fail(p, "bad number");
        else if ((v = new_val(DF_JSON_NUMBER))) v->num = d;
    } else fail(p, "unexpected character");
    p->depth--;
    return v;
}

df_json *df_json_parse(const char *text, size_t len, char *err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    parser p = { text, text + len, err, errlen, 0 };
    df_json *v = parse_value(&p);
    if (v) {
        skip_ws(&p);
        if (p.s != p.end) { fail(&p, "trailing characters"); df_json_free(v); return NULL; }
    } else if (err && errlen && !err[0]) {
        snprintf(err, errlen, "JSON: out of memory");
    }
    if (!v && err && errlen) {
        /* 오류 위치(줄 번호)를 덧붙인다 */
        int line = 1;
        for (const char *c = text; c < p.s && c < text + len; c++) if (*c == '\n') line++;
        size_t used = strlen(err);
        if (used + 16 < errlen) snprintf(err + used, errlen - used, " (line %d)", line);
    }
    return v;
}

void df_json_free(df_json *v) {
    if (!v) return;
    for (size_t i = 0; i < v->n; i++) {
        df_json_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->str);
    free(v);
}

df_json_type df_json_kind(const df_json *v) { return v ? v->type : DF_JSON_NULL; }
const char *df_json_str(const df_json *v) { return v && v->type == DF_JSON_STRING ? v->str : NULL; }
double df_json_num(const df_json *v) { return v && v->type == DF_JSON_NUMBER ? v->num : 0; }
int df_json_bool(const df_json *v) { return v && v->type == DF_JSON_BOOL && v->num != 0; }
size_t df_json_len(const df_json *v) {
    return v && (v->type == DF_JSON_ARRAY || v->type == DF_JSON_OBJECT) ? v->n : 0;
}
df_json *df_json_at(const df_json *v, size_t i) { return i < df_json_len(v) ? v->items[i] : NULL; }
const char *df_json_key_at(const df_json *v, size_t i) {
    return v && v->type == DF_JSON_OBJECT && i < v->n ? v->keys[i] : NULL;
}
df_json *df_json_get(const df_json *v, const char *key) {
    if (!v || v->type != DF_JSON_OBJECT) return NULL;
    /* 중복 키는 마지막 값이 이긴다 */
    for (size_t i = v->n; i-- > 0;)
        if (!strcmp(v->keys[i], key)) return v->items[i];
    return NULL;
}
