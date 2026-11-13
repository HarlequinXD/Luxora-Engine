/* Luxora script SDK: printf/puts write to the Console (and the game's log) */
#ifndef LX_SDK_STDIO_H
#define LX_SDK_STDIO_H
#include <stddef.h>
#include <stdarg.h>
int printf(const char *fmt, ...);
int puts(const char *s);
int snprintf(char *buf, size_t cap, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int sscanf(const char *s, const char *fmt, ...);
#endif
