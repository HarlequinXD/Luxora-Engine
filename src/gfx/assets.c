/* Asset registry: resolves string references to cached meshes, materials, textures and sounds. */
#include "../lx_internal.h"

typedef enum AssetKind { ASSET_MESH, ASSET_MATERIAL, ASSET_TEXTURE, ASSET_SOUND } AssetKind;
typedef struct AssetEntry {
    uint64_t hash;
    AssetKind kind;
    char ref[LX_REF_MAX];
    void *ptr;
    bool missing; /* remember failures so we don't hit the disk every frame */
} AssetEntry;

static AssetEntry *g_assets;
static int g_asset_count, g_asset_cap;
static char g_root[LX_PATH_MAX];
static LxMaterial g_default_material;

static uint64_t fnv(const char *s, AssetKind k) {
    uint64_t h = 1469598103934665603ull ^ (uint64_t)k;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
    return h;
}

void lx_assets_set_root(const char *dir) {
    lx_strcpy(g_root, sizeof g_root, dir ? dir : "");
}
const char *lx_assets_root(void) { return g_root; }

void lx_asset_path(char *out, size_t cap, const char *ref) {
    if (!ref || !ref[0]) { out[0] = 0; return; }
    if (ref[0] == '/' || (ref[0] && ref[1] == ':') || strncmp(ref, "apk:/", 5) == 0) { lx_strcpy(out, cap, ref); return; }
    lx_path_join(out, cap, g_root, ref);
}

static AssetEntry *find(const char *ref, AssetKind k) {
    uint64_t h = fnv(ref, k);
    for (int i = 0; i < g_asset_count; i++)
        if (g_assets[i].hash == h && g_assets[i].kind == k && strcmp(g_assets[i].ref, ref) == 0) return &g_assets[i];
    return NULL;
}

static AssetEntry *add(const char *ref, AssetKind k, void *ptr) {
    AssetEntry e;
    memset(&e, 0, sizeof e);
    e.hash = fnv(ref, k);
    e.kind = k;
    lx_strcpy(e.ref, sizeof e.ref, ref);
    e.ptr = ptr;
    e.missing = ptr == NULL;
    LX_VEC_PUSH(g_assets, g_asset_count, g_asset_cap, e);
    return &g_assets[g_asset_count - 1];
}

LxMesh *lx_asset_mesh(const char *ref) {
    if (!ref || !ref[0]) ref = "builtin:cube";
    AssetEntry *e = find(ref, ASSET_MESH);
    if (e) return e->ptr ? e->ptr : lx_asset_mesh("builtin:cube");
    LxMesh *m = NULL;
    if (strncmp(ref, "builtin:", 8) == 0) {
        for (int p = 0; p < LX_PRIM_COUNT; p++)
            if (strcmp(ref + 8, lx_primitive_name((LxPrimitive)p)) == 0) m = lx_mesh_build_primitive((LxPrimitive)p);
    } else {
        char path[LX_PATH_MAX];
        lx_asset_path(path, sizeof path, ref);
        m = lx_mesh_load_obj(path);
        if (m) lx_strcpy(m->ref, sizeof m->ref, ref);
        else LX_WARN("Mesh not found: %s", ref);
    }
    add(ref, ASSET_MESH, m);
    return m ? m : lx_asset_mesh("builtin:cube");
}

LxMaterial lx_material_default(void) {
    LxMaterial m;
    memset(&m, 0, sizeof m);
    lx_strcpy(m.name, sizeof m.name, "Default");
    m.albedo = lx_v4(0.8f, 0.8f, 0.8f, 1.0f);
    m.metallic = 0.0f;
    m.roughness = 0.5f;
    m.uv_scale = 1.0f;
    m.acoustic_absorption = 0.1f;
    m.acoustic_transmission = 0.02f;
    return m;
}

bool lx_material_save(const LxMaterial *m, const char *path) {
    LxStrBuf sb = {0};
    lx_sb_printf(&sb, "# Luxora material\nname = %s\n", m->name);
    lx_sb_printf(&sb, "albedo = %g %g %g %g\n", m->albedo.x, m->albedo.y, m->albedo.z, m->albedo.w);
    lx_sb_printf(&sb, "metallic = %g\nroughness = %g\n", m->metallic, m->roughness);
    lx_sb_printf(&sb, "emissive = %g %g %g\nemissive_strength = %g\n", m->emissive.x, m->emissive.y, m->emissive.z, m->emissive_strength);
    lx_sb_printf(&sb, "albedo_map = %s\nnormal_map = %s\nuv_scale = %g\n", m->albedo_map, m->normal_map, m->uv_scale);
    lx_sb_printf(&sb, "acoustic_absorption = %g\nacoustic_transmission = %g\n", m->acoustic_absorption, m->acoustic_transmission);
    bool ok = lx_file_write(path, sb.data, sb.len);
    lx_free(sb.data);
    return ok;
}

bool lx_material_load(LxMaterial *m, const char *path) {
    char *text = lx_file_read(path, NULL);
    if (!text) return false;
    *m = lx_material_default();
    const char *cur = text;
    char k[64], v[512];
    int r;
    while ((r = lx_kv_next(&cur, k, sizeof k, v, sizeof v))) {
        if (r != 1) continue;
        if (!strcmp(k, "name")) lx_strcpy(m->name, sizeof m->name, v);
        else if (!strcmp(k, "albedo")) m->albedo = lx_parse_vec4(v);
        else if (!strcmp(k, "metallic")) m->metallic = (float)atof(v);
        else if (!strcmp(k, "roughness")) m->roughness = (float)atof(v);
        else if (!strcmp(k, "emissive")) m->emissive = lx_parse_vec3(v);
        else if (!strcmp(k, "emissive_strength")) m->emissive_strength = (float)atof(v);
        else if (!strcmp(k, "albedo_map")) lx_strcpy(m->albedo_map, sizeof m->albedo_map, v);
        else if (!strcmp(k, "normal_map")) lx_strcpy(m->normal_map, sizeof m->normal_map, v);
        else if (!strcmp(k, "uv_scale")) m->uv_scale = (float)atof(v);
        else if (!strcmp(k, "acoustic_absorption")) m->acoustic_absorption = (float)atof(v);
        else if (!strcmp(k, "acoustic_transmission")) m->acoustic_transmission = (float)atof(v);
    }
    lx_free(text);
    return true;
}

static LxMaterial *builtin_material(const char *name) {
    LxMaterial *m = lx_alloc(sizeof *m);
    *m = lx_material_default();
    lx_strcpy(m->name, sizeof m->name, name);
    if (!strcmp(name, "grid")) { m->albedo = lx_v4(0.55f, 0.55f, 0.58f, 1); m->roughness = 0.8f; }
    else if (!strcmp(name, "metal")) { m->albedo = lx_v4(0.95f, 0.93f, 0.88f, 1); m->metallic = 1.0f; m->roughness = 0.25f; }
    else if (!strcmp(name, "red")) { m->albedo = lx_v4(0.8f, 0.08f, 0.06f, 1); m->roughness = 0.45f; }
    else if (!strcmp(name, "green")) { m->albedo = lx_v4(0.1f, 0.65f, 0.15f, 1); m->roughness = 0.5f; }
    else if (!strcmp(name, "blue")) { m->albedo = lx_v4(0.08f, 0.2f, 0.8f, 1); m->roughness = 0.4f; }
    else if (!strcmp(name, "emissive")) { m->albedo = lx_v4(1, 1, 1, 1); m->emissive = lx_v3(1.0f, 0.75f, 0.45f); m->emissive_strength = 8.0f; }
    else if (!strcmp(name, "concrete")) { m->albedo = lx_v4(0.6f, 0.58f, 0.55f, 1); m->roughness = 0.9f; m->acoustic_absorption = 0.02f; }
    else if (!strcmp(name, "fabric")) { m->albedo = lx_v4(0.45f, 0.3f, 0.25f, 1); m->roughness = 1.0f; m->acoustic_absorption = 0.6f; }
    return m;
}

LxMaterial *lx_asset_material(const char *ref) {
    if (!ref || !ref[0]) ref = "builtin:default";
    AssetEntry *e = find(ref, ASSET_MATERIAL);
    if (e) {
        if (e->ptr) return e->ptr;
        g_default_material = lx_material_default();
        return &g_default_material;
    }
    LxMaterial *m = NULL;
    if (strncmp(ref, "builtin:", 8) == 0) m = builtin_material(ref + 8);
    else {
        char path[LX_PATH_MAX];
        lx_asset_path(path, sizeof path, ref);
        m = lx_alloc(sizeof *m);
        if (!lx_material_load(m, path)) { LX_WARN("Material not found: %s", ref); lx_free(m); m = NULL; }
    }
    add(ref, ASSET_MATERIAL, m);
    if (m) return m;
    g_default_material = lx_material_default();
    return &g_default_material;
}

LxTexture *lx_asset_texture(const char *ref) {
    if (!ref || !ref[0]) return NULL;
    AssetEntry *e = find(ref, ASSET_TEXTURE);
    if (e) return e->ptr;
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, ref);
    LxTexture *t = NULL;
    int w, h;
    uint8_t *px = lx_image_load(path, &w, &h);
    if (px) {
        t = lx_calloc(1, sizeof *t);
        lx_strcpy(t->ref, sizeof t->ref, ref);
        t->width = w;
        t->height = h;
        t->pixels = px;
    }
    add(ref, ASSET_TEXTURE, t);
    return t;
}

LxSound *lx_asset_sound(const char *ref) {
    if (!ref || !ref[0]) return NULL;
    AssetEntry *e = find(ref, ASSET_SOUND);
    if (e) return e->ptr;
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, ref);
    LxSound *s = lx_sound_load_wav(path);
    if (s) lx_strcpy(s->ref, sizeof s->ref, ref);
    else LX_WARN("Sound not found or unsupported: %s", ref);
    add(ref, ASSET_SOUND, s);
    return s;
}

static void free_entry(AssetEntry *e) {
    if (!e->ptr) return;
    switch (e->kind) {
    case ASSET_MESH: lx_mesh_destroy(e->ptr); break;
    case ASSET_MATERIAL: lx_free(e->ptr); break;
    case ASSET_TEXTURE: {
        LxTexture *t = e->ptr;
        lx_renderer_texture_release(t);
        lx_free(t->pixels);
        lx_free(t);
        break;
    }
    case ASSET_SOUND: {
        LxSound *s = e->ptr;
        lx_audio_stop_all();
        lx_free(s->samples);
        lx_free(s);
        break;
    }
    }
    e->ptr = NULL;
}

void lx_assets_reload(const char *ref) {
    for (int i = 0; i < g_asset_count; i++) {
        if (strcmp(g_assets[i].ref, ref) == 0) {
            if (g_assets[i].kind == ASSET_MESH || g_assets[i].kind == ASSET_TEXTURE) lx_renderer_wait_idle();
            free_entry(&g_assets[i]);
            g_assets[i] = g_assets[--g_asset_count];
            i--;
        }
    }
}

void lx_assets_clear(void) {
    lx_renderer_wait_idle();
    for (int i = 0; i < g_asset_count; i++) free_entry(&g_assets[i]);
    g_asset_count = 0;
}
