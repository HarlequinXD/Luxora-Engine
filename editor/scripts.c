/* Game scripts in the editor: Assets/Scripts is compiled with the built-in compiler whenever a
 * script changes (and on Play), and the result is loaded so script behaviours appear in the
 * Inspector and run in Play mode. Builds ship the same compiled module (Data/scripts.lxo). */
#include "editor.h"
#ifndef LUXORA_SCRIPT_COMPILER
#define LUXORA_SCRIPT_COMPILER 0
#endif
#if LUXORA_SCRIPT_COMPILER
#include "compiler.h"
#endif
#include <stdio.h>
#include <string.h>

typedef struct ScriptFiles { char (*paths)[LX_PATH_MAX]; int count, cap; } ScriptFiles;

static void collect(const char *dir, ScriptFiles *f) {
    LxDirEntry *e = NULL;
    int n = lx_dir_list(dir, &e);
    for (int i = 0; i < n; i++) {
        char full[LX_PATH_MAX];
        lx_path_join(full, sizeof full, dir, e[i].name);
        if (e[i].is_dir) { collect(full, f); continue; }
        size_t len = strlen(e[i].name);
        if (len < 3 || strcmp(e[i].name + len - 2, ".c")) continue;
        if (f->count == f->cap) {
            f->cap = f->cap ? f->cap * 2 : 16;
            f->paths = lx_realloc(f->paths, sizeof *f->paths * (size_t)f->cap);
        }
        lx_strcpy(f->paths[f->count++], LX_PATH_MAX, full);
    }
    lx_free(e);
}

/* Changes when any script (or header next to one) changes. */
static uint64_t hash_dir(const char *dir, uint64_t h) {
    LxDirEntry *e = NULL;
    int n = lx_dir_list(dir, &e);
    for (int i = 0; i < n; i++) {
        char full[LX_PATH_MAX];
        lx_path_join(full, sizeof full, dir, e[i].name);
        if (e[i].is_dir) { h = hash_dir(full, h); continue; }
        size_t len = strlen(e[i].name);
        bool src = (len > 2 && !strcmp(e[i].name + len - 2, ".c")) || (len > 2 && !strcmp(e[i].name + len - 2, ".h"));
        if (!src) continue;
        for (const char *p = full; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
        size_t size;
        char *text = lx_file_read(full, &size);
        for (size_t k = 0; text && k < size; k++) h = (h ^ (uint8_t)text[k]) * 1099511628211ull;
        lx_free(text);
    }
    lx_free(e);
    return h;
}

static uint64_t g_hash;          /* sources last compiled */
static bool g_have_hash;
static double g_next_check;
static int g_errors;

int ed_script_errors(void) { return g_errors; }
bool ed_scripts_supported(void) { return LUXORA_SCRIPT_COMPILER != 0; }

void ed_script_sdk_dir(char *out, size_t cap) { lx_path_join(out, cap, ED.project.root, "Library/ScriptSDK"); }

#if LUXORA_SCRIPT_COMPILER
static void diag(void *user, bool warning, const char *msg) {
    LX_UNUSED(user);
    if (warning) LX_WARN("%s", msg);
    else LX_ERROR("%s", msg);
}
#endif

/* Compiles every script under `assets` into out_path (thread-safe: the build uses it too).
 * *had_scripts says whether there was anything to compile; nothing to compile is a success. */
bool ed_scripts_compile_dir(const char *assets, const char *sdk, const char *out_path, bool *had_scripts) {
    ScriptFiles f = {0};
    collect(assets, &f);
    if (had_scripts) *had_scripts = f.count > 0;
    if (!f.count) { lx_free(f.paths); return true; }
#if LUXORA_SCRIPT_COMPILER
    const char **list = lx_alloc(sizeof(char *) * (size_t)f.count);
    for (int i = 0; i < f.count; i++) list[i] = f.paths[i];
    LxScriptCompileStats st;
    double t0 = lx_time();
    bool ok = lx_script_compile(list, f.count, sdk, out_path, diag, NULL, &st);
    if (ok) LX_INFO("Compiled %d script%s in %.0f ms (%d behaviour registration%s)", st.files, st.files == 1 ? "" : "s",
                    (lx_time() - t0) * 1000.0, st.registrations, st.registrations == 1 ? "" : "s");
    else LX_ERROR("Script compile failed: %d error%s", st.errors ? st.errors : 1, st.errors == 1 ? "" : "s");
    lx_free(list);
    lx_free(f.paths);
    return ok;
#else
    LX_UNUSED(sdk); LX_UNUSED(out_path);
    LX_ERROR("This editor build has no script compiler");
    lx_free(f.paths);
    return false;
#endif
}

bool ed_scripts_compile(const char *out_path, bool *had_scripts) {
    if (!ED.has_project) { if (had_scripts) *had_scripts = false; return true; }
    char sdk[LX_PATH_MAX];
    ed_script_sdk_dir(sdk, sizeof sdk);
    return ed_scripts_compile_dir(ED.project.assets, sdk, out_path, had_scripts);
}

/* Recompiles and reloads if any script changed. Only in edit mode: running behaviours keep their code. */
bool ed_scripts_refresh(bool force) {
    if (!ED.has_project || ED.play != ED_EDIT) return g_errors == 0;
    uint64_t h = hash_dir(ED.project.assets, 1469598103934665603ull);
    if (!force && g_have_hash && h == g_hash) return g_errors == 0;
    g_hash = h;
    g_have_hash = true;
    char out[LX_PATH_MAX];
    lx_path_join(out, sizeof out, ED.project.root, "Library/ScriptCache/scripts.lxo");
    bool had = false;
    lx_path_remove(out);
    if (!ed_scripts_compile(out, &had)) {
        g_errors = 1;
        ui_notify(ED.ui, "Script errors - see the Console");
        return false;
    }
    g_errors = 0;
    lx_scripts_unload();
    if (had && !lx_scripts_load(out)) {
        g_errors = 1;
        ui_notify(ED.ui, "Scripts could not be loaded - see the Console");
        return false;
    }
    ED.inspector_dirty = true;
    return true;
}

void ed_scripts_poll(void) {
    double now = lx_time();
    if (now < g_next_check) return;
    g_next_check = now + 1.0;
    ed_scripts_refresh(false);
}

void ed_scripts_close(void) {
    lx_scripts_unload();
    g_have_hash = false;
    g_errors = 0;
}
