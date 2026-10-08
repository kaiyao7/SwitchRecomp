// BT-PERF-001: translator throughput benchmark.
#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "switchrecomp/translator/translator.h"

// Construct 100 `add x0, x0, #1` instructions (4 bytes each).
// Encoding: 0x91000400 -> little-endian bytes 00 04 00 91.
static void TranslateBlock(benchmark::State& state) {
    std::vector<uint8_t> code;
    code.reserve(100 * 4);
    for (int i = 0; i < 100; i++) {
        code.push_back(0x00);
        code.push_back(0x04);
        code.push_back(0x00);
        code.push_back(0x91); // add x0, x0, #1
    }

    for (auto _ : state) {
        auto result = sr::translator::Translate(
            sr::common::span<const uint8_t>(code.data(), code.size()), {});
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations() * 100);
}
BENCHMARK(TranslateBlock);

BENCHMARK_MAIN();
