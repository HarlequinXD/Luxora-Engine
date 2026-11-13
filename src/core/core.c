/* Core services: logging, memory, files, paths, time. */
#include "../lx_internal.h"
#include <stdarg.h>
#include <time.h>

#if LX_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <shlobj.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#endif
#if LX_OS_ANDROID
#include <android/log.h>
#endif

/* ---------------- logging ---------------- */
#define LX_MAX_SINKS 8
static struct { LxLogSink fn; void *user; } g_sinks[LX_MAX_SINKS];
static int g_sink_count;

void lx_log_add_sink(LxLogSink sink, void *user) {
    if (g_sink_count < LX_MAX_SINKS) {
        g_sinks[g_sink_count].fn = sink;
        g_sinks[g_sink_count].user = user;
        g_sink_count++;
    }
}

void lx_log(LxLogLevel level, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    static const char *tags[] = {"info", "warn", "error"};
#if LX_OS_ANDROID
    __android_log_print(level == LX_LOG_ERROR ? ANDROID_LOG_ERROR : (level == LX_LOG_WARN ? ANDROID_LOG_WARN : ANDROID_LOG_INFO),
                        "Luxora", "%s", buf);
#else
    fprintf(level == LX_LOG_INFO ? stdout : stderr, "[luxora:%s] %s\n", tags[level], buf);
    fflush(stdout);
#endif
#if LX_OS_WINDOWS
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
#endif
    for (int i = 0; i < g_sink_count; i++) g_sinks[i].fn(level, buf, g_sinks[i].user);
}

/* ---------------- memory ---------------- */
void *lx_alloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p) { fprintf(stderr, "Luxora: out of memory (%zu bytes)\n", size); abort(); }
    return p;
}
void *lx_calloc(size_t count, size_t size) {
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) { fprintf(stderr, "Luxora: out of memory\n"); abort(); }
    return p;
}
void *lx_realloc(void *p, size_t size) {
    void *r = realloc(p, size ? size : 1);
    if (!r) { fprintf(stderr, "Luxora: out of memory\n"); abort(); }
    return r;
}
void lx_free(void *p) { free(p); }
char *lx_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *r = lx_alloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}
void lx_strcpy(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memmove(dst, src, n);
    dst[n] = 0;
}

/* ---------------- files ---------------- */
#if LX_OS_WINDOWS
static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = lx_alloc(sizeof(wchar_t) * (size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static FILE *lx_fopen(const char *path, const char *mode) {
    wchar_t *wp = widen(path), *wm = widen(mode);
    FILE *f = _wfopen(wp, wm);
    lx_free(wp); lx_free(wm);
    return f;
}
#else
#define lx_fopen fopen
#endif

#if LX_OS_ANDROID
#include <android/asset_manager.h>
AAssetManager *g_lx_android_assets; /* set by the Android platform layer */
static char *android_asset_read(const char *path, size_t *out_size) {
    if (!g_lx_android_assets) return NULL;
    const char *p = path;
    if (strncmp(p, "apk:/", 5) == 0) p += 5;
    AAsset *a = AAssetManager_open(g_lx_android_assets, p, AASSET_MODE_BUFFER);
    if (!a) return NULL;
    size_t n = (size_t)AAsset_getLength(a);
    char *buf = lx_alloc(n + 1);
    AAsset_read(a, buf, n);
    AAsset_close(a);
    buf[n] = 0;
    if (out_size) *out_size = n;
    return buf;
}
#endif

char *lx_file_read(const char *path, size_t *out_size) {
#if LX_OS_ANDROID
    if (strncmp(path, "apk:/", 5) == 0) return android_asset_read(path, out_size);
#endif
    FILE *f = lx_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = lx_alloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    if (out_size) *out_size = got;
    return buf;
}

bool lx_file_write(const char *path, const void *data, size_t size) {
    char parent[LX_PATH_MAX];
    lx_path_parent(parent, sizeof parent, path);
    if (parent[0]) lx_mkdir_p(parent);
    FILE *f = lx_fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

bool lx_file_exists(const char *path) {
#if LX_OS_ANDROID
    if (strncmp(path, "apk:/", 5) == 0) {
        if (!g_lx_android_assets) return false;
        AAsset *a = AAssetManager_open(g_lx_android_assets, path + 5, AASSET_MODE_UNKNOWN);
        if (a) AAsset_close(a);
        return a != NULL;
    }
#endif
#if LX_OS_WINDOWS
    wchar_t *w = widen(path);
    DWORD a = GetFileAttributesW(w);
    lx_free(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool lx_dir_exists(const char *path) {
#if LX_OS_WINDOWS
    wchar_t *w = widen(path);
    DWORD a = GetFileAttributesW(w);
    lx_free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static bool mkdir_one(const char *p) {
#if LX_OS_WINDOWS
    wchar_t *w = widen(p);
    BOOL ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    lx_free(w);
    return ok;
#else
    return mkdir(p, 0755) == 0 || lx_dir_exists(p);
#endif
}

bool lx_mkdir_p(const char *path) {
    char tmp[LX_PATH_MAX];
    lx_strcpy(tmp, sizeof tmp, path);
    size_t n = strlen(tmp);
    if (!n) return false;
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            if (i == 2 && tmp[1] == ':') continue; /* C:/ */
            char c = tmp[i];
            tmp[i] = 0;
            if (tmp[0]) mkdir_one(tmp);
            tmp[i] = c;
        }
    }
    return mkdir_one(tmp);
}

bool lx_path_remove(const char *path) {
    if (lx_dir_exists(path)) {
        LxDirEntry *ents;
        int n = lx_dir_list(path, &ents);
        for (int i = 0; i < n; i++) {
            char child[LX_PATH_MAX];
            lx_path_join(child, sizeof child, path, ents[i].name);
            lx_path_remove(child);
        }
        lx_free(ents);
#if LX_OS_WINDOWS
        wchar_t *w = widen(path);
        BOOL ok = RemoveDirectoryW(w);
        lx_free(w);
        return ok;
#else
        return rmdir(path) == 0;
#endif
    }
#if LX_OS_WINDOWS
    wchar_t *w = widen(path);
    BOOL ok = DeleteFileW(w);
    lx_free(w);
    return ok;
#else
    return unlink(path) == 0;
#endif
}

bool lx_path_rename(const char *from, const char *to) {
#if LX_OS_WINDOWS
    wchar_t *a = widen(from), *b = widen(to);
    BOOL ok = MoveFileW(a, b);
    lx_free(a); lx_free(b);
    return ok;
#else
    return rename(from, to) == 0;
#endif
}

static int dir_entry_cmp(const void *a, const void *b) {
    const LxDirEntry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
#if LX_OS_WINDOWS
    return _stricmp(x->name, y->name);
#else
    return strcasecmp(x->name, y->name);
#endif
}

int lx_dir_list(const char *path, LxDirEntry **out) {
    LxDirEntry *arr = NULL;
    int count = 0, cap = 0;
#if LX_OS_WINDOWS
    char pattern[LX_PATH_MAX];
    lx_path_join(pattern, sizeof pattern, path, "*");
    wchar_t *w = widen(pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    lx_free(w);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            LxDirEntry e;
            memset(&e, 0, sizeof e);
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, e.name, sizeof e.name, NULL, NULL);
            e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            LX_VEC_PUSH(arr, count, cap, e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            LxDirEntry e;
            memset(&e, 0, sizeof e);
            lx_strcpy(e.name, sizeof e.name, de->d_name);
            char full[LX_PATH_MAX];
            lx_path_join(full, sizeof full, path, de->d_name);
            struct stat st;
            if (stat(full, &st) == 0) {
                e.is_dir = S_ISDIR(st.st_mode);
                e.size = (uint64_t)st.st_size;
            }
            LX_VEC_PUSH(arr, count, cap, e);
        }
        closedir(d);
    }
#endif
    if (count > 1) qsort(arr, (size_t)count, sizeof *arr, dir_entry_cmp);
    *out = arr ? arr : lx_alloc(1);
    return count;
}

void lx_path_join(char *out, size_t cap, const char *a, const char *b) {
    if (!a || !a[0]) { lx_strcpy(out, cap, b); return; }
    size_t n = strlen(a);
    bool sep = a[n - 1] == '/' || a[n - 1] == '\\';
    snprintf(out, cap, sep ? "%s%s" : "%s/%s", a, b);
}

const char *lx_path_filename(const char *path) {
    const char *r = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') r = p + 1;
    return r;
}

const char *lx_path_ext(const char *path) {
    const char *f = lx_path_filename(path);
    const char *dot = strrchr(f, '.');
    return dot ? dot : f + strlen(f);
}

void lx_path_parent(char *out, size_t cap, const char *path) {
    lx_strcpy(out, cap, path);
    char *f = (char *)lx_path_filename(out);
    if (f > out) f[-1] = 0;
    else out[0] = 0;
}

const char *lx_home_dir(void) {
    static char home[LX_PATH_MAX];
    if (home[0]) return home;
#if LX_OS_WINDOWS
    const char *h = getenv("USERPROFILE");
    lx_strcpy(home, sizeof home, h ? h : "C:/");
    for (char *p = home; *p; p++) if (*p == '\\') *p = '/';
#elif LX_OS_ANDROID
    lx_strcpy(home, sizeof home, "/data/local/tmp");
#else
    const char *h = getenv("HOME");
    if (!h) { struct passwd *pw = getpwuid(getuid()); h = pw ? pw->pw_dir : "/tmp"; }
    lx_strcpy(home, sizeof home, h);
#endif
    return home;
}

/* ---------------- time ---------------- */
double lx_time(void) {
#if LX_OS_WINDOWS
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

void lx_sleep_ms(int ms) {
#if LX_OS_WINDOWS
    Sleep((DWORD)ms);
#else
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

/* ---------------- input queries ---------------- */
LxInputState g_lx_input = {.dpi_scale = 1.0f};

void lx_input_begin_frame(void) {
    memcpy(g_lx_input.prev_keys, g_lx_input.keys, sizeof g_lx_input.keys);
    memcpy(g_lx_input.prev_mouse, g_lx_input.mouse, sizeof g_lx_input.mouse);
    g_lx_input.prev_mouse_pos = g_lx_input.mouse_pos;
    g_lx_input.mouse_delta_accum = lx_v2(0, 0);
    g_lx_input.wheel = 0;
    g_lx_input.text_count = 0;
}
void lx_input_push_char(uint32_t cp) {
    if (g_lx_input.text_count < LX_ARRAY_COUNT(g_lx_input.text)) g_lx_input.text[g_lx_input.text_count++] = cp;
}
bool lx_key_down(LxKey k) { return k > 0 && k < LX_KEY_COUNT && g_lx_input.keys[k]; }
bool lx_key_pressed(LxKey k) { return k > 0 && k < LX_KEY_COUNT && g_lx_input.keys[k] && !g_lx_input.prev_keys[k]; }
bool lx_key_released(LxKey k) { return k > 0 && k < LX_KEY_COUNT && !g_lx_input.keys[k] && g_lx_input.prev_keys[k]; }
bool lx_mouse_down(LxMouseButton b) { return g_lx_input.mouse[b]; }
bool lx_mouse_pressed(LxMouseButton b) { return g_lx_input.mouse[b] && !g_lx_input.prev_mouse[b]; }
bool lx_mouse_released(LxMouseButton b) { return !g_lx_input.mouse[b] && g_lx_input.prev_mouse[b]; }
LxVec2 lx_mouse_pos(void) { return g_lx_input.mouse_pos; }
LxVec2 lx_mouse_delta(void) {
    LxVec2 d = g_lx_input.mouse_delta_accum;
    if (d.x == 0 && d.y == 0)
        d = lx_v2(g_lx_input.mouse_pos.x - g_lx_input.prev_mouse_pos.x, g_lx_input.mouse_pos.y - g_lx_input.prev_mouse_pos.y);
    return d;
}
float lx_mouse_wheel(void) { return g_lx_input.wheel; }
int lx_touch_count(void) { return g_lx_input.touch_count; }
LxVec2 lx_touch_pos(int i) { return (i >= 0 && i < g_lx_input.touch_count) ? g_lx_input.touches[i] : lx_v2(0, 0); }
const uint32_t *lx_text_input(int *count) { *count = g_lx_input.text_count; return g_lx_input.text; }
void lx_window_size(int *w, int *h) { *w = g_lx_input.width; *h = g_lx_input.height; }
float lx_dpi_scale(void) { return g_lx_input.dpi_scale; }
