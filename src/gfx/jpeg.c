/* JPEG decoding -> RGBA8.
 *
 * Baseline and progressive Huffman JPEGs (the kinds cameras, texture sites and paint programs write),
 * 8-bit, 1/3/4 components (grayscale, YCbCr, RGB, CMYK/YCCK from Adobe files), any chroma subsampling,
 * restart markers. Not supported: arithmetic coding, 12-bit, lossless (all rare; they're refused).
 *
 * Every scan is decoded into coefficient blocks first; the inverse DCT, upsampling and colour conversion
 * then run on all CPU cores. Chroma is upsampled with the same "fancy" triangle filter libjpeg uses. */
#include "../lx_internal.h"

typedef struct JHuff {
    uint8_t fast[1 << 9];        /* 9-bit lookup: index into values, 255 = longer code */
    uint16_t code[256];
    uint8_t values[256];
    uint8_t size[257];
    uint32_t maxcode[18];
    int delta[17];
} JHuff;

typedef struct JComp {
    int id, h, v, tq;
    int hd, ha;                  /* huffman tables for this scan */
    int bx, by;                  /* blocks across/down, padded to whole MCUs */
    int nbx, nby;                /* blocks that hold image data (non-interleaved scans) */
    int16_t *coef;               /* bx*by*64, natural order, not dequantised */
    uint8_t *pix;                /* bx*8 x by*8 after the IDCT */
    int dc_pred;
} JComp;

typedef struct Jpeg {
    const uint8_t *p, *end;
    uint32_t bits;
    int nbits;
    int marker;                  /* marker met while reading entropy data (0xffxx), 0 = none */
    uint16_t qt[4][64];
    JHuff hdc[4], hac[4];
    JComp comp[4];
    int ncomp, w, h, hmax, vmax, mcux, mcuy;
    bool progressive;
    int restart_interval;
    int eobrun;
    int ss, se, ah, al;          /* current scan */
    int scomp[4], nscomp;
    int adobe_transform;         /* -1 = no Adobe marker */
    bool jfif;
} Jpeg;

static const uint8_t ZZ[64 + 15] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    /* runs past the end of a block land here harmlessly */
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63};

/* ---------------- huffman ---------------- */
static bool huff_build(JHuff *h, const uint8_t counts[16]) {
    int k = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < counts[i]; j++) {
            if (k >= 256) return false;
            h->size[k++] = (uint8_t)(i + 1);
        }
    h->size[k] = 0;
    int code = 0;
    k = 0;
    for (int j = 1; j <= 16; j++) {
        h->delta[j] = k - code;
        if (h->size[k] == j) {
            while (h->size[k] == j) h->code[k++] = (uint16_t)code++;
            if (code - 1 >= (1 << j)) return false;
        }
        h->maxcode[j] = (uint32_t)code << (16 - j);
        code <<= 1;
    }
    h->maxcode[17] = 0xffffffff;
    memset(h->fast, 255, sizeof h->fast);
    for (int i = 0; i < k; i++) {
        int s = h->size[i];
        if (s <= 9) {
            int c = h->code[i] << (9 - s), m = 1 << (9 - s);
            for (int j = 0; j < m; j++) h->fast[c + j] = (uint8_t)i;
        }
    }
    return true;
}

static void fill(Jpeg *j) {
    while (j->nbits <= 24) {
        int b = 0;
        if (!j->marker && j->p < j->end) {
            b = *j->p++;
            if (b == 0xff) {
                int c = j->p < j->end ? *j->p : 0;
                while (c == 0xff && j->p < j->end) { j->p++; c = j->p < j->end ? *j->p : 0; }   /* fill bytes */
                if (c == 0) j->p++;
                else { j->marker = 0xff00 | c; j->p++; b = 0; }
            }
        }
        j->bits |= (uint32_t)b << (24 - j->nbits);
        j->nbits += 8;
    }
}

static inline int get_bits(Jpeg *j, int n) {
    if (!n) return 0;
    if (j->nbits < n) fill(j);
    int v = (int)(j->bits >> (32 - n));
    j->bits <<= n;
    j->nbits -= n;
    return v;
}
static inline int get_bit(Jpeg *j) { return get_bits(j, 1); }
/* n bits as a signed value (JPEG's "extend") */
static inline int receive(Jpeg *j, int n) {
    if (!n) return 0;
    int v = get_bits(j, n);
    return v < (1 << (n - 1)) ? v - (1 << n) + 1 : v;
}

static int decode(Jpeg *j, const JHuff *h) {
    if (j->nbits < 16) fill(j);
    int c = h->fast[j->bits >> 23];
    if (c < 255) {
        int s = h->size[c];
        j->bits <<= s;
        j->nbits -= s;
        return h->values[c];
    }
    uint32_t t = j->bits >> 16;
    int k;
    for (k = 10; k < 17; k++) if (t < h->maxcode[k]) break;
    if (k == 17) { j->nbits = 0; return -1; }
    c = (int)(j->bits >> (32 - k)) + h->delta[k];
    if (c < 0 || c > 255) return -1;
    j->bits <<= k;
    j->nbits -= k;
    return h->values[c];
}

/* ---------------- entropy decoding of one block ---------------- */
static bool block_baseline(Jpeg *j, JComp *c, int16_t *blk) {
    int t = decode(j, &j->hdc[c->hd]);
    if (t < 0 || t > 16) return false;
    c->dc_pred += receive(j, t);
    blk[0] = (int16_t)c->dc_pred;
    const JHuff *ac = &j->hac[c->ha];
    for (int k = 1; k < 64;) {
        int rs = decode(j, ac);
        if (rs < 0) return false;
        int r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r != 15) break;   /* end of block */
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) return false;
        blk[ZZ[k++]] = (int16_t)receive(j, s);
    }
    return true;
}

static bool block_dc(Jpeg *j, JComp *c, int16_t *blk) {
    if (j->ah == 0) {
        int t = decode(j, &j->hdc[c->hd]);
        if (t < 0 || t > 16) return false;
        c->dc_pred += receive(j, t);
        blk[0] = (int16_t)(c->dc_pred * (1 << j->al));
    } else if (get_bit(j)) blk[0] = (int16_t)(blk[0] | (1 << j->al));
    return true;
}

static bool block_ac(Jpeg *j, JComp *c, int16_t *blk) {
    const JHuff *ac = &j->hac[c->ha];
    if (j->ah == 0) {   /* first pass over these coefficients */
        if (j->eobrun) { j->eobrun--; return true; }
        for (int k = j->ss; k <= j->se;) {
            int rs = decode(j, ac);
            if (rs < 0) return false;
            int r = rs >> 4, s = rs & 15;
            if (!s) {
                if (r < 15) {
                    j->eobrun = (1 << r) - 1;
                    if (r) j->eobrun += get_bits(j, r);
                    break;
                }
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) return false;
            blk[ZZ[k++]] = (int16_t)(receive(j, s) * (1 << j->al));
        }
        return true;
    }
    /* refinement */
    int bit = 1 << j->al;
    int k = j->ss;
    if (j->eobrun) {
        j->eobrun--;
        for (; k <= j->se; k++) {
            int16_t *p = &blk[ZZ[k]];
            if (*p && get_bit(j) && !(*p & bit)) *p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
        }
        return true;
    }
    do {
        int rs = decode(j, ac);
        if (rs < 0) return false;
        int s = rs & 15, r = rs >> 4;
        if (!s) {
            if (r < 15) {
                j->eobrun = (1 << r) - 1;
                if (r) j->eobrun += get_bits(j, r);
                r = 64;   /* refine the rest of the block, then stop */
            }
        } else {
            if (s != 1) return false;
            s = get_bit(j) ? bit : -bit;
        }
        while (k <= j->se) {
            int16_t *p = &blk[ZZ[k++]];
            if (*p) {
                if (get_bit(j) && !(*p & bit)) *p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
            } else {
                if (r == 0) { *p = (int16_t)s; break; }
                r--;
            }
        }
    } while (k <= j->se);
    return true;
}

static bool decode_block(Jpeg *j, JComp *c, int bx, int by) {
    int16_t *blk = c->coef + ((size_t)by * (size_t)c->bx + (size_t)bx) * 64;
    if (!j->progressive) return block_baseline(j, c, blk);
    return j->ss == 0 ? block_dc(j, c, blk) : block_ac(j, c, blk);
}

/* after each restart interval: realign to the next byte and step over the RSTn marker */
static void restart(Jpeg *j) {
    j->bits = 0;
    j->nbits = 0;
    if (!j->marker) {   /* find the next marker; step over it only if it's an RSTn */
        while (j->p + 1 < j->end && !(j->p[0] == 0xff && j->p[1] != 0 && j->p[1] != 0xff)) j->p++;
        if (j->p + 1 < j->end && j->p[1] >= 0xd0 && j->p[1] <= 0xd7) j->p += 2;
        j->marker = 0;
    } else if (j->marker >= 0xffd0 && j->marker <= 0xffd7) j->marker = 0;   /* RSTn consumed */
    else return;   /* a real marker: the scan is over, leave it for the caller */
    j->eobrun = 0;
    for (int i = 0; i < j->ncomp; i++) j->comp[i].dc_pred = 0;
}

static bool decode_scan(Jpeg *j) {
    j->bits = 0;
    j->nbits = 0;
    j->marker = 0;
    j->eobrun = 0;
    for (int i = 0; i < j->ncomp; i++) j->comp[i].dc_pred = 0;
    int todo = j->restart_interval ? j->restart_interval : 0x7fffffff;
    if (j->nscomp == 1) {
        /* non-interleaved: the component's own blocks in raster order */
        JComp *c = &j->comp[j->scomp[0]];
        for (int y = 0; y < c->nby; y++)
            for (int x = 0; x < c->nbx; x++) {
                if (!decode_block(j, c, x, y)) return false;
                bool last = y == c->nby - 1 && x == c->nbx - 1;
                if (--todo <= 0 && !last) { todo = j->restart_interval; restart(j); }
            }
    } else {
        for (int my = 0; my < j->mcuy; my++)
            for (int mx = 0; mx < j->mcux; mx++) {
                for (int s = 0; s < j->nscomp; s++) {
                    JComp *c = &j->comp[j->scomp[s]];
                    for (int v = 0; v < c->v; v++)
                        for (int h = 0; h < c->h; h++)
                            if (!decode_block(j, c, mx * c->h + h, my * c->v + v)) return false;
                }
                bool last = my == j->mcuy - 1 && mx == j->mcux - 1;
                if (--todo <= 0 && !last) { todo = j->restart_interval; restart(j); }
            }
    }
    /* leave the reader at the next marker */
    if (j->marker) j->p -= 2;
    return true;
}

/* ---------------- IDCT (integer, as in stb_image / libjpeg islow) ---------------- */
#define F2F(x) ((int)((x) * 4096 + 0.5))
#define FSH(x) ((x) * 4096)
#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)                         \
    int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;              \
    p2 = s2; p3 = s6;                                                    \
    p1 = (p2 + p3) * F2F(0.5411961f);                                    \
    t2 = p1 + p3 * F2F(-1.847759065f);                                   \
    t3 = p1 + p2 * F2F(0.765366865f);                                    \
    p2 = s0; p3 = s4;                                                    \
    t0 = FSH(p2 + p3); t1 = FSH(p2 - p3);                                \
    x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2;              \
    t0 = s7; t1 = s5; t2 = s3; t3 = s1;                                  \
    p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2;              \
    p5 = (p3 + p4) * F2F(1.175875602f);                                  \
    t0 = t0 * F2F(0.298631336f); t1 = t1 * F2F(2.053119869f);            \
    t2 = t2 * F2F(3.072711026f); t3 = t3 * F2F(1.501321110f);            \
    p1 = p5 + p1 * F2F(-0.899976223f); p2 = p5 + p2 * F2F(-2.562915447f); \
    p3 = p3 * F2F(-1.961570560f); p4 = p4 * F2F(-0.390180644f);          \
    t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;

static inline uint8_t clamp8(int x) { return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); }

static void idct_block(uint8_t *out, int stride, const int16_t *in, const uint16_t *q) {
    int d[64], val[64];
    /* valid files stay well inside these limits; corrupt ones would otherwise overflow the integer maths */
    for (int i = 0; i < 64; i++) {
        int x = in[i] * q[i];
        d[i] = x < -2047 ? -2047 : x > 2047 ? 2047 : x;
    }
    int *v = val;
    const int *s = d;
    for (int i = 0; i < 8; i++, s++, v++) {
        if (!s[8] && !s[16] && !s[24] && !s[32] && !s[40] && !s[48] && !s[56]) {
            int dc = s[0] * 4;
            v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dc;
        } else {
            IDCT_1D(s[0], s[8], s[16], s[24], s[32], s[40], s[48], s[56])
            x0 += 512; x1 += 512; x2 += 512; x3 += 512;
            v[0] = (x0 + t3) >> 10; v[56] = (x0 - t3) >> 10;
            v[8] = (x1 + t2) >> 10; v[48] = (x1 - t2) >> 10;
            v[16] = (x2 + t1) >> 10; v[40] = (x2 - t1) >> 10;
            v[24] = (x3 + t0) >> 10; v[32] = (x3 - t0) >> 10;
        }
    }
    for (int i = 0; i < 64; i++) val[i] = val[i] < -12000 ? -12000 : val[i] > 12000 ? 12000 : val[i];
    v = val;
    for (int i = 0; i < 8; i++, v += 8, out += stride) {
        IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
        x0 += 65536 + (128 << 17); x1 += 65536 + (128 << 17); x2 += 65536 + (128 << 17); x3 += 65536 + (128 << 17);
        out[0] = clamp8((x0 + t3) >> 17); out[7] = clamp8((x0 - t3) >> 17);
        out[1] = clamp8((x1 + t2) >> 17); out[6] = clamp8((x1 - t2) >> 17);
        out[2] = clamp8((x2 + t1) >> 17); out[5] = clamp8((x2 - t1) >> 17);
        out[3] = clamp8((x3 + t0) >> 17); out[4] = clamp8((x3 - t0) >> 17);
    }
}

typedef struct IdctJob { Jpeg *j; JComp *c; } IdctJob;
static void idct_rows(void *user, int begin, int end, int thread) {
    LX_UNUSED(thread);
    IdctJob *ij = user;
    JComp *c = ij->c;
    int stride = c->bx * 8;
    for (int by = begin; by < end; by++)
        for (int bx = 0; bx < c->bx; bx++)
            idct_block(c->pix + (size_t)by * 8 * (size_t)stride + (size_t)bx * 8, stride, c->coef + ((size_t)by * (size_t)c->bx + (size_t)bx) * 64,
                       ij->j->qt[c->tq]);
}

/* ---------------- upsampling + colour ---------------- */
/* one full-resolution row of component c for image row y (libjpeg's triangle filter for 2x subsampling) */
static void comp_row(const Jpeg *j, const JComp *c, int y, uint8_t *out, uint8_t *tmp) {
    int hs = j->hmax / c->h, vs = j->vmax / c->v;
    int pw = c->bx * 8, ph = c->by * 8;
    int cw = (j->w * c->h + j->hmax - 1) / j->hmax;   /* this component's real width */
    if (cw > pw) cw = pw;
    int ch = (j->h * c->v + j->vmax - 1) / j->vmax;
    if (ch > ph) ch = ph;
    const uint8_t *row;
    if (vs == 2) {
        int cy = y >> 1;
        int fy = (y & 1) ? cy + 1 : cy - 1;
        if (fy < 0) fy = 0;
        if (fy >= ch) fy = ch - 1;
        if (cy >= ch) cy = ch - 1;
        const uint8_t *near = c->pix + (size_t)cy * (size_t)pw, *far = c->pix + (size_t)fy * (size_t)pw;
        if (hs == 2) {
            /* h2v2 */
            int t1 = 3 * near[0] + far[0];
            if (cw == 1) { out[0] = out[1] = (uint8_t)((t1 + 2) >> 2); return; }
            out[0] = (uint8_t)((t1 + 2) >> 2);
            for (int i = 1; i < cw; i++) {
                int t0 = t1;
                t1 = 3 * near[i] + far[i];
                out[i * 2 - 1] = (uint8_t)((3 * t0 + t1 + 8) >> 4);
                out[i * 2] = (uint8_t)((3 * t1 + t0 + 8) >> 4);
            }
            out[cw * 2 - 1] = (uint8_t)((t1 + 2) >> 2);
            return;
        }
        for (int i = 0; i < cw; i++) tmp[i] = (uint8_t)((3 * near[i] + far[i] + 2) >> 2);
        row = tmp;
    } else {
        int cy = y / vs;
        if (cy >= ch) cy = ch - 1;
        row = c->pix + (size_t)cy * (size_t)pw;
    }
    if (hs == 1) { memcpy(out, row, (size_t)cw); return; }
    if (hs == 2) {
        if (cw == 1) { out[0] = out[1] = row[0]; return; }
        out[0] = row[0];
        out[1] = (uint8_t)((row[0] * 3 + row[1] + 2) >> 2);
        int i;
        for (i = 1; i < cw - 1; i++) {
            int n = 3 * row[i] + 2;
            out[i * 2] = (uint8_t)((n + row[i - 1]) >> 2);
            out[i * 2 + 1] = (uint8_t)((n + row[i + 1]) >> 2);
        }
        out[i * 2] = (uint8_t)((row[cw - 2] * 3 + row[cw - 1] + 2) >> 2);
        out[i * 2 + 1] = row[cw - 1];
        return;
    }
    for (int x = 0; x < cw * hs; x++) out[x] = row[x / hs];   /* other factors: nearest */
}

typedef struct ColorJob { Jpeg *j; uint8_t *rgba; int mode; } ColorJob;   /* mode: 0 gray, 1 ycc, 2 rgb, 3 cmyk, 4 ycck */
#define FIX(x) (((int)((x) * 4096.0f + 0.5f)) << 8)
static inline uint8_t blinn(int x, int y) { int t = x * y + 128; return (uint8_t)((t + (t >> 8)) >> 8); }

static void color_rows(void *user, int begin, int end, int thread) {
    LX_UNUSED(thread);
    ColorJob *cj = user;
    Jpeg *j = cj->j;
    int w = j->w, wide = j->mcux * j->hmax * 8 + 16;
    uint8_t *buf = lx_alloc((size_t)wide * 5);
    uint8_t *row[4] = {buf, buf + wide, buf + wide * 2, buf + wide * 3}, *tmp = buf + wide * 4;
    for (int y = begin; y < end; y++) {
        for (int c = 0; c < j->ncomp; c++) comp_row(j, &j->comp[c], y, row[c], tmp);
        uint8_t *o = cj->rgba + (size_t)y * (size_t)w * 4;
        switch (cj->mode) {
        case 0:
            for (int x = 0; x < w; x++, o += 4) o[0] = o[1] = o[2] = row[0][x], o[3] = 255;
            break;
        case 2:
            for (int x = 0; x < w; x++, o += 4) { o[0] = row[0][x]; o[1] = row[1][x]; o[2] = row[2][x]; o[3] = 255; }
            break;
        case 3:   /* Adobe CMYK is stored inverted */
            for (int x = 0; x < w; x++, o += 4) {
                int k = row[3][x];
                o[0] = blinn(row[0][x], k); o[1] = blinn(row[1][x], k); o[2] = blinn(row[2][x], k); o[3] = 255;
            }
            break;
        default:
            for (int x = 0; x < w; x++, o += 4) {
                int yf = (row[0][x] << 20) + (1 << 19), cr = row[2][x] - 128, cb = row[1][x] - 128;
                int r = yf + cr * FIX(1.40200f);
                int g = yf + cr * -FIX(0.71414f) + ((cb * -FIX(0.34414f)) & (int)0xffff0000);
                int b = yf + cb * FIX(1.77200f);
                r >>= 20; g >>= 20; b >>= 20;
                if (cj->mode == 4) {   /* YCCK: the YCbCr part is inverted CMY */
                    int k = row[3][x];
                    o[0] = blinn(255 - clamp8(r), k); o[1] = blinn(255 - clamp8(g), k); o[2] = blinn(255 - clamp8(b), k);
                } else { o[0] = clamp8(r); o[1] = clamp8(g); o[2] = clamp8(b); }
                o[3] = 255;
            }
            break;
        }
    }
    lx_free(buf);
}

/* ---------------- markers ---------------- */
static int be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

static bool read_sof(Jpeg *j, const uint8_t *p, int len) {
    if (len < 8 || p[0] != 8) return false;   /* 8-bit only */
    j->h = be16(p + 1);
    j->w = be16(p + 3);
    j->ncomp = p[5];
    if (!j->w || !j->h || j->w > 16384 || j->h > 16384) return false;
    if (j->ncomp != 1 && j->ncomp != 3 && j->ncomp != 4) return false;
    if (len < 6 + j->ncomp * 3) return false;
    j->hmax = j->vmax = 1;
    for (int i = 0; i < j->ncomp; i++) {
        JComp *c = &j->comp[i];
        c->id = p[6 + i * 3];
        c->h = p[7 + i * 3] >> 4;
        c->v = p[7 + i * 3] & 15;
        c->tq = p[8 + i * 3] & 3;
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4) return false;
        if (c->h > j->hmax) j->hmax = c->h;
        if (c->v > j->vmax) j->vmax = c->v;
    }
    if (j->ncomp == 1) { j->comp[0].h = j->comp[0].v = 1; j->hmax = j->vmax = 1; }   /* a lone component is never subsampled */
    j->mcux = (j->w + j->hmax * 8 - 1) / (j->hmax * 8);
    j->mcuy = (j->h + j->vmax * 8 - 1) / (j->vmax * 8);
    for (int i = 0; i < j->ncomp; i++) {
        JComp *c = &j->comp[i];
        if (j->hmax % c->h || j->vmax % c->v) return false;
        c->bx = j->mcux * c->h;
        c->by = j->mcuy * c->v;
        c->nbx = ((j->w * c->h + j->hmax - 1) / j->hmax + 7) / 8;
        c->nby = ((j->h * c->v + j->vmax - 1) / j->vmax + 7) / 8;
        c->coef = lx_calloc((size_t)c->bx * (size_t)c->by * 64, sizeof(int16_t));
    }
    return true;
}

static bool read_dht(Jpeg *j, const uint8_t *p, int len) {
    while (len > 17) {
        int tc = p[0] >> 4, th = p[0] & 15;
        if (tc > 1 || th > 3) return false;
        uint8_t counts[16];
        int n = 0;
        for (int i = 0; i < 16; i++) { counts[i] = p[1 + i]; n += counts[i]; }
        if (n > 256 || len < 17 + n) return false;
        JHuff *h = tc ? &j->hac[th] : &j->hdc[th];
        if (!huff_build(h, counts)) return false;
        memcpy(h->values, p + 17, (size_t)n);
        p += 17 + n;
        len -= 17 + n;
    }
    return true;
}

static bool read_dqt(Jpeg *j, const uint8_t *p, int len) {
    while (len > 0) {
        int pq = p[0] >> 4, tq = p[0] & 15;
        if (tq > 3) return false;
        int need = 1 + 64 * (pq ? 2 : 1);
        if (len < need) return false;
        for (int i = 0; i < 64; i++) j->qt[tq][ZZ[i]] = (uint16_t)(pq ? be16(p + 1 + i * 2) : p[1 + i]);
        p += need;
        len -= need;
    }
    return true;
}

static bool read_sos(Jpeg *j, const uint8_t *p, int len) {
    int n = p[0];
    if (n < 1 || n > 4 || len < 4 + n * 2) return false;
    j->nscomp = n;
    for (int i = 0; i < n; i++) {
        int id = p[1 + i * 2], tables = p[2 + i * 2], k;
        for (k = 0; k < j->ncomp; k++) if (j->comp[k].id == id) break;
        if (k == j->ncomp) return false;
        j->scomp[i] = k;
        j->comp[k].hd = (tables >> 4) & 3;
        j->comp[k].ha = tables & 3;
    }
    const uint8_t *q = p + 1 + n * 2;
    j->ss = q[0];
    j->se = q[1];
    j->ah = q[2] >> 4;
    j->al = q[2] & 15;
    if (j->progressive) {
        if (j->ss > 63 || j->se > 63 || j->ss > j->se || j->al > 13) return false;
        if (j->ss && n != 1) return false;
    } else j->ss = 0, j->se = 63, j->ah = j->al = 0;
    return true;
}

uint8_t *lx_jpeg_decode(const uint8_t *data, size_t size, int *out_w, int *out_h) {
    if (size < 4 || data[0] != 0xff || data[1] != 0xd8) return NULL;
    Jpeg *j = lx_calloc(1, sizeof *j);
    j->p = data + 2;
    j->end = data + size;
    j->adobe_transform = -1;
    bool ok = true, frame = false, any_scan = false;
    const char *why = NULL;
    while (ok && j->p + 4 <= j->end) {
        if (j->p[0] != 0xff) { j->p++; continue; }   /* junk between segments */
        int m = j->p[1];
        j->p += 2;
        if (m == 0xff || m == 0x01 || (m >= 0xd0 && m <= 0xd7)) { if (m == 0xff) j->p--; continue; }
        if (m == 0xd9) break;   /* EOI */
        int len = be16(j->p);
        if (len < 2 || j->p + len > j->end) { ok = false; why = "truncated"; break; }
        const uint8_t *body = j->p + 2;
        int blen = len - 2;
        j->p += len;
        switch (m) {
        case 0xc0: case 0xc1: case 0xc2:
            if (frame) { ok = false; break; }
            j->progressive = m == 0xc2;
            ok = frame = read_sof(j, body, blen);
            if (!ok) why = "unsupported frame (only 8-bit baseline/progressive)";
            break;
        case 0xc3: case 0xc5: case 0xc6: case 0xc7: case 0xc9: case 0xca: case 0xcb: case 0xcd: case 0xce: case 0xcf:
            ok = false; why = "arithmetic-coded or lossless JPEG"; break;
        case 0xc4: ok = read_dht(j, body, blen); break;
        case 0xdb: ok = read_dqt(j, body, blen); break;
        case 0xdd: if (blen >= 2) j->restart_interval = be16(body); break;
        case 0xee: if (blen >= 12 && !memcmp(body, "Adobe", 5)) j->adobe_transform = body[11]; break;
        case 0xe0: if (blen >= 5 && !memcmp(body, "JFIF", 4)) j->jfif = true; break;
        case 0xda:
            if (!frame || !read_sos(j, body, blen)) { ok = false; break; }
            ok = decode_scan(j);
            any_scan = true;
            /* progressive files may end a scan with junk: carry on from the next marker */
            break;
        default: break;   /* APPn, COM, ... */
        }
    }
    uint8_t *rgba = NULL;
    if (ok && frame && any_scan) {
        for (int i = 0; i < j->ncomp; i++) {
            JComp *c = &j->comp[i];
            c->pix = lx_alloc((size_t)c->bx * 8 * (size_t)c->by * 8);
            IdctJob ij = {j, c};
            lx_parallel_for(c->by, 4, idct_rows, &ij);
        }
        int mode = 1;
        if (j->ncomp == 1) mode = 0;
        else if (j->ncomp == 3) mode = (j->adobe_transform == 0 || (j->comp[0].id == 'R' && j->comp[1].id == 'G' && j->comp[2].id == 'B')) ? 2 : 1;
        else mode = j->adobe_transform == 2 ? 4 : 3;
        rgba = lx_alloc((size_t)j->w * (size_t)j->h * 4);
        ColorJob cj = {j, rgba, mode};
        lx_parallel_for(j->h, 32, color_rows, &cj);
        *out_w = j->w;
        *out_h = j->h;
    } else if (why) LX_WARN("JPEG: %s", why);
    for (int i = 0; i < 4; i++) { lx_free(j->comp[i].coef); lx_free(j->comp[i].pix); }
    lx_free(j);
    return rgba;
}
