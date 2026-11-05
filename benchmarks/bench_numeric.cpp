// Throughput benchmark for the AArch64 -> C numeric substrate.
//
// The project's central claim is that bit-exact, UB-free numeric semantics can
// be *free*: the helpers in common/numeric.h must inline to one or two host
// instructions, so the host optimizer removes the emulation layer. This
// benchmark measures the per-operation cost of the hot helpers over a large
// stream of inputs, mixing in the values that would be UB under naive C.
//
// Interpretation: compare against the hand-written "naive" expression in the
// same harness. A near-zero difference is the feasibility proof; a large gap
// means the helper must be simplified before codegen adopts it.
#include <benchmark/benchmark.h>

#include "switchrecomp/common/numeric.h"

#include <cstdint>

using namespace sr::numeric;
using sr::common::u64;
using sr::common::f64;

namespace {

// Deterministic pseudo-random stream without RNG library overhead.
u64 Next(u64& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

static void BM_AddWithFlags(benchmark::State& state) {
    u64 s = 0x123456789ABCDEF0ull;
    u64 acc = 0;
    for (auto _ : state) {
        u64 a = Next(s), b = Next(s);
        auto r = AddWithFlags(a, b, false);
        acc += r.value ^ r.nzcv;
    }
    benchmark::DoNotOptimize(acc);
    benchmark::ClobberMemory();
}
BENCHMARK(BM_AddWithFlags);

static void BM_AddNaive(benchmark::State& state) {
    u64 s = 0x123456789ABCDEF0ull;
    u64 acc = 0;
    for (auto _ : state) {
        u64 a = Next(s), b = Next(s);
        acc += a + b;  // reference: raw host add, no flags
    }
    benchmark::DoNotOptimize(acc);
    benchmark::ClobberMemory();
}
BENCHMARK(BM_AddNaive);

static void BM_ShlMasked(benchmark::State& state) {
    u64 s = 0xFEDCBA0987654321ull;
    u64 acc = 0;
    for (auto _ : state) {
        u64 a = Next(s), b = Next(s);
        // Includes amounts >= 64 that would be UB in plain C.
        acc += Shl(a, b, false);
    }
    benchmark::DoNotOptimize(acc);
    benchmark::ClobberMemory();
}
BENCHMARK(BM_ShlMasked);

static void BM_FpToSigned(benchmark::State& state) {
    u64 s = 0x0BADF00DDEADBEEFull;
    u64 acc = 0;
    for (auto _ : state) {
        // Mix finite, huge, and NaN bit patterns.
        f64 v = BitsF64(Next(s));
        acc += static_cast<u64>(FpToSigned(v, false));
    }
    benchmark::DoNotOptimize(acc);
    benchmark::ClobberMemory();
}
BENCHMARK(BM_FpToSigned);

static void BM_ConditionHolds(benchmark::State& state) {
    u64 s = 0xABCDEF0123456789ull;
    u64 acc = 0;
    for (auto _ : state) {
        u32 nzcv = static_cast<u32>(Next(s));
        u8 cond = static_cast<u8>(Next(s) & 0xF);
        acc += ConditionHolds(nzcv, cond) ? 1u : 0u;
    }
    benchmark::DoNotOptimize(acc);
    benchmark::ClobberMemory();
}
BENCHMARK(BM_ConditionHolds);

} // namespace

BENCHMARK_MAIN();
