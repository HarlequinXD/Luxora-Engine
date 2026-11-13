#ifndef LX_SDK_STDLIB_H
#define LX_SDK_STDLIB_H
#include <stddef.h>
#define RAND_MAX 0x7fff
void *malloc(size_t n);
void *calloc(size_t count, size_t n);
void *realloc(void *p, size_t n);
void free(void *p);
int abs(int x);
long long llabs(long long x);
int atoi(const char *s);
double atof(const char *s);
long strtol(const char *s, char **end, int base);
float strtof(const char *s, char **end);
double strtod(const char *s, char **end);
int rand(void);
void srand(unsigned seed);
void qsort(void *base, size_t count, size_t size, int (*cmp)(const void *, const void *));
void abort(void);
#endif
