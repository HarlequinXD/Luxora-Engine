/* Image thumbnails for the Content Browser and the texture inspector.
 *
 * Decoding a 4K texture takes a good fraction of a second, so it never happens on the editor's
 * thread: requests go to background workers, the tile shows its icon meanwhile, and the picture
 * appears when it's ready. Every thumbnail is also saved in Library/Thumbnails, so a folder that
 * was opened before shows its pictures straight away (the file's size is part of the key, so a
 * re-exported image is redone). */
#include "editor.h"
#include <stdio.h>

#define THUMB_MAX 256
#define WORKERS 3
#define QUEUE_MAX 512

typedef struct Thumb {
    char path[LX_PATH_MAX];
    uint64_t size;
    int dim;                 /* longest side: 128 tiles, 512 inspector */
    uint32_t tex;
    float aspect;
    int src_w, src_h;        /* the full image */
    uint64_t used;
} Thumb;
static Thumb g_thumbs[THUMB_MAX];
static uint64_t g_clock, g_keep_from;

typedef struct Job {
    char path[LX_PATH_MAX];
    char cache[LX_PATH_MAX];
    uint64_t size;
    int dim;
    /* result */
    uint8_t *rgba;
    int w, h, src_w, src_h;
    bool failed;
} Job;

static LxMutex *g_lock;
static LxSemaphore *g_wake;
static LxThread *g_threads[WORKERS];
static volatile long g_quit;
static Job *g_queue[QUEUE_MAX];   /* waiting (newest first: the folder being looked at now) */
static int g_queue_count;
static Job *g_done[QUEUE_MAX];
static int g_done_count;
/* requested and not back yet (queued or being decoded): not asked for twice */
static char g_pending[QUEUE_MAX * 2][LX_PATH_MAX + 8];
static int g_pending_count;
static bool g_arrived;

static uint64_t fnv(const char *s, uint64_t h) {
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull;
    return h;
}

/* ---------------- disk cache ---------------- */
typedef struct CacheHeader { char magic[4]; uint32_t version; uint64_t size; int32_t w, h, src_w, src_h; } CacheHeader;

static bool cache_read(Job *j) {
    size_t n;
    uint8_t *d = (uint8_t *)lx_file_read(j->cache, &n);
    if (!d) return false;
    CacheHeader hd;
    bool ok = n >= sizeof hd;
    if (ok) {
        memcpy(&hd, d, sizeof hd);
        ok = !memcmp(hd.magic, "LXTH", 4) && hd.version == 1 && hd.size == j->size && hd.w > 0 && hd.h > 0 && hd.w <= 4096 && hd.h <= 4096 &&
             n >= sizeof hd + (size_t)hd.w * (size_t)hd.h * 4;
    }
    if (ok) {
        j->w = hd.w; j->h = hd.h; j->src_w = hd.src_w; j->src_h = hd.src_h;
        j->rgba = lx_alloc((size_t)hd.w * (size_t)hd.h * 4);
        memcpy(j->rgba, d + sizeof hd, (size_t)hd.w * (size_t)hd.h * 4);
    }
    lx_free(d);
    return ok;
}
static void cache_write(const Job *j) {
    size_t px = (size_t)j->w * (size_t)j->h * 4;
    uint8_t *d = lx_alloc(sizeof(CacheHeader) + px);
    CacheHeader hd = {{'L', 'X', 'T', 'H'}, 1, j->size, j->w, j->h, j->src_w, j->src_h};
    memcpy(d, &hd, sizeof hd);
    memcpy(d + sizeof hd, j->rgba, px);
    char dir[LX_PATH_MAX];
    lx_path_parent(dir, sizeof dir, j->cache);
    lx_mkdir_p(dir);
    lx_file_write(j->cache, d, sizeof hd + px);
    lx_free(d);
}

/* ---------------- workers ---------------- */
static void make(Job *j) {
    if (cache_read(j)) return;
    int w, h;
    uint8_t *px = lx_image_load(j->path, &w, &h);
    if (!px || w <= 0 || h <= 0) { lx_free(px); j->failed = true; return; }
    j->src_w = w;
    j->src_h = h;
    float k = (float)j->dim / (float)(w > h ? w : h);
    if (k < 1.0f) {
        int tw = (int)((float)w * k + 0.5f), th = (int)((float)h * k + 0.5f);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        j->rgba = lx_image_resize(px, w, h, tw, th);
        lx_free(px);
        j->w = tw;
        j->h = th;
    } else {
        j->rgba = px;
        j->w = w;
        j->h = h;
    }
    if (!j->rgba) { j->failed = true; return; }
    cache_write(j);
}

static int worker(void *arg) {
    LX_UNUSED(arg);
    for (;;) {
        lx_sem_wait(g_wake);
        if (lx_atomic_load(&g_quit)) return 0;
        lx_mutex_lock(g_lock);
        Job *j = g_queue_count ? g_queue[--g_queue_count] : NULL;
        lx_mutex_unlock(g_lock);
        if (!j) continue;
        make(j);
        lx_mutex_lock(g_lock);
        if (g_done_count < QUEUE_MAX) g_done[g_done_count++] = j;
        else { lx_free(j->rgba); lx_free(j); }
        lx_mutex_unlock(g_lock);
    }
}

static void start(void) {
    if (g_lock) return;
    g_lock = lx_mutex_create();
    g_wake = lx_sem_create(0);
    g_quit = 0;
    for (int i = 0; i < WORKERS; i++) g_threads[i] = lx_thread_create(worker, NULL);
}

static void pending_key(char *out, size_t cap, const char *path, int dim) { snprintf(out, cap, "%d|%s", dim, path); }
static bool pending_has(const char *key) {
    for (int i = 0; i < g_pending_count; i++) if (!strcmp(g_pending[i], key)) return true;
    return false;
}
static void pending_remove(const char *key) {
    for (int i = 0; i < g_pending_count; i++)
        if (!strcmp(g_pending[i], key)) { memcpy(g_pending[i], g_pending[--g_pending_count], sizeof g_pending[0]); return; }
}

static void request(const char *path, uint64_t size, int dim) {
    start();
    char key[LX_PATH_MAX + 8];
    pending_key(key, sizeof key, path, dim);
    if (pending_has(key) || g_pending_count >= QUEUE_MAX * 2) return;
    Job *j = lx_calloc(1, sizeof *j);
    lx_strcpy(j->path, sizeof j->path, path);
    j->size = size;
    j->dim = dim;
    char name[64];
    snprintf(name, sizeof name, "%016llx_%d.thumb", (unsigned long long)fnv(path, 1469598103934665603ull), dim);
    char dir[LX_PATH_MAX];
    lx_path_join(dir, sizeof dir, ED.project.root, "Library/Thumbnails");
    lx_path_join(j->cache, sizeof j->cache, dir, name);
    lx_mutex_lock(g_lock);
    if (g_queue_count == QUEUE_MAX) {   /* drop the oldest request */
        lx_free(g_queue[0]);
        memmove(g_queue, g_queue + 1, sizeof g_queue[0] * (QUEUE_MAX - 1));
        g_queue_count--;
    }
    g_queue[g_queue_count++] = j;
    lx_mutex_unlock(g_lock);
    lx_strcpy(g_pending[g_pending_count++], sizeof g_pending[0], key);
    lx_sem_post(g_wake, 1);
}

/* ---------------- cache in memory (GPU textures) ---------------- */
static Thumb *find(const char *path, uint64_t size, int dim) {
    for (int i = 0; i < THUMB_MAX; i++)
        if (g_thumbs[i].tex && g_thumbs[i].dim == dim && g_thumbs[i].size == size && !strcmp(g_thumbs[i].path, path)) return &g_thumbs[i];
    return NULL;
}
static Thumb *slot_for(const char *path, int dim) {
    for (int i = 0; i < THUMB_MAX; i++) if (g_thumbs[i].tex && g_thumbs[i].dim == dim && !strcmp(g_thumbs[i].path, path)) return &g_thumbs[i];
    for (int i = 0; i < THUMB_MAX; i++) if (!g_thumbs[i].tex) return &g_thumbs[i];
    Thumb *best = NULL;   /* least recently used, never one on screen */
    for (int i = 0; i < THUMB_MAX; i++)
        if (g_thumbs[i].used < g_keep_from && (!best || g_thumbs[i].used < best->used)) best = &g_thumbs[i];
    return best;
}

uint32_t ed_thumb_get(const char *path, uint64_t size, int dim, float *aspect, int *src_w, int *src_h) {
    Thumb *t = find(path, size, dim);
    if (t) {
        t->used = ++g_clock;
        if (aspect) *aspect = t->aspect;
        if (src_w) *src_w = t->src_w;
        if (src_h) *src_h = t->src_h;
        return t->tex;
    }
    request(path, size, dim);
    return 0;
}

/* everything shown from now on is on screen (the Content Browser rebuilt) */
void ed_thumbs_mark_visible(void) { g_keep_from = g_clock + 1; }

/* each frame: finished thumbnails go to the GPU; true when some arrived */
bool ed_thumbs_poll(void) {
    if (!g_lock) return false;
    Job *done[64];
    int n = 0;
    lx_mutex_lock(g_lock);
    while (g_done_count && n < 64) done[n++] = g_done[--g_done_count];
    lx_mutex_unlock(g_lock);
    for (int i = 0; i < n; i++) {
        Job *j = done[i];
        char key[LX_PATH_MAX + 8];
        pending_key(key, sizeof key, j->path, j->dim);
        pending_remove(key);
        if (!j->failed && j->rgba) {
            Thumb *t = slot_for(j->path, j->dim);
            if (t) {
                if (t->tex) lx_ui_texture_free(t->tex);
                t->tex = lx_ui_texture_from_rgba(j->rgba, j->w, j->h);
                lx_strcpy(t->path, sizeof t->path, j->path);
                t->size = j->size;
                t->dim = j->dim;
                t->aspect = (float)j->w / (float)j->h;
                t->src_w = j->src_w;
                t->src_h = j->src_h;
                t->used = ++g_clock;
                g_arrived = true;
            }
        }
        lx_free(j->rgba);
        lx_free(j);
    }
    bool any = g_arrived;
    g_arrived = false;
    return any;
}

/* requests nobody is waiting for any more (another folder was opened) */
void ed_thumbs_cancel_queued(void) {
    if (!g_lock) return;
    lx_mutex_lock(g_lock);
    for (int i = 0; i < g_queue_count; i++) {
        char key[LX_PATH_MAX + 8];
        pending_key(key, sizeof key, g_queue[i]->path, g_queue[i]->dim);
        pending_remove(key);
        lx_free(g_queue[i]);
    }
    g_queue_count = 0;
    lx_mutex_unlock(g_lock);
}

/* project closed or editor exiting: stop the workers and free every texture */
void ed_thumbs_shutdown(void) {
    if (g_lock) {
        ed_thumbs_cancel_queued();
        lx_atomic_store(&g_quit, 1);
        lx_sem_post(g_wake, WORKERS);
        for (int i = 0; i < WORKERS; i++) lx_thread_join(g_threads[i]);
        for (int i = 0; i < g_done_count; i++) { lx_free(g_done[i]->rgba); lx_free(g_done[i]); }
        g_done_count = 0;
        g_pending_count = 0;
        lx_sem_destroy(g_wake);
        lx_mutex_destroy(g_lock);
        g_lock = NULL;
    }
    for (int i = 0; i < THUMB_MAX; i++)
        if (g_thumbs[i].tex) { lx_ui_texture_free(g_thumbs[i].tex); g_thumbs[i].tex = 0; }
}
