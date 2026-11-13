/* Internal engine declarations shared across modules (not part of the public API). */
#ifndef LX_INTERNAL_H
#define LX_INTERNAL_H

#include "luxora/luxora.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#define LX_OS_WINDOWS 1
#elif defined(__ANDROID__)
#define LX_OS_ANDROID 1
#else
#define LX_OS_LINUX 1
#endif

#define LX_ARRAY_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define LX_UNUSED(x) (void)(x)

/* ---------- dynamic array helpers ---------- */
#define LX_VEC_PUSH(arr, count, cap, value)                                   \
    do {                                                                      \
        if ((count) >= (cap)) {                                               \
            (cap) = (cap) ? (cap) * 2 : 16;                                   \
            (arr) = lx_realloc((arr), (size_t)(cap) * sizeof(*(arr)));        \
        }                                                                     \
        (arr)[(count)++] = (value);                                           \
    } while (0)

/* ---------- atomics ---------- */
#if defined(_MSC_VER)
#include <intrin.h>
static __inline long lx_atomic_add(volatile long *p, long v) { return _InterlockedExchangeAdd(p, v) + v; }
static __inline long lx_atomic_load(volatile long *p) { return _InterlockedOr(p, 0); }
static __inline void lx_atomic_store(volatile long *p, long v) { _InterlockedExchange(p, v); }
#else
static inline long lx_atomic_add(volatile long *p, long v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
static inline long lx_atomic_load(volatile long *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void lx_atomic_store(volatile long *p, long v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
#endif

/* ---------- threads (platform) ---------- */
typedef struct LxThread LxThread;
typedef struct LxMutex LxMutex;
typedef struct LxSemaphore LxSemaphore;
LxThread *lx_thread_create(int (*fn)(void *), void *arg);
void lx_thread_join(LxThread *t);
LxMutex *lx_mutex_create(void);
void lx_mutex_destroy(LxMutex *m);
void lx_mutex_lock(LxMutex *m);
void lx_mutex_unlock(LxMutex *m);
LxSemaphore *lx_sem_create(int initial);
void lx_sem_destroy(LxSemaphore *s);
void lx_sem_post(LxSemaphore *s, int count);
void lx_sem_wait(LxSemaphore *s);
int lx_cpu_count(void);

/* ---------- platform window ---------- */
typedef struct LxWindowDesc {
    const char *title;
    int width, height;
    bool vsync;
    bool maximized;
    bool fullscreen;
} LxWindowDesc;

bool lx_platform_init(const LxWindowDesc *desc);
void lx_platform_shutdown(void);
bool lx_platform_pump(void);       /* processes events, false when quit requested */
/* Vulkan surface support: instance extensions the window system needs, and surface creation.
 * `instance` is a VkInstance, `out_surface` a VkSurfaceKHR*; typed as void* to keep vulkan.h out of here. */
int lx_platform_vk_extensions(const char **out_names, int max);
bool lx_platform_vk_create_surface(void *instance, void *out_surface);
void *lx_platform_vk_loader(void);  /* vkGetInstanceProcAddr */
uint32_t lx_platform_surface_generation(void); /* changes when the native window is recreated (Android) */
void lx_platform_set_title(const char *title);
void lx_platform_set_cursor(int cursor); /* 0 arrow, 1 text, 2 resize-h, 3 resize-v, 4 hand */
const char *lx_platform_clipboard_get(void);
void lx_platform_clipboard_set(const char *text);
/* Runs a shell command, streaming each output line to cb. Returns exit code. */
int lx_platform_run_process(const char *cmdline, const char *workdir, void (*cb)(const char *line, void *u), void *u);
void lx_platform_open_folder(const char *path);

/* input state written by the platform layer */
typedef struct LxInputState {
    bool keys[LX_KEY_COUNT], prev_keys[LX_KEY_COUNT];
    bool mouse[LX_MOUSE_COUNT], prev_mouse[LX_MOUSE_COUNT];
    LxVec2 mouse_pos, prev_mouse_pos, mouse_delta_accum;
    float wheel;
    uint32_t text[64];
    int text_count;
    int touch_count;
    LxVec2 touches[10];
    int width, height;
    float dpi_scale;
} LxInputState;
extern LxInputState g_lx_input;

/* gamepad state written by platform layers (Android) or polled by the input module */
#define LX_MAX_PADS 4
typedef struct LxPadState {
    bool connected;
    char name[64];
    bool buttons[LX_PAD_BUTTON_COUNT], prev_buttons[LX_PAD_BUTTON_COUNT];
    float axes[LX_PAD_AXIS_COUNT];
} LxPadState;
extern LxPadState g_lx_pads[LX_MAX_PADS];
extern bool g_lx_pad_polling_disabled; /* tests inject pad state directly */
void lx_input_begin_frame(void); /* copy current->prev, clear per-frame data */
void lx_input_push_char(uint32_t cp);

/* ---------- renderer frame API (Vulkan) ----------
 * Each frame: begin_frame -> any number of lx_render_scene -> optional UI -> end_frame. */
bool lx_renderer_begin_frame(void);  /* false = window minimised, skip frame */
void lx_renderer_end_frame(void);
void lx_renderer_wait_idle(void);
void lx_renderer_set_vsync(bool on);

typedef struct LxUiVertex { float x, y, u, v; uint32_t color; } LxUiVertex;
typedef struct LxUiCmd {
    uint32_t texture;           /* 0 = font atlas, else id from lx_ui_texture_* */
    uint32_t index_offset, index_count;
    float clip[4];              /* x, y, w, h in pixels */
} LxUiCmd;
typedef struct LxUiDrawList {
    const LxUiVertex *vertices; int vertex_count;
    const uint32_t *indices; int index_count;
    const LxUiCmd *cmds; int cmd_count;
    LxVec4 clear_color;
} LxUiDrawList;
void lx_renderer_set_font_atlas(const uint8_t *alpha, int w, int h);
uint32_t lx_ui_texture_from_rgba(const uint8_t *rgba, int w, int h); /* thumbnails/icons */
void lx_ui_texture_free(uint32_t id);
uint32_t lx_ui_white_texture(void);              /* 1x1 white, for solid shapes */
void lx_renderer_draw_ui(const LxUiDrawList *list); /* draws onto the swapchain image */
/* Reads back the last presented swapchain image as RGBA8 (for screenshots/tests). */
bool lx_renderer_screenshot(const char *ppm_path);
int lx_renderer_validation_errors(void);

/* ---------- mesh internals ---------- */
typedef struct LxVertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float tx, ty, tz, tw;
} LxVertex;

#define LX_MAX_LODS 4
typedef struct LxMeshLod {
    LxVertex *vertices;
    uint32_t *indices;
    int vertex_count, index_count;
    float screen_size;  /* use this LOD while projected size >= screen_size */
} LxMeshLod;

struct LxMesh {
    char ref[LX_REF_MAX];
    LxMeshLod lods[LX_MAX_LODS]; /* lods[0] = full detail */
    int lod_count;
    LxAabb bounds;
    void *gpu;                  /* renderer-owned buffers */
};
#define LX_MESH_VERTS(m) ((m)->lods[0].vertices)
#define LX_MESH_IDX(m) ((m)->lods[0].indices)
#define LX_MESH_VCOUNT(m) ((m)->lods[0].vertex_count)
#define LX_MESH_ICOUNT(m) ((m)->lods[0].index_count)

LxMesh *lx_mesh_create(const LxVertex *v, int vc, const uint32_t *idx, int ic, const char *ref);
LxMesh *lx_mesh_build_primitive(LxPrimitive p);
LxMesh *lx_mesh_load_obj(const char *path);
void lx_mesh_destroy(LxMesh *m);
void lx_renderer_mesh_release(LxMesh *m);
void lx_mesh_generate_lods(LxMesh *m);

struct LxTexture {
    char ref[LX_REF_MAX];
    int width, height;
    uint8_t *pixels; /* RGBA8 */
    void *gpu;
};
uint8_t *lx_image_load(const char *path, int *w, int *h); /* RGBA8; PNG, TGA, BMP */
uint8_t *lx_png_decode(const uint8_t *data, size_t size, int *w, int *h);
void lx_renderer_texture_release(LxTexture *t);

struct LxSound {
    char ref[LX_REF_MAX];
    float *samples; /* mono float */
    int frame_count;
    int sample_rate;
};
LxSound *lx_sound_load_wav(const char *path);

/* ---------- scene internals ---------- */
typedef struct LxBehaviourInstance {
    void *state;
    bool started;
    const LxBehaviourDesc *desc;
} LxBehaviourInstance;

typedef struct LxEntityData {
    uint32_t generation;
    bool alive, active;
    char name[LX_NAME_MAX];
    LxEntity parent, first_child, next_sibling, prev_sibling;
    LxVec3 position, euler, scale;
    LxQuat rotation;
    LxMat4 world;
    uint32_t comps;
    LxCamera camera;
    LxLight light;
    LxMeshRenderer mesh;
    LxAudioSource audio;
    LxBehaviourRef behaviour;
    LxCrowdComponent crowd;
    LxBehaviourInstance binst;
    LxCrowd *crowd_sim;
    int audio_voice;  /* runtime voice index or -1 */
    uint32_t order;   /* creation order key */
} LxEntityData;

struct LxScene {
    char name[LX_NAME_MAX];
    LxEntityData *entities; /* slot 0 reserved */
    int capacity, count;    /* count = slots used incl. 0 */
    uint32_t *free_slots;
    int free_count, free_cap;
    uint32_t next_order;
    LxEnvironment env;
    uint32_t static_version;
    bool simulating;
    LxEntity first_root;      /* roots form a sibling list like children */
    LxEntity *order_cache;    /* alive entities in creation order */
    int order_count, order_cap;
    bool order_dirty;
};

LxEntityData *lx_entity_data(const LxScene *s, LxEntity e);
LxEntity lx_entity_make(uint32_t index, uint32_t gen);

/* ---------- geometry acceleration (shared by GI + acoustics) ---------- */
typedef struct LxBvhNode {
    LxAabb bounds;
    int left_or_first; /* inner: left child index, leaf: first tri */
    int count;         /* 0 = inner */
} LxBvhNode;

typedef struct LxTriMaterial {
    LxVec3 albedo;
    LxVec3 emissive;
    float absorption, transmission;
} LxTriMaterial;

typedef struct LxBvh {
    LxVec3 *v0, *e1, *e2; /* triangle as v0 + edges */
    LxVec3 *normals;
    uint16_t *mat_index;
    int *tri_index;
    int tri_count;
    LxBvhNode *nodes;
    int node_count;
    LxTriMaterial *materials;
    int material_count;
    LxAabb bounds;
} LxBvh;

typedef struct LxHit {
    float t;
    int tri;
    float u, v;
} LxHit;

void lx_bvh_build_from_scene(LxBvh *bvh, LxScene *s);
void lx_bvh_free(LxBvh *bvh);
bool lx_bvh_intersect(const LxBvh *bvh, LxRay ray, float tmax, LxHit *hit);
bool lx_bvh_occluded(const LxBvh *bvh, LxRay ray, float tmax);

/* ---------- GI ---------- */
typedef struct LxGiVolume {
    LxVec3 origin, cell_size;
    int res[3];
    const float *sh;    /* res.x*res.y*res.z cells x 12 floats (L1 SH: 4 coeffs x RGB, channel-major) */
    uint32_t version;   /* bumps when data changes */
    bool valid;
} LxGiVolume;
void lx_gi_init(void);
void lx_gi_shutdown(void);
void lx_gi_update(LxScene *s, const LxGraphicsSettings *g);
const LxGiVolume *lx_gi_volume(void);
int lx_gi_last_photon_count(void);
float lx_gi_last_ms(void);
/* CPU access for tests: SH coefficients (12 floats) of a cell */
const float *lx_gi_cell(int x, int y, int z);

/* ---------- builtin behaviours / crowd rendering ---------- */
int lx_crowd_positions(const LxCrowd *c, const LxVec3 **pos, const LxVec3 **vel);

/* ---------- text key/value files (.lxscene, .lxmat, .lxproj) ----------
 * Format: "[section]" lines and "key = value" lines; '#' starts a comment. */
typedef struct LxStrBuf { char *data; size_t len, cap; } LxStrBuf;
void lx_sb_printf(LxStrBuf *sb, const char *fmt, ...);
/* Returns 0 at end, 1 for key/value (key,val filled), 2 for a [section] (key = section name). */
int lx_kv_next(const char **cursor, char *key, size_t kcap, char *val, size_t vcap);
LxVec3 lx_parse_vec3(const char *s);
LxVec4 lx_parse_vec4(const char *s);
bool lx_parse_bool(const char *s);

/* ---------- engine state ---------- */
void lx_engine_set_project(const LxProject *p);

#endif
