// Keep the live libraries usable with the Steam runtime's older libm. Building
// against glibc 2.43 otherwise selects newly versioned defaults for these five
// unchanged public APIs. The explicit imports below use the x86-64 baseline.
//
// Each wrapper is hidden: it binds calls from this library without interposing
// math in CS2 or other injected libraries. Its distinct, versioned import calls
// libm directly, so it cannot recurse into the unversioned wrapper.
#if defined(__linux__) && defined(__x86_64__)
extern "C" double cs2_math_old_remainder(double, double);
extern "C" float cs2_math_old_sqrtf(float);
extern "C" float cs2_math_old_acosf(float);
extern "C" float cs2_math_old_atan2f(float, float);
extern "C" float cs2_math_old_log10f(float);

__asm__(".symver cs2_math_old_remainder,remainder@GLIBC_2.2.5");
__asm__(".symver cs2_math_old_sqrtf,sqrtf@GLIBC_2.2.5");
__asm__(".symver cs2_math_old_acosf,acosf@GLIBC_2.2.5");
__asm__(".symver cs2_math_old_atan2f,atan2f@GLIBC_2.2.5");
__asm__(".symver cs2_math_old_log10f,log10f@GLIBC_2.2.5");

extern "C" double remainder(double x, double y) noexcept { return cs2_math_old_remainder(x,y); }
extern "C" float sqrtf(float value) noexcept { return cs2_math_old_sqrtf(value); }
extern "C" float acosf(float value) noexcept { return cs2_math_old_acosf(value); }
extern "C" float atan2f(float y, float x) noexcept { return cs2_math_old_atan2f(y,x); }
extern "C" float log10f(float value) noexcept { return cs2_math_old_log10f(value); }

// Assembly visibility avoids conflicting with GCC's built-in declarations.
__asm__(".hidden remainder\n.hidden sqrtf\n.hidden acosf\n.hidden atan2f\n.hidden log10f");
#endif
