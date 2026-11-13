/* The built-in script compiler (src/script/compiler.c, linked into the editor and the tests). */
#pragma once
#include <stdbool.h>

typedef void (*LxScriptDiagFn)(void *user, bool warning, const char *message); /* "file:line: error: ..." */
typedef struct LxScriptCompileStats { int files, errors, warnings, registrations; } LxScriptCompileStats;

/* Unpacks the headers/runtime scripts compile against into sdk_dir (only rewriting changed files). */
bool lx_script_sdk_extract(const char *sdk_dir);
/* Compiles the given .c files into one script module for this machine. */
bool lx_script_compile(const char *const *files, int count, const char *sdk_dir, const char *out_path,
                       LxScriptDiagFn diag, void *user, LxScriptCompileStats *stats);
