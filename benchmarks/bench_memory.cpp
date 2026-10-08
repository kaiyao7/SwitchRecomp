// BT-PERF-001: memory manager guest->host lookup latency benchmark.
#include <benchmark/benchmark.h>

#include "switchrecomp/platform/memory.h"
// MemoryManager now lives in the public runtime header.
#include "switchrecomp/runtime/memory_manager.h"

static void GuestToHostLookup(benchmark::State& state) {
    sr::runtime::MemoryManager mm;
    mm.MapGuestMemory(0x100000, 4096, sr::platform::MemProtect::ReadWrite);

    for (auto _ : state) {
        void* p = mm.GuestToHost(0x100000, 4, false);
        benchmark::DoNotOptimize(p);
    }
}
BENCHMARK(GuestToHostLookup);

BENCHMARK_MAIN();
