/* FBX reader: turns a binary or ASCII .fbx file into a tree of named nodes with typed properties.
 * Binary layout (per Autodesk's file format, as documented by Blender's importer): a 27-byte header,
 * then node records [end offset, property count, property bytes, name] followed by properties and
 * nested records; version 7500+ uses 64-bit offsets. Array properties may be zlib-compressed. */
#include "../lx_internal.h"
#include "fbx.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* ---- arena: everything the tree points at lives here ---- */
typedef struct Block { struct Block *next; size_t used, cap; } Block;
struct FbxDoc {
    Block *blocks;
    FbxNode *root;
    int version;
    const uint8_t *data;
    size_t size;
    bool failed;
    char err[160];
};

static void *arena(FbxDoc *d, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!d->blocks || d->blocks->used + n > d->blocks->cap) {
        size_t cap = n > (1u << 20) ? n : (1u << 20);
        Block *b = lx_alloc(sizeof(Block) + cap);
        b->next = d->blocks;
        b->used = 0;
        b->cap = cap;
        d->blocks = b;
    }
    void *p = (uint8_t *)(d->blocks + 1) + d->blocks->used;
    d->blocks->used += n;
    return p;
}

void fbx_free(FbxDoc *d) {
    if (!d) return;
    for (Block *b = d->blocks, *n; b; b = n) { n = b->next; lx_free(b); }
    lx_free(d);
}
FbxNode *fbx_root(FbxDoc *d) { return d->root; }
int fbx_version(const FbxDoc *d) { return d->version; }

static void fail(FbxDoc *d, const char *msg) {
    if (!d->failed) lx_strcpy(d->err, sizeof d->err, msg);
    d->failed = true;
}

/* growable child list while building */
typedef struct KidList { FbxNode **v; int n, cap; } KidList;
static void kid_push(KidList *k, FbxNode *n) { LX_VEC_PUSH(k->v, k->n, k->cap, n); }
static void kids_finish(FbxDoc *d, FbxNode *n, KidList *k) {
    n->nkids = k->n;
    n->kids = k->n ? arena(d, sizeof(FbxNode *) * (size_t)k->n) : NULL;
    if (k->n) memcpy(n->kids, k->v, sizeof(FbxNode *) * (size_t)k->n);
    lx_free(k->v);
}

/* ======================================================================
 *  binary
 * ====================================================================== */
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static bool read_prop(FbxDoc *d, size_t *pos, size_t end, FbxProp *pr) {
    const uint8_t *b = d->data;
    if (*pos >= end) return false;
    char t = (char)b[(*pos)++];
    memset(pr, 0, sizeof *pr);
    size_t need = 0;
    switch (t) {
    case 'Y': need = 2; break;
    case 'C': need = 1; break;
    case 'I': case 'F': need = 4; break;
    case 'D': case 'L': need = 8; break;
    case 'S': case 'R': need = 4; break;
    case 'f': case 'd': case 'l': case 'i': case 'b': need = 12; break;
    default: fail(d, "Unknown FBX property type"); return false;
    }
    if (*pos + need > end) { fail(d, "Truncated FBX property"); return false; }
    const uint8_t *p = b + *pos;
    switch (t) {
    case 'Y': { int16_t v; memcpy(&v, p, 2); pr->type = 'I'; pr->i = v; pr->d = v; *pos += 2; return true; }
    case 'C': pr->type = 'I'; pr->i = p[0] != 0; pr->d = (double)pr->i; *pos += 1; return true;
    case 'I': { int32_t v; memcpy(&v, p, 4); pr->type = 'I'; pr->i = v; pr->d = v; *pos += 4; return true; }
    case 'L': { int64_t v; memcpy(&v, p, 8); pr->type = 'I'; pr->i = v; pr->d = (double)v; *pos += 8; return true; }
    case 'F': { float v; memcpy(&v, p, 4); pr->type = 'D'; pr->d = v; pr->i = (int64_t)v; *pos += 4; return true; }
    case 'D': { double v; memcpy(&v, p, 8); pr->type = 'D'; pr->d = v; pr->i = (int64_t)v; *pos += 8; return true; }
    case 'S': case 'R': {
        uint32_t len = rd32(p);
        *pos += 4;
        if (*pos + len > end) { fail(d, "Truncated FBX string"); return false; }
        pr->type = t;
        pr->s = (const char *)(b + *pos);
        pr->len = len;
        *pos += len;
        return true;
    }
    default: {
        uint32_t count = rd32(p), enc = rd32(p + 4), clen = rd32(p + 8);
        *pos += 12;
        size_t es = (t == 'd' || t == 'l') ? 8 : (t == 'b') ? 1 : 4;
        if (*pos + clen > end || (uint64_t)count * es > (1ull << 31)) { fail(d, "Bad FBX array"); return false; }
        size_t bytes = (size_t)count * es;
        void *out = arena(d, bytes ? bytes : 1);
        if (enc == 0) {
            if (clen < bytes) { fail(d, "Short FBX array"); return false; }
            memcpy(out, b + *pos, bytes);
        } else if (enc == 1) {
            size_t got = 0;
            uint8_t *raw = lx_zlib_inflate(b + *pos, clen, &got);
            if (!raw || got < bytes) { lx_free(raw); fail(d, "Could not decompress an FBX array"); return false; }
            memcpy(out, raw, bytes);
            lx_free(raw);
        } else { fail(d, "Unknown FBX array encoding"); return false; }
        *pos += clen;
        pr->type = 'A';
        pr->atype = t;
        pr->arr = out;
        pr->count = count;
        return true;
    }
    }
}

static FbxNode *read_node(FbxDoc *d, size_t *pos, bool wide, int depth, bool *is_null) {
    const uint8_t *b = d->data;
    size_t hdr = wide ? 25 : 13;
    *is_null = false;
    if (*pos + hdr > d->size) { fail(d, "Truncated FBX node"); return NULL; }
    uint64_t end = wide ? rd64(b + *pos) : rd32(b + *pos);
    uint64_t nprops = wide ? rd64(b + *pos + 8) : rd32(b + *pos + 4);
    uint64_t plen = wide ? rd64(b + *pos + 16) : rd32(b + *pos + 8);
    uint8_t name_len = b[*pos + hdr - 1];
    if (end == 0) { *pos += hdr; *is_null = true; return NULL; }
    if (end > d->size || end <= *pos || depth > 64) { fail(d, "Bad FBX node offset"); return NULL; }
    size_t p = *pos + hdr;
    if (p + name_len > end) { fail(d, "Bad FBX node name"); return NULL; }
    FbxNode *n = arena(d, sizeof *n);
    memset(n, 0, sizeof *n);
    char *name = arena(d, (size_t)name_len + 1);
    memcpy(name, b + p, name_len);
    name[name_len] = 0;
    n->name = name;
    p += name_len;
    size_t props_end = p + (size_t)plen;
    if (props_end > end || nprops > 1u << 24) { fail(d, "Bad FBX property list"); return NULL; }
    n->nprops = (int)nprops;
    n->props = nprops ? arena(d, sizeof(FbxProp) * (size_t)nprops) : NULL;
    for (uint64_t i = 0; i < nprops; i++)
        if (!read_prop(d, &p, props_end, &n->props[i])) return NULL;
    p = props_end;
    KidList kids = {0};
    while (p < end) {
        bool nul;
        FbxNode *c = read_node(d, &p, wide, depth + 1, &nul);
        if (d->failed) { lx_free(kids.v); return NULL; }
        if (nul) break;
        kid_push(&kids, c);
    }
    kids_finish(d, n, &kids);
    *pos = (size_t)end;
    return n;
}

static void parse_binary(FbxDoc *d) {
    d->version = (int)rd32(d->data + 23);
    bool wide = d->version >= 7500;
    size_t pos = 27;
    FbxNode *root = arena(d, sizeof *root);
    memset(root, 0, sizeof *root);
    root->name = "";
    KidList kids = {0};
    while (pos < d->size && !d->failed) {
        bool nul;
        FbxNode *n = read_node(d, &pos, wide, 0, &nul);
        if (nul || !n) break;
        kid_push(&kids, n);
    }
    kids_finish(d, root, &kids);
    d->root = root;
}

/* ======================================================================
 *  ASCII
 * ====================================================================== */
typedef struct Lex { const char *p, *end; } Lex;

static void skip_ws(Lex *l) {
    for (;;) {
        while (l->p < l->end && (isspace((unsigned char)*l->p) || *l->p == ',')) l->p++;
        if (l->p < l->end && *l->p == ';') { while (l->p < l->end && *l->p != '\n') l->p++; continue; }
        return;
    }
}

/* one value: "string", number, or a bare word (T, Y, ...) */
static bool read_value(FbxDoc *d, Lex *l, FbxProp *pr) {
    memset(pr, 0, sizeof *pr);
    if (l->p >= l->end) return false;
    if (*l->p == '"') {
        const char *s = ++l->p;
        while (l->p < l->end && *l->p != '"') l->p++;
        uint32_t len = (uint32_t)(l->p - s);
        char *c = arena(d, len + 1);
        memcpy(c, s, len);
        c[len] = 0;
        /* ASCII FBX names use "Model::Name"; binary uses "Name\x00\x01Model" - keep one form */
        pr->type = 'S';
        pr->s = c;
        pr->len = len;
        if (l->p < l->end) l->p++;
        return true;
    }
    const char *s = l->p;
    while (l->p < l->end && !isspace((unsigned char)*l->p) && *l->p != ',' && *l->p != '{' && *l->p != '}') l->p++;
    size_t len = (size_t)(l->p - s);
    char buf[64];
    if (!len || len >= sizeof buf) return false;
    memcpy(buf, s, len);
    buf[len] = 0;
    char *e;
    double v = strtod(buf, &e);
    if (*e == 0) {
        bool integral = !strpbrk(buf, ".eE");
        pr->type = integral ? 'I' : 'D';
        pr->d = v;
        pr->i = integral ? strtoll(buf, NULL, 10) : (int64_t)v;
    } else {
        char *c = arena(d, len + 1);
        memcpy(c, buf, len + 1);
        pr->type = 'S';
        pr->s = c;
        pr->len = (uint32_t)len;
    }
    return true;
}

static FbxNode *ascii_node(FbxDoc *d, Lex *l, int depth);

static void ascii_children(FbxDoc *d, Lex *l, FbxNode *n, int depth) {
    KidList kids = {0};
    for (;;) {
        skip_ws(l);
        if (l->p >= l->end || d->failed) break;
        if (*l->p == '}') { l->p++; break; }
        FbxNode *c = ascii_node(d, l, depth + 1);
        if (!c) break;
        kid_push(&kids, c);
    }
    kids_finish(d, n, &kids);
}

static FbxNode *ascii_node(FbxDoc *d, Lex *l, int depth) {
    if (depth > 64) { fail(d, "FBX nesting too deep"); return NULL; }
    skip_ws(l);
    const char *s = l->p;
    while (l->p < l->end && *l->p != ':' && !isspace((unsigned char)*l->p)) l->p++;
    if (l->p >= l->end || *l->p != ':') { if (l->p < l->end) fail(d, "Bad ASCII FBX syntax"); return NULL; }
    size_t len = (size_t)(l->p - s);
    l->p++;
    FbxNode *n = arena(d, sizeof *n);
    memset(n, 0, sizeof *n);
    char *name = arena(d, len + 1);
    memcpy(name, s, len);
    name[len] = 0;
    n->name = name;
    FbxProp *props = NULL;
    int np = 0, cap = 0;
    for (;;) {
        /* values continue on this line (or after commas) until '{' or a newline without a trailing comma */
        while (l->p < l->end && (*l->p == ' ' || *l->p == '\t' || *l->p == '\r')) l->p++;
        if (l->p >= l->end || *l->p == '\n' || *l->p == '}' || *l->p == ';') break;
        if (*l->p == ',') { l->p++; while (l->p < l->end && isspace((unsigned char)*l->p)) l->p++; continue; }
        if (*l->p == '{') break;
        if (*l->p == '*') {   /* array: *N { a: v,v,v } */
            l->p++;
            long count = strtol(l->p, (char **)&l->p, 10);
            skip_ws(l);
            if (l->p < l->end && *l->p == '{') l->p++;
            skip_ws(l);
            while (l->p < l->end && *l->p != ':') l->p++;   /* "a:" */
            if (l->p < l->end) l->p++;
            double *arr = arena(d, sizeof(double) * (size_t)(count > 0 ? count : 1));
            long got = 0;
            for (;;) {
                skip_ws(l);
                if (l->p >= l->end || *l->p == '}') break;
                char *e;
                double v = strtod(l->p, &e);
                if (e == l->p) { l->p++; continue; }
                l->p = e;
                if (got < count) arr[got] = v;
                got++;
            }
            if (l->p < l->end) l->p++;
            FbxProp pr;
            memset(&pr, 0, sizeof pr);
            pr.type = 'A';
            pr.atype = 'd';
            pr.arr = arr;
            pr.count = (uint32_t)(got < count ? got : count);
            LX_VEC_PUSH(props, np, cap, pr);
            continue;
        }
        FbxProp pr;
        if (!read_value(d, l, &pr)) break;
        LX_VEC_PUSH(props, np, cap, pr);
    }
    n->nprops = np;
    n->props = np ? arena(d, sizeof(FbxProp) * (size_t)np) : NULL;
    if (np) memcpy(n->props, props, sizeof(FbxProp) * (size_t)np);
    lx_free(props);
    while (l->p < l->end && (*l->p == ' ' || *l->p == '\t' || *l->p == '\r')) l->p++;
    if (l->p < l->end && *l->p == '{') {
        l->p++;
        ascii_children(d, l, n, depth);
    }
    return n;
}

static void parse_ascii(FbxDoc *d) {
    Lex l = {(const char *)d->data, (const char *)d->data + d->size};
    FbxNode *root = arena(d, sizeof *root);
    memset(root, 0, sizeof *root);
    root->name = "";
    KidList kids = {0};
    for (;;) {
        skip_ws(&l);
        if (l.p >= l.end || d->failed) break;
        FbxNode *n = ascii_node(d, &l, 0);
        if (!n) break;
        kid_push(&kids, n);
        if (!strcmp(n->name, "FBXHeaderExtension")) {
            FbxNode *v = fbx_child(n, "FBXVersion");
            if (v && v->nprops) d->version = (int)fbx_num(&v->props[0]);
        }
    }
    kids_finish(d, root, &kids);
    d->root = root;
}

FbxDoc *fbx_parse(const uint8_t *data, size_t size, char *err, size_t err_cap) {
    FbxDoc *d = lx_calloc(1, sizeof *d);
    d->data = data;
    d->size = size;
    if (size >= 27 && !memcmp(data, "Kaydara FBX Binary", 18)) parse_binary(d);
    else if (size > 0) parse_ascii(d);
    else fail(d, "Empty file");
    if (!d->failed && (!d->root || !d->root->nkids)) fail(d, "Not an FBX file");
    if (d->failed) {
        if (err && err_cap) lx_strcpy(err, err_cap, d->err);
        fbx_free(d);
        return NULL;
    }
    return d;
}

/* ======================================================================
 *  helpers
 * ====================================================================== */
FbxNode *fbx_child(const FbxNode *n, const char *name) {
    if (!n) return NULL;
    for (int i = 0; i < n->nkids; i++) if (!strcmp(n->kids[i]->name, name)) return n->kids[i];
    return NULL;
}
double fbx_num(const FbxProp *p) { return p->type == 'I' ? (double)p->i : p->d; }
const char *fbx_str(FbxDoc *d, const FbxProp *p) {
    if (p->type != 'S' && p->type != 'R') return "";
    char *c = arena(d, (size_t)p->len + 1);
    memcpy(c, p->s, p->len);
    c[p->len] = 0;
    return c;
}
double fbx_arr_d(const FbxProp *p, uint32_t i) {
    if (p->type != 'A' || i >= p->count) return 0;
    switch (p->atype) {
    case 'd': return ((const double *)p->arr)[i];
    case 'f': return ((const float *)p->arr)[i];
    case 'i': return ((const int32_t *)p->arr)[i];
    case 'l': return (double)((const int64_t *)p->arr)[i];
    case 'b': return ((const uint8_t *)p->arr)[i];
    default: return 0;
    }
}
int64_t fbx_arr_i(const FbxProp *p, uint32_t i) {
    if (p->type != 'A' || i >= p->count) return 0;
    switch (p->atype) {
    case 'i': return ((const int32_t *)p->arr)[i];
    case 'l': return ((const int64_t *)p->arr)[i];
    case 'b': return ((const uint8_t *)p->arr)[i];
    case 'd': return (int64_t)((const double *)p->arr)[i];
    case 'f': return (int64_t)((const float *)p->arr)[i];
    default: return 0;
    }
}
