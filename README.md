# SwitchRecomp

A research framework for **static recompilation of Nintendo Switch (AArch64) binaries to
native PC executables**, targeting legally owned software, published research material, and
self-authored test programs.

**Status: early-stage research skeleton. It does not run any commercial game.**
Do not expect to point it at a game dump and play. See
[Roadmap](#roadmap--where-you-can-help) for what exists and what is missing.

## What it does today

```
guest AArch64 machine code → Translate (decoder) → SSA-like IR → C code generation
                            → host C/C++ compiler → native executable → automated tests
```

- **Translator**: AArch64 user-space instruction decoding (integer core, FP32/FP64
  scalar families, a growing AdvSimd/SIMD subset) into a typed, JSON-serializable IR.
- **Code generation**: IR → portable C11; the generated code is compiled with a real
  host compiler (g++) and executed in end-to-end tests — no simulation, no skipped
  assertions.
- **Runtime**: HLE service registry (`sr_hle_register` / `sr_hle_invoke` C ABI),
  ELF64/AArch64 and (simplified) NRO loaders, guest page-table memory manager with
  W^X semantics, minimal thread/synchronization SVCs.
- **GPU (minimal)**: Maxwell FIFO command word parsing, command processor dispatch,
  Vulkan backend with a working compute-pipeline proof of path.
- **Numeric substrate** (`common/numeric.h`, header-only): bit-exact, UB-free AArch64
  width/shift/flag/FP/conversion semantics, with a throughput benchmark
  (`bench_numeric`) backing the "inlines to zero overhead" claim.
- **Tests**: 295 unit / golden-fixture / end-to-end tests; clean rebuilds are
  repeatable with all tests passing.

## What it deliberately is not

- Not a JIT emulator. The point of the project is to remove the JIT layer
  (CPU), replace OS services with high-level native implementations (HLE), and
  eventually rewrite GPU command streams into a modern graphics API renderer.
- Not a way to play pirated games. This repository contains **no Nintendo
  copyrighted material**: no keys, no firmware, no game data, no shader binaries
  extracted from games. It only processes binaries you are legally entitled to
  use. Do not open issues or PRs containing copyrighted material.

## Why this project exists: the numeric substrate

The purpose of this repository is **the lowest-level numeric optimization for
AArch64 → C static recompilation**. Translating instructions is the easy part;
making every guest arithmetic / flag / FP / conversion operation bit-exact,
undefined-behaviour-free, and still optimizable by the host compiler is the
hard part. Plain C expressions are UB for arbitrary guest bit patterns
(signed overflow, shift ≥ width, out-of-range float→int), and a host optimizer
is allowed to exploit that UB — so a recompiler must first pin the semantics
down in an explicit, inlineable layer.

`include/switchrecomp/common/numeric.h` is that layer. It is header-only, all
`static inline`, and folds to one or two host instructions at `-O2`. The full
feasibility map — what is built and tested, what is designed but not yet built,
and what needs external conditions — is in
[`docs/design/numeric_semantics.md`](docs/design/numeric_semantics.md), with
throughput numbers in `benchmarks/bench_numeric.cpp`. The numeric layer is the
foundation; IR-level optimizations (constant folding, flag merging, fastmem)
sit on top of it.

## Architecture

Strict one-way module layering (enforced by `scripts/check_deps.py`):

```
common ← platform ← ir ← translator / codegen / runtime ← gpu
```

- `include/switchrecomp/**`, `src/**` — framework code
- `tests/unit/**`, `tests/integration/**` — GoogleTest suites
- `tests/fixtures/**` — golden IR fixtures and embedded shader sources
- `docs/design/**` — design documents (numeric semantics + feasibility map,
  SIMD architecture, FP compare/flags, FP rounding, instruction coverage notes);
  kept in sync with the code
- `benchmarks/` — translator / codegen / memory-manager / numeric-substrate
  micro-benchmarks
- `.github/workflows/ci.yml` — CI

Design rule of thumb: explicit data structures and interfaces over implicit
global state; every functional change ships with unit + integration tests;
performance claims must be backed by benchmark numbers.

## Build & test

Requirements: a C++20 host toolchain (developed on MSYS2 UCRT64 GCC 16 + Ninja),
CMake, and optionally Vulkan SDK / a Vulkan-capable GPU for the GPU tests.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

End-to-end tests compile generated C with a real compiler at test time; they
`GTEST_SKIP` gracefully when no compiler is available.

## Roadmap — where you can help

| Phase | Scope | State |
|-------|-------|-------|
| P1 | Pipeline closure: fixtures → decoder → IR → codegen → native → tests | **done** |
| P2 | SIMD stage 3/4 (LD1/ST1, vector FSQRT/FRINT/FCMP), integer vector I32x4, instruction-coverage gap list | partially done, open |
| P3 | Real-compiler fixtures (clang -target aarch64): function pointers, jump tables, control-flow recovery design | open |
| P4 | Real three-segment NRO/NSO loading, dynamic overlays, indirect-branch runtime fallback | open |
| P5 | Minimal homebrew HLE set (subset of vi/nvdrv/hid/fs) + minimal 3D GPU path: run a libnx hello-world / spinning triangle | open |
| P6+ | Maxwell shader translation (Maxwell ISA → SPIR-V), simple 2D commercial titles, long-term: large-scale titles | open, hard |

Good first contributions:

- Instruction coverage: pick an unimplemented AArch64 user-space instruction,
  add decoder + golden fixture + codegen + an end-to-end test (see existing
  tests for the pattern).
- Control-flow recovery: design notes and prototypes for jump-table /
  vtable target recovery are welcome before code.
- GPU: extend the command processor method coverage; the compute path is the
  reference implementation.

Please keep the layering rule, keep tests green (`ctest` must pass on a clean
build), and update `docs/design/` when you change architecture.

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).

This choice keeps derivatives open so community work stays community-owned.
The project name and code are original; "Switch" and any game titles mentioned
in documentation are trademarks of their respective owners and are referenced
for interoperability research only.
