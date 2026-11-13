#ifndef LX_SDK_ASSERT_H
#define LX_SDK_ASSERT_H
void lx_script_assert_failed(const char *expr, const char *file, int line);
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : lx_script_assert_failed(#e, __FILE__, __LINE__))
#endif
#endif
