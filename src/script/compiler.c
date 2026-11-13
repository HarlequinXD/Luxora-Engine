/* The built-in script compiler: turns Assets/Scripts into a script module (scripts.lxo) with
 * TinyCC, linked into the editor as a library. Needs nothing installed: the C headers scripts can
 * use (the Luxora API and a small C library) are embedded and unpacked into the project's Library
 * folder when needed. The module is loaded by src/script/module.c, in the editor and in games. */
#include "../lx_internal.h"
#include "compiler.h"
#include "libtcc.h"
#include <stdio.h>
#include <string.h>

typedef struct LxSdkFile { const char *path; const unsigned char *data; unsigned size; } LxSdkFile;
extern const LxSdkFile lx_sdk_files[];
extern const int lx_sdk_file_count;

bool lx_script_sdk_extract(const char *dir) {
    bool ok = true;
    for (int i = 0; i < lx_sdk_file_count; i++) {
        char path[LX_PATH_MAX], parent[LX_PATH_MAX];
        lx_path_join(path, sizeof path, dir, lx_sdk_files[i].path);
        size_t size;
        char *old = lx_file_read(path, &size);
        bool same = old && size == lx_sdk_files[i].size && !memcmp(old, lx_sdk_files[i].data, size);
        lx_free(old);
        if (same) continue;
        lx_path_parent(parent, sizeof parent, path);
        lx_mkdir_p(parent);
        ok &= lx_file_write(path, lx_sdk_files[i].data, lx_sdk_files[i].size);
    }
    return ok;
}

typedef struct DiagCtx {
    LxScriptDiagFn fn;
    void *user;
    int errors, warnings;
} DiagCtx;

static void on_tcc_message(void *opaque, const char *msg) {
    DiagCtx *c = opaque;
    bool warning = strstr(msg, ": warning: ") != NULL;
    if (warning) c->warnings++;
    else c->errors++;
    if (c->fn) c->fn(c->user, warning, msg);
}

/* the registration calls: every LX_SCRIPT_REGISTER(name) in the scripts */
static void collect_registrations(const char *text, char *decls, size_t dcap, char *calls, size_t ccap) {
    for (const char *p = text; (p = strstr(p, "LX_SCRIPT_REGISTER")); p++) {
        const char *q = p + strlen("LX_SCRIPT_REGISTER");
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '(') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        char ident[LX_NAME_MAX];
        int n = 0;
        while (n < (int)sizeof ident - 1 && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_'))
            ident[n++] = *q++;
        ident[n] = 0;
        while (*q == ' ' || *q == '\t') q++;
        if (!n || *q != ')') continue;
        /* skip the macro's own definition and duplicates */
        if (!strcmp(ident, "name")) continue;
        char line[LX_NAME_MAX + 32];
        snprintf(line, sizeof line, "void lx_script_%s(void);\n", ident);
        if (strstr(decls, line)) continue;
        lx_strcpy(decls + strlen(decls), dcap - strlen(decls), line);
        snprintf(line, sizeof line, "    lx_script_%s();\n", ident);
        lx_strcpy(calls + strlen(calls), ccap - strlen(calls), line);
    }
}

bool lx_script_compile(const char *const *files, int count, const char *sdk_dir, const char *out_path,
                       LxScriptDiagFn diag, void *user, LxScriptCompileStats *stats) {
    DiagCtx ctx = {diag, user, 0, 0};
    if (stats) memset(stats, 0, sizeof *stats);
    if (!lx_script_sdk_extract(sdk_dir)) {
        if (diag) diag(user, false, "Could not write the script SDK headers to the project's Library folder");
        return false;
    }
    TCCState *s = tcc_new();
    if (!s) return false;
    tcc_set_error_func(s, &ctx, on_tcc_message);
    tcc_set_options(s, "-nostdlib -nostdinc -Wall -Wno-unused-function");
    char inc[LX_PATH_MAX], lib[LX_PATH_MAX], path[LX_PATH_MAX];
    lx_path_join(inc, sizeof inc, sdk_dir, "include");
    lx_path_join(lib, sizeof lib, sdk_dir, "lib");
    tcc_add_sysinclude_path(s, inc);
    tcc_set_lib_path(s, lib);
    tcc_define_symbol(s, "LX_SCRIPT", "1");
    tcc_set_output_type(s, TCC_OUTPUT_OBJ);

    size_t dcap = 16384, ccap = 16384;
    char *decls = lx_calloc(1, dcap), *calls = lx_calloc(1, ccap);
    bool ok = true;
    for (int i = 0; i < count && ok; i++) {
        size_t size;
        char *text = lx_file_read(files[i], &size);
        if (text) {
            char *z = lx_realloc(text, size + 1);
            z[size] = 0;
            collect_registrations(z, decls, dcap, calls, ccap);
            lx_free(z);
        }
        if (tcc_add_file(s, files[i]) < 0) ok = false;
        else if (stats) stats->files++;
    }
    if (ok) {
        size_t cap = dcap + ccap + 256;
        char *unit = lx_alloc(cap);
        snprintf(unit, cap, "%svoid lx_game_register_scripts(void) {\n%s}\nconst char lx_module_abi[] = \"%s\";\n", decls, calls,
                 LX_MODULE_ABI);
        if (tcc_compile_string(s, unit) < 0) ok = false;
        lx_free(unit);
    }
    /* the compiler's runtime helpers (64-bit division, varargs, alloca, stack probes) */
#if defined(__x86_64__) || defined(_M_X64)
    static const char *runtime[] = {"libtcc1.c", "alloca.S",
#if LX_OS_WINDOWS
                                    "chkstk.S",
#else
                                    "va_list.c",
#endif
                                    NULL};
#elif defined(__aarch64__) || defined(_M_ARM64)
    static const char *runtime[] = {"lib-arm64.c", NULL};
#else
    static const char *runtime[] = {NULL};
#endif
    for (int i = 0; ok && runtime[i]; i++) {
        lx_path_join(path, sizeof path, lib, runtime[i]);
        if (tcc_add_file(s, path) < 0) ok = false;
    }
    if (ok) {
        char parent[LX_PATH_MAX];
        lx_path_parent(parent, sizeof parent, out_path);
        if (parent[0]) lx_mkdir_p(parent);
        if (tcc_output_file(s, out_path) < 0) ok = false;
    }
    tcc_delete(s);
    if (stats) {
        stats->errors = ctx.errors;
        stats->warnings = ctx.warnings;
        stats->registrations = 0;
        for (const char *p = calls; (p = strstr(p, "lx_script_")); p++) stats->registrations++;
    }
    lx_free(decls);
    lx_free(calls);
    return ok && !ctx.errors;
}
