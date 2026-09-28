// Guest CPU context and helpers used by recompiled AArch64 code.
//
// Guest memory is host memory: the module is mapped at NX_BASE and every guest pointer is a host pointer.
// Both sides are little endian, so loads and stores are plain (unaligned-safe) memory accesses.
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#ifndef NX_BASE
#define NX_BASE 0x0000004000000000ull
#endif

namespace nx {

union alignas(16) V128 {
    uint8_t u8[16];
    int8_t s8[16];
    uint16_t u16[8];
    int16_t s16[8];
    uint32_t u32[4];
    int32_t s32[4];
    uint64_t u64[2];
    int64_t s64[2];
    float f32[4];
    double f64[2];
};

struct CPU {
    uint64_t x[32];  // x[31] is SP; XZR is handled by the recompiler
    V128 v[32];
    uint8_t nf, zf, cf, vf;
    uint64_t fpcr, fpsr;
    uint64_t tpidr, tpidrro;
    uint64_t excl_addr = ~0ull;
    uint64_t excl_val, excl_val_hi;
};

using Fn = void (*)(CPU&);

// Provided by the runtime.
void call(CPU& c, uint64_t target);  // indirect call / tail call through the function table
[[noreturn]] void unimpl(CPU& c, uint64_t pc, const char* what);
void svc(CPU& c, uint32_t imm, uint64_t pc);
uint64_t mrs(CPU& c, uint32_t sysreg);
void msr(CPU& c, uint32_t sysreg, uint64_t value);

// ---- memory -------------------------------------------------------------------------------------------------

template <typename T>
inline T ld(uint64_t a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T));
    return v;
}

template <typename T>
inline void st(uint64_t a, T v) {
    std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T));
}

template <typename T>
inline T ld_acq(uint64_t a) {
    return __atomic_load_n(reinterpret_cast<T*>(a), __ATOMIC_ACQUIRE);
}

template <typename T>
inline void st_rel(uint64_t a, T v) {
    __atomic_store_n(reinterpret_cast<T*>(a), v, __ATOMIC_RELEASE);
}

// Exclusive monitor: LDXR remembers (address, value); STXR succeeds if memory still holds that value.
template <typename T>
inline T ldxr(CPU& c, uint64_t a) {
    T v = __atomic_load_n(reinterpret_cast<T*>(a), __ATOMIC_ACQUIRE);
    c.excl_addr = a;
    c.excl_val = v;
    return v;
}

template <typename T>
inline uint32_t stxr(CPU& c, uint64_t a, T v) {
    if (c.excl_addr != a) return 1;
    c.excl_addr = ~0ull;
    T expected = static_cast<T>(c.excl_val);
    return __atomic_compare_exchange_n(reinterpret_cast<T*>(a), &expected, v, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST)
               ? 0
               : 1;
}

inline void clrex(CPU& c) { c.excl_addr = ~0ull; }

inline void barrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

// ---- flags --------------------------------------------------------------------------------------------------

inline uint64_t adds64(CPU& c, uint64_t a, uint64_t b, uint64_t carry) {
    uint64_t r = a + b + carry;
    c.nf = r >> 63;
    c.zf = r == 0;
    c.cf = carry ? (r <= a) : (r < a);
    c.vf = ((a ^ r) & (b ^ r)) >> 63;
    return r;
}

inline uint32_t adds32(CPU& c, uint32_t a, uint32_t b, uint32_t carry) {
    uint64_t wide = uint64_t(a) + b + carry;
    uint32_t r = uint32_t(wide);
    c.nf = r >> 31;
    c.zf = r == 0;
    c.cf = wide >> 32;
    c.vf = ((a ^ r) & (b ^ r)) >> 31;
    return r;
}

inline uint64_t subs64(CPU& c, uint64_t a, uint64_t b) { return adds64(c, a, ~b, 1); }
inline uint32_t subs32(CPU& c, uint32_t a, uint32_t b) { return adds32(c, a, ~b, 1); }

inline void logic64(CPU& c, uint64_t r) {
    c.nf = r >> 63;
    c.zf = r == 0;
    c.cf = 0;
    c.vf = 0;
}

inline void logic32(CPU& c, uint32_t r) {
    c.nf = r >> 31;
    c.zf = r == 0;
    c.cf = 0;
    c.vf = 0;
}

inline void set_nzcv(CPU& c, uint32_t nzcv) {
    c.nf = (nzcv >> 3) & 1;
    c.zf = (nzcv >> 2) & 1;
    c.cf = (nzcv >> 1) & 1;
    c.vf = nzcv & 1;
}

inline uint64_t get_nzcv(const CPU& c) {
    return (uint64_t(c.nf) << 31) | (uint64_t(c.zf) << 30) | (uint64_t(c.cf) << 29) | (uint64_t(c.vf) << 28);
}

template <typename F>
inline void fcmp(CPU& c, F a, F b) {
    if (std::isnan(a) || std::isnan(b))
        set_nzcv(c, 0b0011);
    else if (a == b)
        set_nzcv(c, 0b0110);
    else if (a < b)
        set_nzcv(c, 0b1000);
    else
        set_nzcv(c, 0b0010);
}

// ---- integer helpers ----------------------------------------------------------------------------------------

inline uint16_t bswap(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t bswap(uint32_t v) { return __builtin_bswap32(v); }
inline uint64_t bswap(uint64_t v) { return __builtin_bswap64(v); }

inline uint64_t ror64(uint64_t v, unsigned s) { return std::rotr(v, int(s & 63)); }
inline uint32_t ror32(uint32_t v, unsigned s) { return std::rotr(v, int(s & 31)); }
inline uint64_t asr64(uint64_t v, unsigned s) { return uint64_t(int64_t(v) >> (s & 63)); }
inline uint32_t asr32(uint32_t v, unsigned s) { return uint32_t(int32_t(v) >> (s & 31)); }

inline uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
inline int64_t sdiv64(int64_t a, int64_t b) {
    if (!b) return 0;
    if (a == std::numeric_limits<int64_t>::min() && b == -1) return a;
    return a / b;
}
inline int32_t sdiv32(int32_t a, int32_t b) {
    if (!b) return 0;
    if (a == std::numeric_limits<int32_t>::min() && b == -1) return a;
    return a / b;
}

inline uint64_t umulh(uint64_t a, uint64_t b) {
#if defined(_MSC_VER)
    return __umulh(a, b);
#else
    return uint64_t((unsigned __int128)a * b >> 64);
#endif
}
inline int64_t smulh(int64_t a, int64_t b) {
#if defined(_MSC_VER)
    return __mulh(a, b);
#else
    return int64_t((__int128)a * b >> 64);
#endif
}

inline uint64_t rbit64(uint64_t v) {
    v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
    v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
    v = ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
    return nx::bswap(v);
}
inline uint32_t rbit32(uint32_t v) { return uint32_t(rbit64(v) >> 32); }
inline uint64_t rev16_64(uint64_t v) { return ((v >> 8) & 0x00FF00FF00FF00FFull) | ((v & 0x00FF00FF00FF00FFull) << 8); }
inline uint32_t rev16_32(uint32_t v) { return ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8); }
inline uint64_t rev32_64(uint64_t v) {
    return uint64_t(nx::bswap(uint32_t(v))) | (uint64_t(nx::bswap(uint32_t(v >> 32))) << 32);
}

// ---- floating point -----------------------------------------------------------------------------------------

inline float f16_to_f32(uint16_t h) {
    uint32_t s = uint32_t(h >> 15) << 31, e = (h >> 10) & 0x1F, m = h & 0x3FF, bits;
    if (e == 0) {
        if (m == 0) {
            bits = s;
        } else {  // subnormal
            e = 127 - 15 + 1;
            while (!(m & 0x400)) {
                m <<= 1;
                e--;
            }
            bits = s | (e << 23) | ((m & 0x3FF) << 13);
        }
    } else if (e == 31) {
        bits = s | 0x7F800000u | (m << 13);
    } else {
        bits = s | ((e + 127 - 15) << 23) | (m << 13);
    }
    return std::bit_cast<float>(bits);
}

inline uint16_t f32_to_f16(float f) {
    uint32_t x = std::bit_cast<uint32_t>(f);
    uint32_t s = (x >> 16) & 0x8000;
    int32_t e = int32_t((x >> 23) & 0xFF) - 127 + 15;
    uint32_t m = x & 0x7FFFFF;
    if (((x >> 23) & 0xFF) == 0xFF) return uint16_t(s | 0x7C00 | (m ? 0x200 | (m >> 13) : 0));
    if (e >= 31) return uint16_t(s | 0x7C00);
    if (e <= 0) {
        if (e < -10) return uint16_t(s);
        m |= 0x800000;
        uint32_t shift = uint32_t(14 - e);
        uint32_t r = m >> shift, rem = m & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (r & 1))) r++;
        return uint16_t(s | r);
    }
    uint32_t r = (uint32_t(e) << 10) | (m >> 13), rem = m & 0x1FFF;
    if (rem > 0x1000 || (rem == 0x1000 && (r & 1))) r++;
    return uint16_t(s | r);
}

// Float -> integer with AArch64 saturation (NaN -> 0). Rounding already applied by the caller.
template <typename I, typename F>
inline I fcvt_sat(F v) {
    if (std::isnan(v)) return 0;
    if (v <= F(std::numeric_limits<I>::min())) return std::numeric_limits<I>::min();
    if (v >= F(std::numeric_limits<I>::max())) return std::numeric_limits<I>::max();
    return I(v);
}

template <typename F>
inline F round_away(F v) { return std::round(v); }
template <typename F>
inline F round_even(F v) { return std::nearbyint(v); }  // host default rounding: nearest-even

template <typename F>
inline bool is_snan(F v) {
    if constexpr (sizeof(F) == 4)
        return std::isnan(v) && !(std::bit_cast<uint32_t>(v) & 0x00400000u);
    else
        return std::isnan(v) && !(std::bit_cast<uint64_t>(v) & 0x0008000000000000ull);
}

template <typename F>
inline F quiet(F v) {
    if constexpr (sizeof(F) == 4)
        return std::bit_cast<F>(std::bit_cast<uint32_t>(v) | 0x00400000u);
    else
        return std::bit_cast<F>(std::bit_cast<uint64_t>(v) | 0x0008000000000000ull);
}

// FMAXNM/FMINNM: a quiet NaN is a missing value, a signaling NaN propagates (quieted); +0 > -0.
template <typename F>
inline F fmaxnm(F a, F b) {
    if (is_snan(a)) return quiet(a);
    if (is_snan(b)) return quiet(b);
    if (std::isnan(a)) return std::isnan(b) ? a : b;
    if (std::isnan(b)) return a;
    if (a == 0 && b == 0) return std::signbit(a) ? b : a;
    return a > b ? a : b;
}
template <typename F>
inline F fminnm(F a, F b) {
    if (is_snan(a)) return quiet(a);
    if (is_snan(b)) return quiet(b);
    if (std::isnan(a)) return std::isnan(b) ? a : b;
    if (std::isnan(b)) return a;
    if (a == 0 && b == 0) return std::signbit(a) ? a : b;
    return a < b ? a : b;
}
template <typename F>
inline F fmax_(F a, F b) {
    if (std::isnan(a) || std::isnan(b)) return a + b;
    if (a == 0 && b == 0) return std::signbit(a) ? b : a;
    return a > b ? a : b;
}
template <typename F>
inline F fmin_(F a, F b) {
    if (std::isnan(a) || std::isnan(b)) return a + b;
    if (a == 0 && b == 0) return std::signbit(a) ? a : b;
    return a < b ? a : b;
}

// ---- vector helpers -----------------------------------------------------------------------------------------

inline V128 vzero() {
    V128 r;
    r.u64[0] = r.u64[1] = 0;
    return r;
}

template <typename T>
inline T sat(int64_t v) {
    if (v < int64_t(std::numeric_limits<T>::min())) return std::numeric_limits<T>::min();
    if (v > int64_t(std::numeric_limits<T>::max())) return std::numeric_limits<T>::max();
    return T(v);
}

}  // namespace nx
