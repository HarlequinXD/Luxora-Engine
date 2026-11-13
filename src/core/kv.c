#include "../lx_internal.h"
#include <stdarg.h>
#include <ctype.h>

void lx_sb_printf(LxStrBuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    if (sb->len + (size_t)n + 1 > sb->cap) {
        size_t cap = sb->cap ? sb->cap : 1024;
        while (cap < sb->len + (size_t)n + 1) cap *= 2;
        sb->data = lx_realloc(sb->data, cap);
        sb->cap = cap;
    }
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, n - i + 1);
}

int lx_kv_next(const char **cursor, char *key, size_t kcap, char *val, size_t vcap) {
    const char *p = *cursor;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[1024];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p = eol ? eol + 1 : p + strlen(p);
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) *end = 0;
            lx_strcpy(key, kcap, line + 1);
            val[0] = 0;
            *cursor = p;
            return 2;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char k[256], v[1024];
        lx_strcpy(k, sizeof k, line);
        lx_strcpy(v, sizeof v, eq + 1);
        trim(k);
        trim(v);
        lx_strcpy(key, kcap, k);
        lx_strcpy(val, vcap, v);
        *cursor = p;
        return 1;
    }
    *cursor = p;
    return 0;
}

LxVec3 lx_parse_vec3(const char *s) {
    LxVec3 v = {0, 0, 0};
    sscanf(s, "%f %f %f", &v.x, &v.y, &v.z);
    return v;
}
LxVec4 lx_parse_vec4(const char *s) {
    LxVec4 v = {0, 0, 0, 1};
    sscanf(s, "%f %f %f %f", &v.x, &v.y, &v.z, &v.w);
    return v;
}
bool lx_parse_bool(const char *s) { return s[0] == '1' || s[0] == 't' || s[0] == 'T' || s[0] == 'y'; }
