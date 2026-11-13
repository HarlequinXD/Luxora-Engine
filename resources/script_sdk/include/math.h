/* Luxora script SDK: math */
#ifndef LX_SDK_MATH_H
#define LX_SDK_MATH_H
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_SQRT2 1.41421356237309504880
#define M_E 2.71828182845904523536
#define INFINITY ((float)(1e300 * 1e300))
#define HUGE_VALF INFINITY
#define HUGE_VAL ((double)INFINITY)
#define NAN (INFINITY - INFINITY)
#define isnan(x) ((x) != (x))
#define isinf(x) (!isnan(x) && isnan((x) - (x)))
#define isfinite(x) (!isnan((x) - (x)))
#define signbit(x) ((x) < 0 || (sizeof(x) == sizeof(float) ? 1.0f / (float)(x) < 0 : 1.0 / (double)(x) < 0))
#define LX_SDK_MATH_FN(name) float name##f(float); double name(double);
LX_SDK_MATH_FN(sqrt) LX_SDK_MATH_FN(sin) LX_SDK_MATH_FN(cos) LX_SDK_MATH_FN(tan)
LX_SDK_MATH_FN(asin) LX_SDK_MATH_FN(acos) LX_SDK_MATH_FN(atan) LX_SDK_MATH_FN(exp)
LX_SDK_MATH_FN(log) LX_SDK_MATH_FN(log2) LX_SDK_MATH_FN(log10) LX_SDK_MATH_FN(exp2)
LX_SDK_MATH_FN(floor) LX_SDK_MATH_FN(ceil) LX_SDK_MATH_FN(round) LX_SDK_MATH_FN(trunc)
LX_SDK_MATH_FN(fabs) LX_SDK_MATH_FN(sinh) LX_SDK_MATH_FN(cosh) LX_SDK_MATH_FN(tanh) LX_SDK_MATH_FN(cbrt)
#undef LX_SDK_MATH_FN
#define LX_SDK_MATH_FN2(name) float name##f(float, float); double name(double, double);
LX_SDK_MATH_FN2(atan2) LX_SDK_MATH_FN2(pow) LX_SDK_MATH_FN2(fmod) LX_SDK_MATH_FN2(fmin)
LX_SDK_MATH_FN2(fmax) LX_SDK_MATH_FN2(copysign) LX_SDK_MATH_FN2(hypot)
#undef LX_SDK_MATH_FN2
#endif
