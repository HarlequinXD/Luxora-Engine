/* Image loading: PNG (own inflate implementation), TGA and BMP -> RGBA8. */
#include "../lx_internal.h"

/* ---------------- inflate (RFC 1951) ---------------- */
typedef struct Bits {
    const uint8_t *p, *end;
    uint32_t buf;
    int count;
} Bits;

static int bits_get(Bits *b, int n) {
    while (b->count < n) {
        uint32_t byte = b->p < b->end ? *b->p++ : 0;
        b->buf |= byte << b->count;
        b->count += 8;
    }
    int v = (int)(b->buf & ((1u << n) - 1u));
    b->buf >>= n;
    b->count -= n;
    return v;
}

typedef struct Huff {
    uint16_t counts[16];
    uint16_t symbols[320];
} Huff;

static void huff_build(Huff *h, const uint8_t *lengths, int n) {
    memset(h->counts, 0, sizeof h->counts);
    for (int i = 0; i < n; i++) h->counts[lengths[i]]++;
    h->counts[0] = 0;
    uint16_t offs[16];
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + h->counts[i]);
    for (int i = 0; i < n; i++)
        if (lengths[i]) h->symbols[offs[lengths[i]]++] = (uint16_t)i;
}

static int huff_decode(Bits *b, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bits_get(b, 1);
        int count = h->counts[len];
        if (code - count < first) return h->symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

typedef struct Out { uint8_t *data; size_t len, cap; } Out;
static void out_push(Out *o, uint8_t c) {
    if (o->len == o->cap) { o->cap = o->cap ? o->cap * 2 : 65536; o->data = lx_realloc(o->data, o->cap); }
    o->data[o->len++] = c;
}

static const uint16_t LEN_BASE[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEN_EXTRA[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DIST_BASE[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t DIST_EXTRA[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static bool inflate_block(Bits *b, Out *o, const Huff *lit, const Huff *dist) {
    for (;;) {
        int sym = huff_decode(b, lit);
        if (sym < 0) return false;
        if (sym < 256) out_push(o, (uint8_t)sym);
        else if (sym == 256) return true;
        else {
            sym -= 257;
            if (sym >= 29) return false;
            int len = LEN_BASE[sym] + bits_get(b, LEN_EXTRA[sym]);
            int ds = huff_decode(b, dist);
            if (ds < 0 || ds >= 30) return false;
            size_t d = (size_t)(DIST_BASE[ds] + bits_get(b, DIST_EXTRA[ds]));
            if (d > o->len) return false;
            for (int i = 0; i < len; i++) out_push(o, o->data[o->len - d]);
        }
    }
}

static uint8_t *zlib_inflate(const uint8_t *data, size_t size, size_t *out_len) {
    if (size < 2) return NULL;
    Bits b = {data + 2, data + size, 0, 0}; /* skip zlib header */
    Out o = {0};
    int final;
    do {
        final = bits_get(&b, 1);
        int type = bits_get(&b, 2);
        if (type == 0) {
            b.buf = 0; b.count = 0;
            if (b.p + 4 > b.end) break;
            int len = b.p[0] | (b.p[1] << 8);
            b.p += 4;
            for (int i = 0; i < len && b.p < b.end; i++) out_push(&o, *b.p++);
        } else if (type == 1 || type == 2) {
            Huff lit, dist;
            uint8_t lengths[320];
            if (type == 1) {
                int i = 0;
                for (; i < 144; i++) lengths[i] = 8;
                for (; i < 256; i++) lengths[i] = 9;
                for (; i < 280; i++) lengths[i] = 7;
                for (; i < 288; i++) lengths[i] = 8;
                huff_build(&lit, lengths, 288);
                for (i = 0; i < 30; i++) lengths[i] = 5;
                huff_build(&dist, lengths, 30);
            } else {
                int hlit = bits_get(&b, 5) + 257, hdist = bits_get(&b, 5) + 1, hclen = bits_get(&b, 4) + 4;
                static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                uint8_t cl[19] = {0};
                for (int i = 0; i < hclen; i++) cl[order[i]] = (uint8_t)bits_get(&b, 3);
                Huff clh;
                huff_build(&clh, cl, 19);
                int n = 0;
                while (n < hlit + hdist) {
                    int sym = huff_decode(&b, &clh);
                    if (sym < 0) { lx_free(o.data); return NULL; }
                    if (sym < 16) lengths[n++] = (uint8_t)sym;
                    else {
                        int rep = 0;
                        uint8_t val = 0;
                        if (sym == 16) { if (!n) { lx_free(o.data); return NULL; } val = lengths[n - 1]; rep = 3 + bits_get(&b, 2); }
                        else if (sym == 17) rep = 3 + bits_get(&b, 3);
                        else rep = 11 + bits_get(&b, 7);
                        while (rep-- && n < hlit + hdist) lengths[n++] = val;
                    }
                }
                huff_build(&lit, lengths, hlit);
                huff_build(&dist, lengths + hlit, hdist);
            }
            if (!inflate_block(&b, &o, &lit, &dist)) { lx_free(o.data); return NULL; }
        } else { lx_free(o.data); return NULL; }
    } while (!final);
    *out_len = o.len;
    return o.data;
}

/* ---------------- PNG ---------------- */
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

uint8_t *lx_png_decode(const uint8_t *data, size_t size, int *w, int *h) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (size < 8 || memcmp(data, sig, 8) != 0) return NULL;
    size_t pos = 8;
    uint32_t width = 0, height = 0;
    int depth = 0, color = 0, interlace = 0;
    uint8_t *idat = NULL;
    size_t idat_len = 0;
    uint8_t palette[256][4];
    int pal_count = 0;
    memset(palette, 255, sizeof palette);
    while (pos + 12 <= size) {
        uint32_t len = be32(data + pos);
        const uint8_t *type = data + pos + 4, *body = data + pos + 8;
        if (pos + 12 + len > size) break;
        if (!memcmp(type, "IHDR", 4)) {
            width = be32(body); height = be32(body + 4);
            depth = body[8]; color = body[9]; interlace = body[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            pal_count = (int)(len / 3);
            for (int i = 0; i < pal_count && i < 256; i++) { palette[i][0] = body[i * 3]; palette[i][1] = body[i * 3 + 1]; palette[i][2] = body[i * 3 + 2]; }
        } else if (!memcmp(type, "tRNS", 4) && color == 3) {
            for (uint32_t i = 0; i < len && i < 256; i++) palette[i][3] = body[i];
        } else if (!memcmp(type, "IDAT", 4)) {
            idat = lx_realloc(idat, idat_len + len);
            memcpy(idat + idat_len, body, len);
            idat_len += len;
        } else if (!memcmp(type, "IEND", 4)) break;
        pos += 12 + len;
    }
    if (!idat || !width || !height || interlace || depth != 8 || width > 16384 || height > 16384) {
        if (idat && depth != 8) LX_WARN("PNG: only 8-bit, non-interlaced images are supported");
        lx_free(idat);
        return NULL;
    }
    int channels = color == 0 ? 1 : color == 2 ? 3 : color == 3 ? 1 : color == 4 ? 2 : 4;
    size_t raw_len = 0;
    uint8_t *raw = zlib_inflate(idat, idat_len, &raw_len);
    lx_free(idat);
    size_t stride = (size_t)width * (size_t)channels;
    if (!raw || raw_len < (stride + 1) * height) { lx_free(raw); return NULL; }
    uint8_t *img = lx_alloc(stride * height);
    for (uint32_t y = 0; y < height; y++) {
        int filter = raw[y * (stride + 1)];
        const uint8_t *src = raw + y * (stride + 1) + 1;
        uint8_t *dst = img + y * stride;
        const uint8_t *prev = y ? img + (y - 1) * stride : NULL;
        for (size_t x = 0; x < stride; x++) {
            int a = x >= (size_t)channels ? dst[x - (size_t)channels] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= (size_t)channels) ? prev[x - (size_t)channels] : 0;
            int v = src[x];
            switch (filter) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: v += paeth(a, b, c); break;
            default: break;
            }
            dst[x] = (uint8_t)v;
        }
    }
    lx_free(raw);
    uint8_t *rgba = lx_alloc((size_t)width * height * 4);
    for (size_t i = 0; i < (size_t)width * height; i++) {
        const uint8_t *s = img + i * (size_t)channels;
        uint8_t *d = rgba + i * 4;
        switch (color) {
        case 0: d[0] = d[1] = d[2] = s[0]; d[3] = 255; break;
        case 2: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; break;
        case 3: memcpy(d, palette[s[0]], 4); break;
        case 4: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
        default: memcpy(d, s, 4); break;
        }
    }
    lx_free(img);
    *w = (int)width;
    *h = (int)height;
    return rgba;
}

/* ---------------- TGA / BMP ---------------- */
static uint8_t *decode_tga(const uint8_t *d, size_t n, int *w, int *h) {
    if (n < 18) return NULL;
    int idlen = d[0], type = d[2], bpp = d[16];
    int width = d[12] | (d[13] << 8), height = d[14] | (d[15] << 8);
    bool top = (d[17] & 0x20) != 0;
    if ((type != 2 && type != 10) || (bpp != 24 && bpp != 32) || width <= 0 || height <= 0) return NULL;
    int bytes = bpp / 8;
    const uint8_t *p = d + 18 + idlen, *end = d + n;
    uint8_t *out = lx_alloc((size_t)width * (size_t)height * 4);
    int total = width * height, i = 0;
    while (i < total && p < end) {
        int count = 1;
        bool rle_packet = false;
        if (type == 10) { count = (*p & 0x7f) + 1; rle_packet = (*p & 0x80) != 0; p++; }
        for (int k = 0; k < count && i < total; k++, i++) {
            const uint8_t *px = p;
            if (px + bytes > end) break;
            int y = i / width, x = i % width;
            int row = top ? y : height - 1 - y;
            uint8_t *o = out + ((size_t)row * (size_t)width + (size_t)x) * 4;
            o[0] = px[2]; o[1] = px[1]; o[2] = px[0]; o[3] = bytes == 4 ? px[3] : 255;
            if (!rle_packet || type == 2) p += bytes;
        }
        if (rle_packet && type == 10) p += bytes;
    }
    *w = width; *h = height;
    return out;
}

static uint8_t *decode_bmp(const uint8_t *d, size_t n, int *w, int *h) {
    if (n < 54 || d[0] != 'B' || d[1] != 'M') return NULL;
    uint32_t off = d[10] | (d[11] << 8) | ((uint32_t)d[12] << 16) | ((uint32_t)d[13] << 24);
    int32_t width = (int32_t)(d[18] | (d[19] << 8) | (d[20] << 16) | ((uint32_t)d[21] << 24));
    int32_t height = (int32_t)(d[22] | (d[23] << 8) | (d[24] << 16) | ((uint32_t)d[25] << 24));
    int bpp = d[28] | (d[29] << 8);
    if ((bpp != 24 && bpp != 32) || width <= 0 || height == 0) return NULL;
    bool top = height < 0;
    if (top) height = -height;
    int bytes = bpp / 8;
    size_t stride = ((size_t)width * (size_t)bytes + 3) & ~(size_t)3;
    if (off + stride * (size_t)height > n) return NULL;
    uint8_t *out = lx_alloc((size_t)width * (size_t)height * 4);
    for (int y = 0; y < height; y++) {
        const uint8_t *row = d + off + stride * (size_t)(top ? y : height - 1 - y);
        for (int x = 0; x < width; x++) {
            uint8_t *o = out + ((size_t)y * (size_t)width + (size_t)x) * 4;
            o[0] = row[x * bytes + 2]; o[1] = row[x * bytes + 1]; o[2] = row[x * bytes]; o[3] = bytes == 4 ? row[x * bytes + 3] : 255;
        }
    }
    *w = width; *h = height;
    return out;
}

uint8_t *lx_image_load(const char *path, int *w, int *h) {
    size_t n = 0;
    uint8_t *data = (uint8_t *)lx_file_read(path, &n);
    if (!data) return NULL;
    uint8_t *img = lx_png_decode(data, n, w, h);
    if (!img) img = decode_bmp(data, n, w, h);
    if (!img) img = decode_tga(data, n, w, h);
    lx_free(data);
    if (!img) LX_WARN("Unsupported or corrupt image: %s", path);
    return img;
}

/* ======================================================================
 *  Encoding: PNG (uncompressed deflate - small icons only), resizing, .ico
 * ====================================================================== */
static uint32_t crc_table[256];
static void crc_init(void) {
    if (crc_table[1]) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc32_of(const uint8_t *p, size_t n) {
    crc_init();
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

uint8_t *lx_png_encode(const uint8_t *rgba, int w, int h, size_t *out_size) {
    size_t row = (size_t)w * 4 + 1, raw_size = row * (size_t)h;
    size_t blocks = (raw_size + 65534) / 65535;
    size_t zsize = 2 + raw_size + blocks * 5 + 4;
    size_t total = 8 + (12 + 13) + (12 + zsize) + 12;
    uint8_t *out = lx_alloc(total), *o = out;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    memcpy(o, sig, 8); o += 8;
    /* IHDR */
    put_be32(o, 13); memcpy(o + 4, "IHDR", 4);
    put_be32(o + 8, (uint32_t)w); put_be32(o + 12, (uint32_t)h);
    o[16] = 8; o[17] = 6; o[18] = 0; o[19] = 0; o[20] = 0;
    put_be32(o + 21, crc32_of(o + 4, 17));
    o += 25;
    /* IDAT: zlib stream of stored blocks */
    put_be32(o, (uint32_t)zsize); memcpy(o + 4, "IDAT", 4);
    uint8_t *z = o + 8, *zp = z;
    *zp++ = 0x78; *zp++ = 0x01;
    uint32_t a = 1, b = 0;
    size_t done = 0;
    uint8_t *raw = lx_alloc(raw_size);
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * row] = 0;
        memcpy(raw + (size_t)y * row + 1, rgba + (size_t)y * (size_t)w * 4, (size_t)w * 4);
    }
    while (done < raw_size) {
        size_t n = raw_size - done > 65535 ? 65535 : raw_size - done;
        *zp++ = done + n == raw_size ? 1 : 0;
        *zp++ = (uint8_t)n; *zp++ = (uint8_t)(n >> 8);
        *zp++ = (uint8_t)~n; *zp++ = (uint8_t)(~n >> 8);
        memcpy(zp, raw + done, n);
        for (size_t i = 0; i < n; i++) { a = (a + raw[done + i]) % 65521; b = (b + a) % 65521; }
        zp += n;
        done += n;
    }
    put_be32(zp, (b << 16) | a); zp += 4;
    lx_free(raw);
    put_be32(zp, crc32_of(o + 4, 4 + zsize));
    o = zp + 4;
    /* IEND */
    put_be32(o, 0); memcpy(o + 4, "IEND", 4); put_be32(o + 8, crc32_of(o + 4, 4));
    o += 12;
    *out_size = (size_t)(o - out);
    return out;
}

/* Area-averaging resize (exact box filter when shrinking, bilinear-ish when growing).
 * Colour is averaged premultiplied by alpha so transparent edges don't darken. */
uint8_t *lx_image_resize(const uint8_t *src, int sw, int sh, int dw, int dh) {
    uint8_t *dst = lx_alloc((size_t)dw * (size_t)dh * 4);
    float sx = (float)sw / (float)dw, sy = (float)sh / (float)dh;
    for (int y = 0; y < dh; y++) {
        float y0 = (float)y * sy, y1 = y0 + sy;
        for (int x = 0; x < dw; x++) {
            float x0 = (float)x * sx, x1 = x0 + sx;
            float acc[4] = {0, 0, 0, 0}, wsum = 0;
            int iy0 = (int)y0, iy1 = (int)ceilf(y1), ix0 = (int)x0, ix1 = (int)ceilf(x1);
            if (iy1 <= iy0) iy1 = iy0 + 1;
            if (ix1 <= ix0) ix1 = ix0 + 1;
            for (int yy = iy0; yy < iy1 && yy < sh; yy++) {
                float wy = lx_minf((float)yy + 1, y1) - lx_maxf((float)yy, y0);
                if (sy < 1) wy = 1;
                for (int xx = ix0; xx < ix1 && xx < sw; xx++) {
                    float wx = lx_minf((float)xx + 1, x1) - lx_maxf((float)xx, x0);
                    if (sx < 1) wx = 1;
                    const uint8_t *p = src + ((size_t)yy * (size_t)sw + (size_t)xx) * 4;
                    float wgt = wx * wy, al = (float)p[3] / 255.0f;
                    acc[0] += (float)p[0] * al * wgt; acc[1] += (float)p[1] * al * wgt; acc[2] += (float)p[2] * al * wgt;
                    acc[3] += al * wgt;
                    wsum += wgt;
                }
            }
            uint8_t *d = dst + ((size_t)y * (size_t)dw + (size_t)x) * 4;
            float al = wsum > 0 ? acc[3] / wsum : 0;
            for (int c = 0; c < 3; c++) d[c] = (uint8_t)lx_clampf(acc[3] > 0 ? acc[c] / acc[3] : 0, 0, 255);
            d[3] = (uint8_t)lx_clampf(al * 255.0f + 0.5f, 0, 255);
        }
    }
    return dst;
}

/* Square-crops (centred) and scales an image to size x size. */
uint8_t *lx_image_square(const uint8_t *src, int w, int h, int size) {
    int s = w < h ? w : h, ox = (w - s) / 2, oy = (h - s) / 2;
    uint8_t *crop = lx_alloc((size_t)s * (size_t)s * 4);
    for (int y = 0; y < s; y++) memcpy(crop + (size_t)y * (size_t)s * 4, src + ((size_t)(y + oy) * (size_t)w + (size_t)ox) * 4, (size_t)s * 4);
    uint8_t *out = lx_image_resize(crop, s, s, size, size);
    lx_free(crop);
    return out;
}

/* Windows .ico with PNG-compressed entries (Vista+) at the standard sizes. */
bool lx_ico_write(const char *path, const uint8_t *rgba, int w, int h) {
    static const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    enum { N = (int)(sizeof sizes / sizeof sizes[0]) };
    uint8_t *png[N];
    size_t png_size[N], total = 6 + 16 * N;
    for (int i = 0; i < N; i++) {
        uint8_t *px = lx_image_square(rgba, w, h, sizes[i]);
        png[i] = lx_png_encode(px, sizes[i], sizes[i], &png_size[i]);
        lx_free(px);
        total += png_size[i];
    }
    uint8_t *f = lx_alloc(total);
    memset(f, 0, 6 + 16 * N);
    f[2] = 1; f[4] = (uint8_t)N;
    size_t off = 6 + 16 * N;
    for (int i = 0; i < N; i++) {
        uint8_t *e = f + 6 + 16 * i;
        e[0] = (uint8_t)(sizes[i] >= 256 ? 0 : sizes[i]);
        e[1] = e[0];
        e[4] = 1; e[6] = 32;
        uint32_t sz = (uint32_t)png_size[i], of = (uint32_t)off;
        memcpy(e + 8, &sz, 4);   /* little-endian on every platform Luxora targets */
        memcpy(e + 12, &of, 4);
        memcpy(f + off, png[i], png_size[i]);
        off += png_size[i];
        lx_free(png[i]);
    }
    bool ok = lx_file_write(path, f, total);
    lx_free(f);
    return ok;
}
