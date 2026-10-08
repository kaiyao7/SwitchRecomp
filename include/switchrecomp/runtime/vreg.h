// SIMD phase 1: unified AArch64 SIMD&FP vector register type and accessors.
//
// All 32 physical 128-bit V registers share one type, |sr_vreg|. The B/H/S/D
// scalar views and the per-lane views are aliases of the same bytes:
//   - scalar writes (set_b/h/s/d) write the low field and ZERO the upper
//     unused bits, as required by the AArch64 SIMD&FP register file
//     ("unused upper bits ... are set to zero on writes");
//   - lane writes (set_lane_*) implement INS semantics: only the named lane
//     changes, every other byte is preserved.
//
// Pure C11 / C++11 dual-compatible. The fixed-size memcpy/memset forms are
// strict-aliasing safe and fold to a single load/store/zero at -O1 and above
// on GCC/Clang/MSVC. Codegen emits calls to these functions on both the
// C++ (runtime.h) and the fallback pure-C struct paths.
#pragma once

#include <stdint.h>
#include <string.h>
#if !defined(__cplusplus)
// In C, alignas is provided as a macro expanding to _Alignas. In C++ it is a
// language keyword, so the header is not needed (and harmless) there.
#include <stdalign.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// One 128-bit vector register, 16-byte aligned (matches the AArch64 V reg).
typedef struct sr_vreg {
    alignas(16) uint8_t bytes[16];
} sr_vreg;

/* ------------------------------------------------------------------ */
/* Scalar reads (fixed-size memcpy: strict-aliasing safe loads).       */
/* ------------------------------------------------------------------ */

static inline float sr_v_get_s(const sr_vreg* r) {
    float v;
    memcpy(&v, r->bytes, 4);
    return v;
}

static inline double sr_v_get_d(const sr_vreg* r) {
    double v;
    memcpy(&v, r->bytes, 8);
    return v;
}

static inline uint16_t sr_v_get_h(const sr_vreg* r) {
    uint16_t v;
    memcpy(&v, r->bytes, 2);
    return v;
}

static inline uint8_t sr_v_get_b(const sr_vreg* r) {
    return r->bytes[0];
}

/* ------------------------------------------------------------------ */
/* Scalar writes: write the named width, zero all upper bits.          */
/* ------------------------------------------------------------------ */

// Write Sn: [31:0] = v, [127:32] = 0 (upper 96 bits).
static inline void sr_v_set_s(sr_vreg* r, float v) {
    memcpy(r->bytes, &v, 4);
    memset(r->bytes + 4, 0, 12);
}

// Write Dn: [63:0] = v, [127:64] = 0 (upper 64 bits).
static inline void sr_v_set_d(sr_vreg* r, double v) {
    memcpy(r->bytes, &v, 8);
    memset(r->bytes + 8, 0, 8);
}

// Write Hn: [15:0] = v, [127:16] = 0 (upper 112 bits). The float16 view is
// added in the FP16 phase; for now H is a raw 16-bit container.
static inline void sr_v_set_h(sr_vreg* r, uint16_t v) {
    memcpy(r->bytes, &v, 2);
    memset(r->bytes + 2, 0, 14);
}

// Write Bn: [7:0] = v, [127:8] = 0 (upper 120 bits).
static inline void sr_v_set_b(sr_vreg* r, uint8_t v) {
    r->bytes[0] = v;
    memset(r->bytes + 1, 0, 15);
}

/* ------------------------------------------------------------------ */
/* Lane views (INS semantics: a lane write leaves all other bytes).    */
/* lane 0 occupies the lowest address (little-endian, Switch is LE).   */
/* ------------------------------------------------------------------ */

static inline float sr_v_get_lane_s(const sr_vreg* r, int lane) {
    float v;
    memcpy(&v, r->bytes + lane * 4, 4);
    return v;
}

static inline void sr_v_set_lane_s(sr_vreg* r, int lane, float v) {
    memcpy(r->bytes + lane * 4, &v, 4);
}

static inline double sr_v_get_lane_d(const sr_vreg* r, int lane) {
    double v;
    memcpy(&v, r->bytes + lane * 8, 8);
    return v;
}

static inline void sr_v_set_lane_d(sr_vreg* r, int lane, double v) {
    memcpy(r->bytes + lane * 8, &v, 8);
}

static inline uint16_t sr_v_get_lane_h(const sr_vreg* r, int lane) {
    uint16_t v;
    memcpy(&v, r->bytes + lane * 2, 2);
    return v;
}

static inline void sr_v_set_lane_h(sr_vreg* r, int lane, uint16_t v) {
    memcpy(r->bytes + lane * 2, &v, 2);
}

static inline uint8_t sr_v_get_lane_b(const sr_vreg* r, int lane) {
    return r->bytes[lane];
}

static inline void sr_v_set_lane_b(sr_vreg* r, int lane, uint8_t v) {
    r->bytes[lane] = v;
}

/* ------------------------------------------------------------------ */
/* Full 128-bit reads/writes (MOVI, vector load/store, Q-form results)*/
/* ------------------------------------------------------------------ */

static inline void sr_v_load16(sr_vreg* r, const void* src) {
    memcpy(r->bytes, src, 16);
}

static inline void sr_v_store16(const sr_vreg* r, void* dst) {
    memcpy(dst, r->bytes, 16);
}

#ifdef __cplusplus
} // extern "C"
#endif
