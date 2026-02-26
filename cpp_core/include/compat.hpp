#pragma once

// ── Branch prediction hints (portable across GCC/Clang/MSVC) ─────────────────
#if defined(__GNUC__) || defined(__clang__)
#  define LIKELY(x)   (__builtin_expect(!!(x), 1))
#  define UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#  define LIKELY(x)   (x)
#  define UNLIKELY(x) (x)
#endif
