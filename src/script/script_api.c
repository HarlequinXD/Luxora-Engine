/* What compiled game scripts can call: the whole public engine API (luxora.h, listed in the
 * generated script_api_table.h) plus the small C library declared by the script SDK headers
 * (resources/script_sdk/include). */
#include "../lx_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

/* printf and puts go to the Console / game log */
static int script_printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t len = strlen(buf);
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
    lx_log(LX_LOG_INFO, "%s", buf);
    return n;
}
static int script_puts(const char *s) { lx_log(LX_LOG_INFO, "%s", s); return 1; }
static int script_snprintf(char *b, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, cap, fmt, ap);
    va_end(ap);
    return n;
}
static int script_sprintf(char *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, 1u << 20, fmt, ap);
    va_end(ap);
    return n;
}
static int script_vsnprintf(char *b, size_t cap, const char *fmt, va_list ap) { return vsnprintf(b, cap, fmt, ap); }
static void script_assert_failed(const char *e, const char *file, int line) {
    lx_log(LX_LOG_ERROR, "Assertion failed: %s (%s:%d)", e, lx_path_filename(file), line);
}
static void script_abort(void) {
    lx_log(LX_LOG_ERROR, "A script called abort()");
    abort();
}
static int script_rand(void) { return rand() & 0x7fff; }
static int script_abs(int x) { return abs(x); }
static long long script_llabs(long long x) { return x < 0 ? -x : x; }

/* the C library: wrapped so each is a plain function with the SDK's signature on every compiler */
#define W1(n)                                                     \
    static float w_##n##f(float x) { return n##f(x); }            \
    static double w_##n(double x) { return n(x); }
#define W2(n)                                                     \
    static float w_##n##f(float x, float y) { return n##f(x, y); } \
    static double w_##n(double x, double y) { return n(x, y); }
W1(sqrt) W1(sin) W1(cos) W1(tan) W1(asin) W1(acos) W1(atan) W1(exp) W1(log) W1(log2) W1(log10) W1(exp2)
W1(floor) W1(ceil) W1(round) W1(trunc) W1(fabs) W1(sinh) W1(cosh) W1(tanh) W1(cbrt)
W2(atan2) W2(pow) W2(fmod) W2(fmin) W2(fmax) W2(copysign) W2(hypot)
static void *w_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void *w_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static void *w_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static int w_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static void *w_memchr(const void *s, int c, size_t n) { return (void *)memchr(s, c, n); }
static size_t w_strlen(const char *s) { return strlen(s); }
static int w_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static int w_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static char *w_strcpy(char *d, const char *s) { return strcpy(d, s); }
static char *w_strncpy(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
static char *w_strcat(char *d, const char *s) { return strcat(d, s); }
static char *w_strncat(char *d, const char *s, size_t n) { return strncat(d, s, n); }
static char *w_strchr(const char *s, int c) { return (char *)strchr(s, c); }
static char *w_strrchr(const char *s, int c) { return (char *)strrchr(s, c); }
static char *w_strstr(const char *s, const char *f) { return (char *)strstr(s, f); }
static void *w_malloc(size_t n) { return malloc(n); }
static void *w_calloc(size_t c, size_t n) { return calloc(c, n); }
static void *w_realloc(void *p, size_t n) { return realloc(p, n); }
static void w_free(void *p) { free(p); }
static int w_atoi(const char *s) { return atoi(s); }
static double w_atof(const char *s) { return atof(s); }
static long w_strtol(const char *s, char **e, int b) { return strtol(s, e, b); }
static float w_strtof(const char *s, char **e) { return strtof(s, e); }
static double w_strtod(const char *s, char **e) { return strtod(s, e); }
static void w_srand(unsigned s) { srand(s); }
static void w_qsort(void *b, size_t n, size_t sz, int (*cmp)(const void *, const void *)) { qsort(b, n, sz, cmp); }
static int w_sscanf(const char *s, const char *fmt, void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
    return sscanf(s, fmt, a, b, c, d, e, f, g, h); /* up to 8 outputs */
}

typedef struct ApiSym { const char *name; const void *addr; } ApiSym;
#define FN(f) (const void *)(uintptr_t)(f)
static const ApiSym g_api[] = {
#define LX_API_FN(n) {#n, FN(n)},
#include "script_api_table.h"
#undef LX_API_FN
#define M1(n) {#n "f", FN(w_##n##f)}, {#n, FN(w_##n)},
    M1(sqrt) M1(sin) M1(cos) M1(tan) M1(asin) M1(acos) M1(atan) M1(exp) M1(log) M1(log2) M1(log10) M1(exp2)
    M1(floor) M1(ceil) M1(round) M1(trunc) M1(fabs) M1(sinh) M1(cosh) M1(tanh) M1(cbrt)
    M1(atan2) M1(pow) M1(fmod) M1(fmin) M1(fmax) M1(copysign) M1(hypot)
#undef M1
    {"memcpy", FN(w_memcpy)}, {"memmove", FN(w_memmove)}, {"memset", FN(w_memset)}, {"memcmp", FN(w_memcmp)},
    {"memchr", FN(w_memchr)}, {"strlen", FN(w_strlen)}, {"strcmp", FN(w_strcmp)}, {"strncmp", FN(w_strncmp)},
    {"strcpy", FN(w_strcpy)}, {"strncpy", FN(w_strncpy)}, {"strcat", FN(w_strcat)}, {"strncat", FN(w_strncat)},
    {"strchr", FN(w_strchr)}, {"strrchr", FN(w_strrchr)}, {"strstr", FN(w_strstr)},
    {"printf", FN(script_printf)}, {"puts", FN(script_puts)}, {"snprintf", FN(script_snprintf)},
    {"sprintf", FN(script_sprintf)}, {"vsnprintf", FN(script_vsnprintf)}, {"sscanf", FN(w_sscanf)},
    {"malloc", FN(w_malloc)}, {"calloc", FN(w_calloc)}, {"realloc", FN(w_realloc)}, {"free", FN(w_free)},
    {"abs", FN(script_abs)}, {"llabs", FN(script_llabs)}, {"atoi", FN(w_atoi)}, {"atof", FN(w_atof)},
    {"strtol", FN(w_strtol)}, {"strtof", FN(w_strtof)}, {"strtod", FN(w_strtod)},
    {"rand", FN(script_rand)}, {"srand", FN(w_srand)}, {"qsort", FN(w_qsort)}, {"abort", FN(script_abort)},
    {"lx_script_assert_failed", FN(script_assert_failed)},
};

const void *lx_script_api_symbol(const char *name) {
    for (size_t i = 0; i < sizeof g_api / sizeof g_api[0]; i++)
        if (!strcmp(g_api[i].name, name)) return g_api[i].addr;
    return NULL;
}

int lx_script_api_count(void) { return (int)(sizeof g_api / sizeof g_api[0]); }
