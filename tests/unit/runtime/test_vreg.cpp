// SIMD phase 1: AArch64 vector register alias semantics.
//
// Pins down the rules the unified sr_vreg bank must enforce:
//   - scalar S write: low 32 bits written, upper 96 bits zeroed;
//   - scalar D write: low 64 bits written, upper 64 bits zeroed;
//   - a later S write after a D write also zeroes bytes [8..15]
//     (no stale upper half);
//   - writes to one register never touch another register;
//   - lane writes (INS) preserve every other byte of the same register;
//   - get/set round-trip for S and D.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "switchrecomp/runtime/vreg.h"

namespace {

bool AllZero(const sr_vreg& r, int from, int count) {
    for (int i = 0; i < count; ++i) {
        if (r.bytes[from + i] != 0) return false;
    }
    return true;
}

} // namespace

TEST(VReg, SetSThenUpper96Zero) {
    sr_vreg r;
    std::memset(r.bytes, 0xAB, sizeof(r.bytes));  // prefill: prove the zeroing
    sr_v_set_s(&r, 1.5f);
    EXPECT_FLOAT_EQ(sr_v_get_s(&r), 1.5f);
    // [127:32] — bytes 4..15 — must all read zero.
    EXPECT_TRUE(AllZero(r, 4, 12));
}

TEST(VReg, SetDThenUpper64Zero) {
    sr_vreg r;
    std::memset(r.bytes, 0xAB, sizeof(r.bytes));
    sr_v_set_d(&r, 1234.5);
    EXPECT_DOUBLE_EQ(sr_v_get_d(&r), 1234.5);
    // [127:64] — bytes 8..15 — must all read zero.
    EXPECT_TRUE(AllZero(r, 8, 8));
}

TEST(VReg, WriteDThenS) {
    sr_vreg r;
    // Pi: bytes 4..7 (the upper 32 bits of its 8-byte payload) are non-zero.
    sr_v_set_d(&r, 3.14159265358979323846);
    ASSERT_FALSE(AllZero(r, 4, 4));  // sanity: upper payload half is populated
    ASSERT_TRUE(AllZero(r, 8, 8));   // set_d already zeroed bytes 8..15
    sr_v_set_s(&r, 1.0f);
    EXPECT_FLOAT_EQ(sr_v_get_s(&r), 1.0f);
    // The S write must have re-zeroed bytes 4..15, including the stale
    // upper payload half (bytes 4..7) left by the D write.
    EXPECT_TRUE(AllZero(r, 4, 12));
    // Bit-level view of the 8-byte D read: low 32 bits are the f32 1.0 pattern
    // and the upper 32 bits are zero (the value is a tiny denormal double, not
    // 1.0 — aliasing is about bits, not numeric promotion).
    uint64_t dbits;
    memcpy(&dbits, r.bytes, 8);
    EXPECT_EQ(dbits & 0xFFFFFFFFu, 0x3F800000u);
    EXPECT_EQ(dbits >> 32, 0u);
}

TEST(VReg, NoCrossRegister) {
    sr_vreg v[2];
    std::memset(v[0].bytes, 0xCD, sizeof(v[0].bytes));
    std::memset(v[1].bytes, 0xCD, sizeof(v[1].bytes));
    sr_v_set_s(&v[1], 2.25f);
    // v[0] must be byte-for-byte untouched.
    for (uint8_t b : v[0].bytes) EXPECT_EQ(b, 0xCD);
    EXPECT_FLOAT_EQ(sr_v_get_s(&v[1]), 2.25f);
}

TEST(VReg, LaneSetKeepsOthers) {
    sr_vreg r;
    std::memset(r.bytes, 0x77, sizeof(r.bytes));
    sr_v_set_lane_s(&r, 2, 4.5f);
    EXPECT_FLOAT_EQ(sr_v_get_lane_s(&r, 2), 4.5f);
    // Every byte outside lane 2 (bytes 8..11) keeps the preset value.
    for (int i = 0; i < 16; ++i) {
        if (i >= 8 && i < 12) continue;
        EXPECT_EQ(r.bytes[i], 0x77) << "byte " << i << " changed by lane write";
    }
}

TEST(VReg, RoundTripS) {
    sr_vreg r;
    const float values[] = {0.0f, -0.0f, 1.0f, -2.5f, 1e-30f, 3.4028235e38f};
    for (float v : values) {
        sr_v_set_s(&r, v);
        EXPECT_EQ(sr_v_get_s(&r), v);
    }
}

TEST(VReg, RoundTripD) {
    sr_vreg r;
    const double values[] = {0.0, -0.0, 1.0, -2.5, 2.2250738585072014e-308,
                             1.7976931348623157e308};
    for (double v : values) {
        sr_v_set_d(&r, v);
        EXPECT_EQ(sr_v_get_d(&r), v);
    }
}
