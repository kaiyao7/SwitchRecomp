// Low-level AArch64 numeric semantics for AArch64 -> C static recompilation.
//
// This header is the numeric substrate of the project: every guest arithmetic
// / conversion / flag operation must be expressible as a bit-exact, UB-free,
// inlineable host operation. The whole point is that the generated C stays
// cheap -- each helper folds to one or two host instructions at -O2 on
// GCC/Clang/MSVC, so the host optimizer removes the emulation overhead rather
// than layering it on top.
//
// Design rules (why this layer exists):
//   * Bit exact: W-register results zero-extend to 64 bits; shift amounts are
//     masked the way AArch64 does; FP abs/neg/nan use raw bit patterns; NZCV
//     is computed from unsigned bit patterns, never from signed arithmetic.
//   * No undefined behaviour: all integer arithmetic is unsigned. C signed
//     overflow, shift >= width, and float->int out-of-range are UB; a
//     recompiler must never emit them for arbitrary guest bit patterns.
//   * Zero overhead: every function is `static inline`, header-only, with no
//     allocation, no indirect calls, and no libm dependency.
//
// Pure C++17. Lives under `common`, so translator / codegen / runtime may all
// include it without violating the dependency matrix.
//
// Scope note: this is the *scalar integer + scalar FP flag/conversion* core.
// Vector lane ops, RELR/relocation math, and fenv-controlled rounding are
// deliberately separate concerns (see docs/design/numeric_semantics.md).
#pragma once

#include "switchrecomp/common/types.h"

#include <cstdint>
#include <cstring>

namespace sr::numeric {

using sr::common::u8;
using sr::common::u16;
using sr::common::u32;
using sr::common::u64;
using sr::common::s32;
using sr::common::s64;
using sr::common::f32;
using sr::common::f64;

// ---------------------------------------------------------------------------
// Bit casts. memcpy is strict-aliasing safe and folds to a register move.
// ---------------------------------------------------------------------------

static inline u32 F32Bits(f32 v) {
    u32 b;
    std::memcpy(&b, &v, 4);
    return b;
}
static inline f32 BitsF32(u32 b) {
    f32 v;
    std::memcpy(&v, &b, 4);
    return v;
}
static inline u64 F64Bits(f64 v) {
    u64 b;
    std::memcpy(&b, &v, 8);
    return b;
}
static inline f64 BitsF64(u64 b) {
    f64 v;
    std::memcpy(&v, &b, 8);
    return v;
}

// ---------------------------------------------------------------------------
// Width helpers. AArch64 W operations produce a 32-bit result that is
// zero-extended into the 64-bit X register file. Missing this zero-extension
// is one of the most common recompilation bugs.
// ---------------------------------------------------------------------------

static inline u32 W(u64 x) { return static_cast<u32>(x); }
static inline u64 Zext32(u64 x) { return static_cast<u64>(static_cast<u32>(x)); }
// Defined below in terms of Sext (see the sign-extension helper).
static inline u64 Sext32(u64 x);

// Sign-extend the low `bits` (1..63) of x to 64 bits, branch-free.
static inline u64 Sext(u64 x, unsigned bits) {
    if (bits >= 64) return x;
    const u64 m = (1ull << bits) - 1;
    const u64 v = x & m;
    const u64 sign = 1ull << (bits - 1);
    return (v ^ sign) - sign;
}

static inline u64 Sext32(u64 x) { return Sext(x, 32); }

// ---------------------------------------------------------------------------
// Shifts. AArch64 masks a register shift amount to 5 bits (W) / 6 bits (X).
// C shifts by >= width are UB, so masking is mandatory for arbitrary guest
// values. All shifts here operate on unsigned values: never UB at s==0.
// ---------------------------------------------------------------------------

static inline u32 MaskShift32(u64 amount) { return static_cast<u32>(amount & 31u); }
static inline u32 MaskShift64(u64 amount) { return static_cast<u32>(amount & 63u); }

static inline u64 Shl(u64 a, u64 amount, bool is32) {
    const u32 s = is32 ? MaskShift32(amount) : MaskShift64(amount);
    const u64 r = a << s;
    return is32 ? Zext32(r) : r;
}
static inline u64 Shr(u64 a, u64 amount, bool is32) {
    const u32 s = is32 ? MaskShift32(amount) : MaskShift64(amount);
    const u64 v = is32 ? Zext32(a) : a;
    return v >> s;
}
static inline u64 Sar(u64 a, u64 amount, bool is32) {
    // Arithmetic shift right built from unsigned ops only: we do not rely on
    // the implementation-defined behaviour of signed >>. The fill mask is
    // constructed explicitly and OR-ed into the logical-shift result.
    const u32 s = is32 ? MaskShift32(amount) : MaskShift64(amount);
    if (s == 0) return is32 ? Zext32(a) : a;
    if (is32) {
        const u64 v = Zext32(a);
        const u64 sign = (v >> 31) & 1ull;
        const u64 mask = (~0ull << (32 - s)) & 0xFFFFFFFFull;
        return (v >> s) | (sign ? mask : 0ull);
    }
    const u64 sign = (a >> 63) & 1ull;
    const u64 mask = ~0ull << (64 - s);
    return (a >> s) | (sign ? mask : 0ull);
}

// ---------------------------------------------------------------------------
// Integer arithmetic with NZCV. Everything is derived from unsigned bit
// patterns, so there is no signed-overflow UB for arbitrary guest inputs.
// nzcv layout matches the runtime: bit31 N, bit30 Z, bit29 C, bit28 V.
// ---------------------------------------------------------------------------

struct FlagsResult { u64 value; u32 nzcv; };

static inline FlagsResult AddWithFlags(u64 a, u64 b, bool is32) {
    const u64 x = is32 ? Zext32(a) : a;
    const u64 y = is32 ? Zext32(b) : b;
    const u64 r = x + y;  // unsigned wrap: defined
    const unsigned sb = is32 ? 31u : 63u;
    const u64 rr = is32 ? Zext32(r) : r;
    const u32 n = static_cast<u32>((rr >> sb) & 1u);
    const u32 z = static_cast<u32>(rr == 0);
    // Carry out of the *destination width*: compare the width-masked sum to
    // the masked left operand. (Using the unmasked 64-bit sum here would miss
    // 32-bit carries, because 0xFFFFFFFF+1 does not wrap in 64 bits.)
    const u32 c = static_cast<u32>(rr < x);
    const u32 sa = static_cast<u32>((x >> sb) & 1u);
    const u32 sbb = static_cast<u32>((y >> sb) & 1u);
    const u32 sr = static_cast<u32>((rr >> sb) & 1u);
    const u32 v = static_cast<u32>((sa == sbb) && (sr != sa));
    return {rr, (n << 31) | (z << 30) | (c << 29) | (v << 28)};
}

static inline FlagsResult SubWithFlags(u64 a, u64 b, bool is32) {
    const u64 x = is32 ? Zext32(a) : a;
    const u64 y = is32 ? Zext32(b) : b;
    const u64 r = x - y;  // unsigned wrap: defined
    const unsigned sb = is32 ? 31u : 63u;
    const u64 rr = is32 ? Zext32(r) : r;
    const u32 n = static_cast<u32>((rr >> sb) & 1u);
    const u32 z = static_cast<u32>(rr == 0);
    const u32 c = static_cast<u32>(x >= y);  // SUBS carry = NOT borrow
    const u32 sa = static_cast<u32>((x >> sb) & 1u);
    const u32 sbb = static_cast<u32>((y >> sb) & 1u);
    const u32 sr = static_cast<u32>((rr >> sb) & 1u);
    const u32 v = static_cast<u32>((sa != sbb) && (sr != sa));
    return {rr, (n << 31) | (z << 30) | (c << 29) | (v << 28)};
}

// ANDS / flag-setting logical ops: N/Z from result, C=0, V=0.
static inline u32 LogicFlags(u64 result, bool is32) {
    const u64 rr = is32 ? Zext32(result) : result;
    const unsigned sb = is32 ? 31u : 63u;
    const u32 n = static_cast<u32>((rr >> sb) & 1u);
    const u32 z = static_cast<u32>(rr == 0);
    return (n << 31) | (z << 30);
}

// Condition-code evaluation: given NZCV, does `cond` (0..15) hold?
static inline bool ConditionHolds(u32 nzcv, u8 cond) {
    const u32 n = (nzcv >> 31) & 1u;
    const u32 z = (nzcv >> 30) & 1u;
    const u32 c = (nzcv >> 29) & 1u;
    const u32 v = (nzcv >> 28) & 1u;
    switch (cond & 0xF) {
        case 0:  return z == 1;                    // EQ
        case 1:  return z == 0;                    // NE
        case 2:  return c == 1;                    // CS/HS
        case 3:  return c == 0;                    // CC/LO
        case 4:  return n == 1;                    // MI
        case 5:  return n == 0;                    // PL
        case 6:  return v == 1;                    // VS
        case 7:  return v == 0;                    // VC
        case 8:  return c == 1 && z == 0;         // HI
        case 9:  return c == 0 || z == 1;         // LS
        case 10: return n == v;                   // GE
        case 11: return n != v;                   // LT
        case 12: return z == 0 && n == v;         // GT
        case 13: return z == 1 || n != v;         // LE
        case 14: return true;                     // AL
        case 15: return true;                     // NV (behaves as AL)
    }
    return true;
}

// ---------------------------------------------------------------------------
// FP bit-level helpers. Generated code needs FABS / FNEG / NaN handling to be
// exact for NaN and signed zero, and cheap: raw bit manipulation is one host
// instruction, whereas libm calls would dominate the hot path.
// ---------------------------------------------------------------------------

static inline u32 FpSign32(f32 v) { return F32Bits(v) >> 31; }
static inline u32 FpSign64(f64 v) { return static_cast<u32>(F64Bits(v) >> 63); }

static inline f32 FpAbs32(f32 v) { return BitsF32(F32Bits(v) & 0x7FFFFFFFu); }
static inline f64 FpAbs64(f64 v) { return BitsF64(F64Bits(v) & 0x7FFFFFFFFFFFFFFFull); }
static inline f32 FpNeg32(f32 v) { return BitsF32(F32Bits(v) ^ 0x80000000u); }
static inline f64 FpNeg64(f64 v) { return BitsF64(F64Bits(v) ^ 0x8000000000000000ull); }

// Default (quiet, positive) NaN produced by AArch64 default-NaN arithmetic.
static inline f32 DefaultNan32() { return BitsF32(0x7FC00000u); }
static inline f64 DefaultNan64() { return BitsF64(0x7FF8000000000000ull); }

static inline bool IsNan32(f32 v) {
    const u32 b = F32Bits(v);
    return (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu) != 0;
}
static inline bool IsNan64(f64 v) {
    const u64 b = F64Bits(v);
    return (b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
           (b & 0x000FFFFFFFFFFFFFull) != 0;
}

// FCMP -> NZCV. NaN makes the comparison unordered: N=Z=C=V=0 here, so all
// ordered condition codes (EQ/GT/GE/...) fail, as required. FCMPE differs only
// in raising the invalid exception, which is a runtime concern, not a flag.
static inline u32 FpCompareNZCV(f64 a, f64 b) {
    if (IsNan64(a) || IsNan64(b)) return 0;
    const u32 z = static_cast<u32>(a == b);
    const u32 c = static_cast<u32>(a >= b);
    // N=0, V=0 for FCMP (AArch64 sets N/Z/C only for ordered results).
    return (z << 30) | (c << 29);
}

// ---------------------------------------------------------------------------
// FP -> integer conversion (FCVTZS / FCVTZU truncation semantics).
//
// This is UB-free by construction: the only float->integer cast happens after
// a range check that guarantees the value is representable in the target
// integer type. Out-of-range and NaN follow the AArch64 FCVTZ* rules:
//   * NaN            -> 0
//   * out of range   -> saturate to the integer type's min / max
//   * otherwise      -> truncate toward zero
//
// `is32` selects a 32-bit destination (W register). For F64 sources the
// bounds below are exactly representable as double, so the comparisons are
// exact; the cast itself is then well defined.
// ---------------------------------------------------------------------------

static inline u64 FpToUnsigned(f64 v, bool is32) {
    if (!(v == v)) return 0;  // NaN
    if (v <= 0.0) return 0;
    // 2^32-1 and 2^64 are exactly representable as double.
    const f64 lim = is32 ? 4294967295.0 : 18446744073709551616.0;  // 2^64
    if (v >= lim) return is32 ? 0xFFFFFFFFull : ~0ull;             // saturate
    // v is now in (0, 2^64) so the cast to u64 is well defined.
    const u64 r = static_cast<u64>(v);  // truncates toward zero
    return is32 ? (r & 0xFFFFFFFFull) : r;
}

static inline s64 FpToSigned(f64 v, bool is32) {
    if (!(v == v)) return 0;  // NaN
    // Bounds are exactly representable as double. A value strictly inside
    // [lo, hi) casts to the signed type without UB.
    const f64 hi = is32 ? 2147483648.0 : 9223372036854775808.0;   // 2^31 / 2^63
    const f64 lo = is32 ? -2147483648.0 : -9223372036854775808.0;
    if (v >= hi) return is32 ? 2147483647ll : 9223372036854775807ll;
    if (v < lo)  return is32 ? (-2147483647ll - 1) : (-9223372036854775807ll - 1);
    const s64 r = static_cast<s64>(v);  // truncates toward zero, in range
    return is32 ? static_cast<s64>(static_cast<s32>(r)) : r;
}

// Integer -> FP (SCVTF / UCVTF). The u64 -> double cast is well defined for
// every bit pattern (it rounds); narrowing to float is a separate, defined
// conversion.
static inline f64 IntToFp(u64 value, bool signed_source, bool is32_result) {
    const f64 d = signed_source
        ? static_cast<f64>(static_cast<s64>(value))
        : static_cast<f64>(value);
    return is32_result ? static_cast<f64>(static_cast<f32>(d)) : d;
}

} // namespace sr::numeric
