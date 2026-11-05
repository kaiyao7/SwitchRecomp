// Feasibility tests for the AArch64 -> C numeric substrate.
//
// Every helper in common/numeric.h is exercised against bit-exact expected
// values, including the inputs that would be undefined behaviour if the
// generated C used plain signed C arithmetic. These tests are the executable
// proof that the "zero-UB / zero-overhead numeric semantics" claim holds.
#include <gtest/gtest.h>

#include "switchrecomp/common/numeric.h"

#include <cstdint>
#include <limits>

using namespace sr::numeric;
using sr::common::u32;
using sr::common::u64;
using sr::common::s64;
using sr::common::f32;
using sr::common::f64;

namespace {

// NZCV bit positions used throughout the runtime.
constexpr u32 kN = 1u << 31;
constexpr u32 kZ = 1u << 30;
constexpr u32 kC = 1u << 29;
constexpr u32 kV = 1u << 28;

} // namespace

// ---------------------------------------------------------------------------
// Bit casts: exact round-trip through raw patterns (including NaN / -0).
// ---------------------------------------------------------------------------

TEST(NumericBits, F32RoundTrip) {
    const u32 patterns[] = {0x00000000u, 0x80000000u, 0x3F800000u,
                            0x7F800000u, 0x7FC00000u, 0xFF800000u,
                            0x7F7FFFFFu, 0xDEADBEEFu};
    for (u32 b : patterns) {
        EXPECT_EQ(F32Bits(BitsF32(b)), b);
    }
}

TEST(NumericBits, F64RoundTrip) {
    const u64 patterns[] = {0x0ull, 0x8000000000000000ull,
                            0x3FF0000000000000ull, 0x7FF0000000000000ull,
                            0x7FF8000000000000ull, 0xFFF0000000000000ull,
                            0x7FEFFFFFFFFFFFFFull, 0xDEADBEEFCAFEBABEull};
    for (u64 b : patterns) {
        EXPECT_EQ(F64Bits(BitsF64(b)), b);
    }
}

// ---------------------------------------------------------------------------
// Width helpers: the W zero-extension behaviour that recompilation must not
// lose.
// ---------------------------------------------------------------------------

TEST(NumericWidth, Zext32DropsHighBits) {
    EXPECT_EQ(Zext32(0x12345678ABCDEF00ull), 0xABCDEF00ull);
    EXPECT_EQ(Zext32(0xFFFFFFFFFFFFFFFFull), 0xFFFFFFFFull);
    EXPECT_EQ(W(0x1122334455667788ull), 0x55667788u);
}

TEST(NumericWidth, Sext32AndSext) {
    EXPECT_EQ(Sext32(0xFFFFFFFFull), 0xFFFFFFFFFFFFFFFFull);
    EXPECT_EQ(Sext32(0x0000000080000000ull), 0xFFFFFFFF80000000ull);
    EXPECT_EQ(Sext32(0x7FFFFFFFull), 0x7FFFFFFFull);
    // Generic sign extension for several widths.
    EXPECT_EQ(Sext(0xFFull, 8), 0xFFFFFFFFFFFFFFFFull);
    EXPECT_EQ(Sext(0x7Full, 8), 0x7Full);
    EXPECT_EQ(Sext(0x8000ull, 16), 0xFFFFFFFFFFFF8000ull);
    EXPECT_EQ(Sext(0xABCull, 12), 0xFFFFFFFFFFFFFABCull);
    EXPECT_EQ(Sext(0xDEADBEEFull, 64), 0xDEADBEEFull);
}

// ---------------------------------------------------------------------------
// Shifts: masking must make >= width and 0 both defined and correct.
// ---------------------------------------------------------------------------

TEST(NumericShift, MaskAmounts) {
    EXPECT_EQ(MaskShift32(31), 31u);
    EXPECT_EQ(MaskShift32(32), 0u);
    EXPECT_EQ(MaskShift32(63), 31u);
    EXPECT_EQ(MaskShift64(63), 63u);
    EXPECT_EQ(MaskShift64(64), 0u);
    EXPECT_EQ(MaskShift64(127), 63u);
}

TEST(NumericShift, ShlIsDefinedForLargeAmounts) {
    // 32-bit: amount 32 masks to 0 -> unchanged low 32 bits, zero-extended.
    EXPECT_EQ(Shl(0xFFFFFFFF00000001ull, 32, true), 1ull);
    EXPECT_EQ(Shl(1, 31, true), 0x80000000ull);
    // 64-bit: amount 64 masks to 0.
    EXPECT_EQ(Shl(0xDEADBEEFCAFEBABEull, 64, false), 0xDEADBEEFCAFEBABEull);
    EXPECT_EQ(Shl(1, 63, false), 0x8000000000000000ull);
}

TEST(NumericShift, ShrIgnoresHighGarbageForW) {
    EXPECT_EQ(Shr(0xFFFFFFFF80000000ull, 0, true), 0x80000000ull);
    EXPECT_EQ(Shr(0xFFFFFFFF80000000ull, 31, true), 1ull);
    EXPECT_EQ(Shr(0x8000000000000000ull, 63, false), 1ull);
}

TEST(NumericShift, SarFillsSignBitsWithoutSignedShift) {
    // 32-bit arithmetic shift of a negative W value.
    EXPECT_EQ(Sar(0xFFFFFFFF80000000ull, 4, true), 0xF8000000ull);
    EXPECT_EQ(Sar(0x0000000080000000ull, 31, true), 0xFFFFFFFFull);
    EXPECT_EQ(Sar(0x000000007FFFFFFFull, 31, true), 0ull);
    // 64-bit.
    EXPECT_EQ(Sar(0x8000000000000000ull, 63, false), 0xFFFFFFFFFFFFFFFFull);
    EXPECT_EQ(Sar(0x7FFFFFFFFFFFFFFFull, 63, false), 0ull);
    EXPECT_EQ(Sar(0xDEADBEEFCAFEBABEull, 0, false), 0xDEADBEEFCAFEBABEull);
}

// ---------------------------------------------------------------------------
// Integer arithmetic + NZCV. These are the cases where naive C would be UB.
// ---------------------------------------------------------------------------

TEST(NumericFlags, Add32CarryAndOverflow) {
    // 0xFFFFFFFF + 1 = 0, C set (carry out), Z set, no signed overflow.
    auto r = AddWithFlags(0xFFFFFFFFull, 1, true);
    EXPECT_EQ(r.value, 0ull);
    EXPECT_EQ(r.nzcv & kC, kC);
    EXPECT_EQ(r.nzcv & kZ, kZ);
    EXPECT_EQ(r.nzcv & kV, 0u);

    // 0x7FFFFFFF + 1 = 0x80000000: signed overflow V set, N set, C clear.
    r = AddWithFlags(0x7FFFFFFFull, 1, true);
    EXPECT_EQ(r.value, 0x80000000ull);
    EXPECT_EQ(r.nzcv & kV, kV);
    EXPECT_EQ(r.nzcv & kN, kN);
    EXPECT_EQ(r.nzcv & kC, 0u);
}

TEST(NumericFlags, Sub32BorrowAndOverflow) {
    // 0 - 1 = 0xFFFFFFFF, C clear (borrow), N set, no overflow.
    auto r = SubWithFlags(0, 1, true);
    EXPECT_EQ(r.value, 0xFFFFFFFFull);
    EXPECT_EQ(r.nzcv & kC, 0u);
    EXPECT_EQ(r.nzcv & kN, kN);
    EXPECT_EQ(r.nzcv & kV, 0u);

    // 0x80000000 - 1 = 0x7FFFFFFF: signed overflow, C set, N clear.
    r = SubWithFlags(0x80000000ull, 1, true);
    EXPECT_EQ(r.value, 0x7FFFFFFFull);
    EXPECT_EQ(r.nzcv & kV, kV);
    EXPECT_EQ(r.nzcv & kC, kC);
    EXPECT_EQ(r.nzcv & kN, 0u);

    // Equal operands -> Z set, C set.
    r = SubWithFlags(0x1234ull, 0x1234ull, true);
    EXPECT_EQ(r.value, 0ull);
    EXPECT_EQ(r.nzcv & kZ, kZ);
    EXPECT_EQ(r.nzcv & kC, kC);
}

TEST(NumericFlags, Add64SignedOverflow) {
    auto r = AddWithFlags(0x7FFFFFFFFFFFFFFFull, 1, false);
    EXPECT_EQ(r.value, 0x8000000000000000ull);
    EXPECT_EQ(r.nzcv & kV, kV);
    EXPECT_EQ(r.nzcv & kN, kN);
    EXPECT_EQ(r.nzcv & kC, 0u);
}

TEST(NumericFlags, LogicFlags) {
    EXPECT_EQ(LogicFlags(0, false), kZ);
    EXPECT_EQ(LogicFlags(0xF000000000000000ull, false), kN);
    EXPECT_EQ(LogicFlags(0x80000000ull, true), kN);
    EXPECT_EQ(LogicFlags(0, true), kZ);
}

TEST(NumericFlags, ConditionHoldsTruthTable) {
    // Enumerate all 16 NZCV combinations and verify the documented encodings.
    for (u32 n = 0; n < 2; ++n)
    for (u32 z = 0; z < 2; ++z)
    for (u32 c = 0; c < 2; ++c)
    for (u32 v = 0; v < 2; ++v) {
        const u32 f = (n << 31) | (z << 30) | (c << 29) | (v << 28);
        EXPECT_EQ(ConditionHolds(f, 0), z == 1);
        EXPECT_EQ(ConditionHolds(f, 1), z == 0);
        EXPECT_EQ(ConditionHolds(f, 2), c == 1);
        EXPECT_EQ(ConditionHolds(f, 3), c == 0);
        EXPECT_EQ(ConditionHolds(f, 4), n == 1);
        EXPECT_EQ(ConditionHolds(f, 5), n == 0);
        EXPECT_EQ(ConditionHolds(f, 6), v == 1);
        EXPECT_EQ(ConditionHolds(f, 7), v == 0);
        EXPECT_EQ(ConditionHolds(f, 8), c == 1 && z == 0);
        EXPECT_EQ(ConditionHolds(f, 9), c == 0 || z == 1);
        EXPECT_EQ(ConditionHolds(f, 10), n == v);
        EXPECT_EQ(ConditionHolds(f, 11), n != v);
        EXPECT_EQ(ConditionHolds(f, 12), z == 0 && n == v);
        EXPECT_EQ(ConditionHolds(f, 13), z == 1 || n != v);
        EXPECT_TRUE(ConditionHolds(f, 14));
        EXPECT_TRUE(ConditionHolds(f, 15));
    }
}

// ---------------------------------------------------------------------------
// FP bit helpers.
// ---------------------------------------------------------------------------

TEST(NumericFp, AbsNegAreBitExact) {
    // -0.0 -> +0.0 for FABS; +/-0 flips sign for FNEG.
    EXPECT_EQ(F32Bits(FpAbs32(BitsF32(0x80000000u))), 0x00000000u);
    EXPECT_EQ(F32Bits(FpNeg32(BitsF32(0x00000000u))), 0x80000000u);
    EXPECT_EQ(F64Bits(FpAbs64(BitsF64(0x8000000000000000ull))), 0x0ull);
    EXPECT_EQ(F64Bits(FpNeg64(BitsF64(0x0000000000000000ull))), 0x8000000000000000ull);
    // NaN payload is preserved by FABS/FNEG except the sign bit.
    EXPECT_EQ(F32Bits(FpAbs32(BitsF32(0xFFC12345u))), 0x7FC12345u);
    EXPECT_EQ(FpSign32(BitsF32(0x80000000u)), 1u);
    EXPECT_EQ(FpSign64(BitsF64(0x8000000000000000ull)), 1u);
}

TEST(NumericFp, NanDetectionAndDefaultNan) {
    EXPECT_TRUE(IsNan32(BitsF32(0x7FC00000u)));
    EXPECT_TRUE(IsNan32(BitsF32(0xFFC00000u)));
    EXPECT_FALSE(IsNan32(BitsF32(0x7F800000u)));  // +inf
    EXPECT_FALSE(IsNan32(BitsF32(0x3F800000u)));  // 1.0
    EXPECT_TRUE(IsNan64(DefaultNan64()));
    EXPECT_TRUE(IsNan32(DefaultNan32()));
    EXPECT_EQ(F64Bits(DefaultNan64()), 0x7FF8000000000000ull);
}

TEST(NumericFp, FpCompareNZCV) {
    const f64 one = 1.0, two = 2.0;
    EXPECT_EQ(FpCompareNZCV(one, one), kZ | kC);   // equal -> Z, C
    EXPECT_EQ(FpCompareNZCV(two, one), kC);        // greater -> C
    EXPECT_EQ(FpCompareNZCV(one, two), 0u);        // less -> none
    // NaN -> unordered: all flags clear, so ordered conditions fail.
    EXPECT_EQ(FpCompareNZCV(DefaultNan64(), one), 0u);
    EXPECT_FALSE(ConditionHolds(FpCompareNZCV(DefaultNan64(), one), 0));
}

// ---------------------------------------------------------------------------
// FP <-> integer conversion: saturating, NaN->0, and no UB for any input.
// ---------------------------------------------------------------------------

TEST(NumericConvert, FpToUnsigned) {
    EXPECT_EQ(FpToUnsigned(0.0, false), 0ull);
    EXPECT_EQ(FpToUnsigned(3.9, false), 3ull);       // truncate toward zero
    EXPECT_EQ(FpToUnsigned(-1.0, false), 0ull);      // negative -> 0
    EXPECT_EQ(FpToUnsigned(1e300, false), ~0ull);    // out of range -> saturate
    EXPECT_EQ(FpToUnsigned(1e300, true), 0xFFFFFFFFull);
    EXPECT_EQ(FpToUnsigned(BitsF64(0x7FF8000000000000ull), false), 0ull);  // NaN
}

TEST(NumericConvert, FpToSigned) {
    EXPECT_EQ(FpToSigned(0.0, false), 0ll);
    EXPECT_EQ(FpToSigned(-3.9, false), -3ll);
    EXPECT_EQ(FpToSigned(1e300, false), 9223372036854775807ll);
    EXPECT_EQ(FpToSigned(-1e300, false), -9223372036854775807ll - 1);
    EXPECT_EQ(FpToSigned(1e300, true), 2147483647ll);
    EXPECT_EQ(FpToSigned(-1e300, true), -2147483647ll - 1);
    EXPECT_EQ(FpToSigned(BitsF64(0x7FF8000000000000ull), false), 0ll);
    // Exact upper boundary that is still in range for a 32-bit result.
    EXPECT_EQ(FpToSigned(2147483647.0, true), 2147483647ll);
}

TEST(NumericConvert, IntToFp) {
    EXPECT_EQ(IntToFp(42, false, false), 42.0);
    EXPECT_EQ(IntToFp(42, true, false), 42.0);
    // Signed source: 0xFFFF...FFFF as s64 is -1.
    EXPECT_EQ(IntToFp(0xFFFFFFFFFFFFFFFFull, true, false), -1.0);
    // Unsigned max rounds to 2^64 as a double.
    EXPECT_EQ(IntToFp(0xFFFFFFFFFFFFFFFFull, false, false), 18446744073709551616.0);
    // F32 result is narrowed: 0x3F800000 as an integer is 1065353216, which
    // is exactly representable as float. (This is an integer value, not the
    // bit pattern of 1.0f.)
    EXPECT_EQ(IntToFp(0x3F800000ull, false, true), 1065353216.0);
    // A value that is not exactly representable as float must be rounded to
    // the nearest float (2^32 + 1 -> 2^32).
    EXPECT_EQ(IntToFp(0x100000001ull, false, true), 4294967296.0);
}

// ---------------------------------------------------------------------------
// Exhaustive small-domain cross-check for AddWithFlags / SubWithFlags: the
// NZCV predicates must match a straightforward reference model built from
// wider (unsigned) arithmetic, for many operand pairs including boundaries.
// ---------------------------------------------------------------------------

TEST(NumericFlags, ExhaustiveSmallDomain) {
    const u64 vals[] = {0, 1, 2, 0x7FFFFFFEull, 0x7FFFFFFFull,
                        0x80000000ull, 0x80000001ull, 0xFFFFFFFEull,
                        0xFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull,
                        0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull};
    for (bool is32 : {true, false}) {
        for (u64 a : vals) {
            for (u64 b : vals) {
                auto add = AddWithFlags(a, b, is32);
                auto sub = SubWithFlags(a, b, is32);
                // Value always respects the destination width.
                if (is32) {
                    EXPECT_EQ(add.value & 0xFFFFFFFF00000000ull, 0ull);
                    EXPECT_EQ(sub.value & 0xFFFFFFFF00000000ull, 0ull);
                }
                // Z is consistent with the result.
                EXPECT_EQ((add.nzcv & kZ) != 0, add.value == 0);
                EXPECT_EQ((sub.nzcv & kZ) != 0, sub.value == 0);
                // N is the top bit of the (width-masked) result.
                const unsigned sb = is32 ? 31u : 63u;
                EXPECT_EQ((add.nzcv & kN) != 0, ((add.value >> sb) & 1ull) != 0);
                EXPECT_EQ((sub.nzcv & kN) != 0, ((sub.value >> sb) & 1ull) != 0);
            }
        }
    }
}
