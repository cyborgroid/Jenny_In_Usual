/* PNG 입출력: 인코더(무압축 deflate 블록) + 디코더(inflate 전체 구현) */
#include "dollyforge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- CRC32 / Adler32 ---------------- */

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_ready = 1;
}

uint32_t df_crc32(const uint8_t *data, size_t len, uint32_t crc) {
    if (!crc_ready) crc_init();
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint32_t adler32(const uint8_t *d, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
    return b << 16 | a;
}

void df_free(void *p) { free(p); }

/* ---------------- 인코더 ---------------- */

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }

static uint8_t *put_chunk(uint8_t *o, const char *type, const uint8_t *data, uint32_t len) {
    be32(o, len);
    memcpy(o + 4, type, 4);
    if (len) memcpy(o + 8, data, len);
    be32(o + 8 + len, df_crc32(o + 4, len + 4, 0));
    return o + 12 + len;
}

int df_png_encode(const uint8_t *rgba, int w, int h, uint8_t **out, size_t *outlen) {
    if (w <= 0 || h <= 0) return 0;
    size_t stride = (size_t)w * 4, raw_len = (stride + 1) * (size_t)h;
    uint8_t *raw = malloc(raw_len);
    if (!raw) return 0;
    for (int y = 0; y < h; y++) {
        raw[y * (stride + 1)] = 0; /* filter: None */
        memcpy(raw + y * (stride + 1) + 1, rgba + y * stride, stride);
    }
    size_t blocks = raw_len / 65535 + 1;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    uint8_t *z = malloc(z_len), *zp = z;
    if (!z) { free(raw); return 0; }
    *zp++ = 0x78; *zp++ = 0x01;
    for (size_t off = 0; off < raw_len || off == 0;) {
        size_t n = raw_len - off > 65535 ? 65535 : raw_len - off;
        *zp++ = off + n >= raw_len ? 1 : 0; /* BFINAL, BTYPE=00 */
        *zp++ = (uint8_t)n; *zp++ = (uint8_t)(n >> 8);
        *zp++ = (uint8_t)~n; *zp++ = (uint8_t)(~n >> 8);
        memcpy(zp, raw + off, n);
        zp += n; off += n;
        if (n == 0) break;
    }
    be32(zp, adler32(raw, raw_len)); zp += 4;
    z_len = (size_t)(zp - z);
    free(raw);

    size_t total = 8 + (12 + 13) + (12 + z_len) + 12;
    uint8_t *png = malloc(total), *o = png;
    if (!png) { free(z); return 0; }
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    memcpy(o, sig, 8); o += 8;
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w); be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    o = put_chunk(o, "IHDR", ihdr, 13);
    o = put_chunk(o, "IDAT", z, (uint32_t)z_len);
    o = put_chunk(o, "IEND", NULL, 0);
    free(z);
    *out = png;
    *outlen = (size_t)(o - png);
    return 1;
}

/* ---------------- inflate (RFC 1951) ---------------- */

typedef struct {
    const uint8_t *in, *end;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t olen, ocap, olimit;
    int bad;
} inflater;

typedef struct { short count[16]; short symbol[288]; } huffman;

static int bits(inflater *s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in >= s->end) { s->bad = 1; return 0; }
        val |= (uint32_t)(*s->in++) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

static int emit(inflater *s, uint8_t b) {
    if (s->olen == s->ocap) {
        size_t cap = s->ocap ? s->ocap * 2 : 4096;
        if (cap > s->olimit) cap = s->olimit;
        if (s->olen == cap) { s->bad = 1; return 0; }
        uint8_t *o = realloc(s->out, cap);
        if (!o) { s->bad = 1; return 0; }
        s->out = o; s->ocap = cap;
    }
    s->out[s->olen++] = b;
    return 1;
}

static int build(huffman *h, const short *length, int n) {
    short offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1; /* 과포화 */
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int sym = 0; sym < n; sym++)
        if (length[sym]) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static int decode(inflater *s, const huffman *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bits(s, 1);
        if (s->bad) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count;
        first <<= 1; code <<= 1;
    }
    s->bad = 1;
    return -1;
}

static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                 8193, 12289, 16385, 24577 };
static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(inflater *s, const huffman *lencode, const huffman *distcode) {
    for (;;) {
        int sym = decode(s, lencode);
        if (sym < 0) return 0;
        if (sym < 256) { if (!emit(s, (uint8_t)sym)) return 0; continue; }
        if (sym == 256) return 1;
        sym -= 257;
        if (sym >= 29) { s->bad = 1; return 0; }
        int len = lbase[sym] + bits(s, lext[sym]);
        int dsym = decode(s, distcode);
        if (dsym < 0 || dsym >= 30) { s->bad = 1; return 0; }
        size_t dist = (size_t)(dbase[dsym] + bits(s, dext[dsym]));
        if (s->bad || dist > s->olen) { s->bad = 1; return 0; }
        while (len--) if (!emit(s, s->out[s->olen - dist])) return 0;
    }
}

static int inflate_stream(inflater *s) {
    static huffman fixed_len, fixed_dist;
    static int fixed_ready;
    int last;
    do {
        last = bits(s, 1);
        int type = bits(s, 2);
        if (s->bad) return 0;
        if (type == 0) {
            s->bitbuf = 0; s->bitcnt = 0; /* 바이트 경계로 */
            if (s->end - s->in < 4) return 0;
            unsigned len = s->in[0] | s->in[1] << 8, nlen = s->in[2] | s->in[3] << 8;
            s->in += 4;
            if (len != (~nlen & 0xFFFF) || (size_t)(s->end - s->in) < len) return 0;
            while (len--) if (!emit(s, *s->in++)) return 0;
        } else if (type == 1) {
            if (!fixed_ready) {
                short l[288];
                int i = 0;
                for (; i < 144; i++) l[i] = 8;
                for (; i < 256; i++) l[i] = 9;
                for (; i < 280; i++) l[i] = 7;
                for (; i < 288; i++) l[i] = 8;
                build(&fixed_len, l, 288);
                for (i = 0; i < 30; i++) l[i] = 5;
                build(&fixed_dist, l, 30);
                fixed_ready = 1;
            }
            if (!codes(s, &fixed_len, &fixed_dist)) return 0;
        } else if (type == 2) {
            static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            short lengths[320];
            huffman lencode, distcode;
            int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
            if (s->bad || nlen > 286 || ndist > 30) return 0;
            int i = 0;
            for (; i < ncode; i++) lengths[order[i]] = (short)bits(s, 3);
            for (; i < 19; i++) lengths[order[i]] = 0;
            if (build(&lencode, lengths, 19) != 0) return 0;
            for (i = 0; i < nlen + ndist;) {
                int sym = decode(s, &lencode);
                if (sym < 0) return 0;
                if (sym < 16) { lengths[i++] = (short)sym; continue; }
                short len = 0;
                int rep;
                if (sym == 16) {
                    if (i == 0) return 0;
                    len = lengths[i - 1];
                    rep = 3 + bits(s, 2);
                } else if (sym == 17) rep = 3 + bits(s, 3);
                else rep = 11 + bits(s, 7);
                if (i + rep > nlen + ndist) return 0;
                while (rep--) lengths[i++] = len;
            }
            if (lengths[256] == 0) return 0;
            int err = build(&lencode, lengths, nlen);
            if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return 0;
            err = build(&distcode, lengths + nlen, ndist);
            if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return 0;
            if (!codes(s, &lencode, &distcode)) return 0;
        } else return 0;
    } while (!last);
    return !s->bad;
}

/* ---------------- 디코더 ---------------- */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

#define DECODE_FAIL(msg) do { if (err && errlen) snprintf(err, errlen, "PNG: %s", msg); goto done; } while (0)

int df_png_decode(const uint8_t *data, size_t len, uint8_t **rgba_out, int *w_out, int *h_out,
                  char *err, size_t errlen) {
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    int ok = 0;
    uint8_t *idat = NULL, *rgba = NULL;
    inflater s = { 0 };
    size_t idat_len = 0;
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = -1, interlace = 0;
    uint8_t pal[256][4];
    int npal = 0;
    int trns_gray = -1, trns_r = -1, trns_g = -1, trns_b = -1;
    if (err && errlen) err[0] = 0;
    memset(pal, 255, sizeof pal);

    if (len < 8 || memcmp(data, sig, 8)) DECODE_FAIL("not a PNG file");
    for (size_t off = 8; off + 12 <= len;) {
        uint32_t clen = rd32(data + off);
        const uint8_t *type = data + off + 4, *cd = data + off + 8;
        if (clen > len - off - 12) DECODE_FAIL("truncated chunk");
        if (!memcmp(type, "IHDR", 4)) {
            if (clen < 13) DECODE_FAIL("bad IHDR");
            w = rd32(cd); h = rd32(cd + 4); depth = cd[8]; ctype = cd[9]; interlace = cd[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            npal = (int)(clen / 3 > 256 ? 256 : clen / 3);
            for (int i = 0; i < npal; i++) { pal[i][0] = cd[i * 3]; pal[i][1] = cd[i * 3 + 1]; pal[i][2] = cd[i * 3 + 2]; }
        } else if (!memcmp(type, "tRNS", 4)) {
            if (ctype == 3) for (uint32_t i = 0; i < clen && i < 256; i++) pal[i][3] = cd[i];
            else if (ctype == 0 && clen >= 2) trns_gray = cd[0] << 8 | cd[1];
            else if (ctype == 2 && clen >= 6) { trns_r = cd[0] << 8 | cd[1]; trns_g = cd[2] << 8 | cd[3]; trns_b = cd[4] << 8 | cd[5]; }
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t *n = realloc(idat, idat_len + clen);
            if (!n) DECODE_FAIL("out of memory");
            idat = n;
            memcpy(idat + idat_len, cd, clen);
            idat_len += clen;
        } else if (!memcmp(type, "IEND", 4)) break;
        off += 12 + clen;
    }
    if (!w || !h || w > 8192 || h > 8192) DECODE_FAIL("bad dimensions");
    if (interlace) DECODE_FAIL("interlaced PNG is not supported");
    int channels;
    switch (ctype) {
    case 0: channels = 1; break;
    case 2: channels = 3; break;
    case 3: channels = 1; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    default: DECODE_FAIL("unknown color type");
    }
    if (!(depth == 8 || depth == 16 || ((ctype == 0 || ctype == 3) && (depth == 1 || depth == 2 || depth == 4))))
        DECODE_FAIL("unsupported bit depth");
    if (ctype == 3 && depth == 16) DECODE_FAIL("bad palette depth");
    if (idat_len < 6) DECODE_FAIL("missing image data");

    size_t stride = ((size_t)w * channels * depth + 7) / 8;
    size_t bpp = (size_t)(channels * depth + 7) / 8;
    s.in = idat + 2; /* zlib 헤더 건너뜀 */
    s.end = idat + idat_len;
    s.olimit = (stride + 1) * h;
    if (!inflate_stream(&s) || s.olen < s.olimit) DECODE_FAIL("corrupt image data");

    /* 필터 해제 (제자리) */
    uint8_t *prev = NULL;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = s.out + y * (stride + 1);
        int f = row[0];
        uint8_t *px = row + 1;
        for (size_t i = 0; i < stride; i++) {
            int a = i >= bpp ? px[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = prev && i >= bpp ? prev[i - bpp] : 0;
            switch (f) {
            case 0: break;
            case 1: px[i] = (uint8_t)(px[i] + a); break;
            case 2: px[i] = (uint8_t)(px[i] + b); break;
            case 3: px[i] = (uint8_t)(px[i] + ((a + b) >> 1)); break;
            case 4: px[i] = (uint8_t)(px[i] + paeth(a, b, c)); break;
            default: DECODE_FAIL("bad filter type");
            }
        }
        prev = px;
    }

    rgba = malloc((size_t)w * h * 4);
    if (!rgba) DECODE_FAIL("out of memory");
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *px = s.out + y * (stride + 1) + 1;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *o = rgba + ((size_t)y * w + x) * 4;
            int sample[4] = { 0, 0, 0, 0 };
            if (depth < 8) {
                size_t bit = (size_t)x * depth;
                int v = (px[bit / 8] >> (8 - depth - (int)(bit % 8))) & ((1 << depth) - 1);
                sample[0] = ctype == 3 ? v : v * 255 / ((1 << depth) - 1);
            } else {
                for (int c = 0; c < channels; c++)
                    sample[c] = depth == 8 ? px[(size_t)x * channels + c]
                                           : (px[((size_t)x * channels + c) * 2] << 8 | px[((size_t)x * channels + c) * 2 + 1]);
            }
            int full = depth == 16 ? 257 : 1; /* 16비트 → 8비트 */
            switch (ctype) {
            case 0: {
                int g = depth < 8 ? sample[0] : sample[0] / full;
                o[0] = o[1] = o[2] = (uint8_t)g;
                o[3] = depth < 8 ? (trns_gray >= 0 && sample[0] * ((1 << depth) - 1) / 255 == trns_gray ? 0 : 255)
                                 : (sample[0] == trns_gray ? 0 : 255);
                break;
            }
            case 2:
                o[0] = (uint8_t)(sample[0] / full); o[1] = (uint8_t)(sample[1] / full); o[2] = (uint8_t)(sample[2] / full);
                o[3] = sample[0] == trns_r && sample[1] == trns_g && sample[2] == trns_b ? 0 : 255;
                break;
            case 3:
                if (sample[0] >= npal) DECODE_FAIL("palette index out of range");
                memcpy(o, pal[sample[0]], 4);
                break;
            case 4:
                o[0] = o[1] = o[2] = (uint8_t)(sample[0] / full); o[3] = (uint8_t)(sample[1] / full);
                break;
            case 6:
                for (int c = 0; c < 4; c++) o[c] = (uint8_t)(sample[c] / full);
                break;
            }
        }
    }
    *rgba_out = rgba;
    rgba = NULL;
    *w_out = (int)w;
    *h_out = (int)h;
    ok = 1;
done:
    free(rgba);
    free(idat);
    free(s.out);
    return ok;
}
