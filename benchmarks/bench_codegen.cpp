// BT-PERF-001: codegen (IR -> C) throughput benchmark.
#include <benchmark/benchmark.h>

#include "switchrecomp/codegen/codegen.h"
#include "switchrecomp/ir/ir.h"

static void GenerateCModule(benchmark::State& state) {
    // Build a small IR module once (outside the timed loop): one function,
    // one basic block with 10 Add instructions followed by a Return.
    sr::ir::IRBuilder builder;
    sr::ir::Module mod;

    sr::ir::Function* fn = mod.AddFunction();
    fn->set_guest_address(0x400000);
    sr::ir::BasicBlock* bb = fn->AddBlock();

    sr::ir::Constant* zero = builder.CreateConstant(0, sr::ir::Type::U64());
    sr::ir::Constant* one = builder.CreateConstant(1, sr::ir::Type::U64());
    for (int i = 0; i < 10; i++) {
        bb->Append(builder.CreateAdd(zero, one));
    }
    bb->Append(builder.CreateReturn());

    for (auto _ : state) {
        auto result = sr::codegen::GenerateC(mod, {});
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations() * 10);
}
BENCHMARK(GenerateCModule);

BENCHMARK_MAIN();
