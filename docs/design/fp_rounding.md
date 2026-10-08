# RC-FP-005 设计：FSQRT 与 FRINT 舍入族

状态：已实现
日期：2026-10-08
作者：arm64-recompiler

## 1. 范围

ARMv8-A 标量浮点 **1-source**（单源）族中的两条类：

- `FSQRT Sd, Sn`：平方根；
- `FRINT* Sd, Sn` 舍入到整数（结果仍为浮点值），共五条：

| 指令 | 舍入方向 |
|------|----------|
| FRINTN | ties to even（最近偶数；忽略 FPCR.RMode） |
| FRINTP | toward +∞ |
| FRINTM | toward −∞ |
| FRINTZ | toward 0 |
| FRINTA | ties away from zero（最近且等距时远离 0；忽略 FPCR.RMode） |

S（单精度，type=00）与 D（双精度，type=01）两种形式。H（半精度，type=11）
本任务不实现。

## 2. 编码

字段布局（QEMU `disas_fp_1src` 与 LLVM `BaseSingleOperandFPData` 一致）：

```
31 30 29-24    23:22  21  20:15(6-bit opcode)  14:10  9:5  4:0
M  0  011110   type    1   opcode               10000   Rn    Rd
```

- 固定 bits31-24 = `00011110`（0x1E）；bit21 = 1；bits14-10 = `10000`；
- type 位 bits23:22：S=00、D=01、H=11；
- 6-bit opcode（bits20:15）：

| 指令 | opcode | S 基址 | D 基址 |
|------|--------|--------|--------|
| FMOV（寄存器） | 0x00 | 0x1E204000 | 0x1E604000 |
| FABS | 0x01 | 0x1E20C000 | 0x1E60C000 |
| FNEG | 0x02 | 0x1E214000 | 0x1E614000 |
| FSQRT | 0x03 | 0x1E21C000 | 0x1E61C000 |
| FRINTN | 0x08 | 0x1E244000 | 0x1E644000 |
| FRINTP | 0x09 | 0x1E24C000 | 0x1E64C000 |
| FRINTM | 0x0A | 0x1E254000 | 0x1E654000 |
| FRINTZ | 0x0B | 0x1E25C000 | 0x1E65C000 |
| FRINTA | 0x0C | 0x1E264000 | 0x1E664000 |

同组掩码 `0xFFFFFC00`（固定 bits31-10，Rn/Rd 自由）。各 value 互异，与
FCVT（0x1E22C000 / 0x1E624000）及 FABS/FNEG 不抢占。

编码已由四个独立来源交叉核实：ARM ARM ISA XML（`FSQRT/FRINTN/...` enc1）、
QEMU 9.0 `disas_fp_1src`（translate-a64.c）、LLVM 18
`BaseSingleOperandFPData` 各 defm、VIXL `FPDataProcessing1SourceFixed`
（=0x1E204000）常量。

## 3. IR 设计

公共头仅做枚举追加，不改任何既有签名。`Opcode` 末尾追加：
`Sqrt, FrintN, FrintP, FrintM, FrintZ, FrintA`；JSON 字符串同名
（`"Sqrt"`、`"FrintN"` …）。

所有指令统一为两操作数形式 `{dst 寄存器常量, src}`，指令类型即精度
（F32/F64），与 FABS/FNEG 一致。IRBuilder 增加 `CreateSqrt` 与
`CreateFrintN/P/M/Z/A`。

译码：新增 `DecodeFSqrt`；五条 FRINT 共用模板 `DecodeFrint<Opcode>`
（表项注册各自实例化）。Rn（bits9:5）、Rd（bits4:0）、isd（bit22）
决定 `cache.FReg()` / `cache.DReg()`。

## 4. 生成 C 的形式与 host libm 映射

全部降级为对 host libm 的单参函数调用（生成代码顶部已引入 `math.h`）：

| ARM 指令 | float（S） | double（D） | 生成示例（S） |
|----------|-----------|-------------|---------------|
| FSQRT | `sqrtf` | `sqrt` | `ctx->f[0] = sqrtf(ctx->f[1]);` |
| FRINTN | `rintf` | `rint` | `ctx->f[0] = rintf(ctx->f[1]);` |
| FRINTP | `ceilf` | `ceil` | `ctx->f[0] = ceilf(ctx->f[1]);` |
| FRINTM | `floorf` | `floor` | `ctx->f[0] = floorf(ctx->f[1]);` |
| FRINTZ | `truncf` | `trunc` | `ctx->f[0] = truncf(ctx->f[1]);` |
| FRINTA | `roundf` | `round` | `ctx->f[0] = roundf(ctx->f[1]);` |

函数选择由目标寄存器类型（F32/F64）决定，与既有 FABS（fabsf/fabs）路径同构。

### 语义等价性与限制

- `ceil/ceilf`、`floor/floorf`、`trunc/truncf` 与 FRINTP/FRINTM/FRINTZ
  语义一一对应；`round/roundf` 的舍入规则正是 "ties away from zero"，
  与 FRINTA 完全一致。NaN、±Inf、±0.0 均原样透传。
- **FRINTN → rint/rintf 的前提**：rint 系列遵循**当前 host 舍入模式**，
  而 host 默认模式为 round-to-nearest ties-to-even（FE_TONEAREST），恰为
  FRINTN 的固定语义。本项目生成代码不修改舍入模式，因此默认环境下等价；
  若 host 进程曾通过 `fesetround` 改变舍入模式，FRINTN 将随之偏离——
  精确做法（如 `nearbyint` 在非默认模式下同样受影响，C 标准库无"强制
  ties-to-even 且不触发 INEXACT"的单函数）需显式保存/恢复模式或手写
  舍入，留待需要时再加强。
- FRINTN/FRINTA 在 ARM 上**不产生 INEXACT 异常**（显式舍入指令）；
  rint/round 等 libm 调用的浮点异常行为在不同 libc/编译器组合下不保证
  一致。当前 recompiler 不建模 FPSR 累积状态位，故无功能影响。
- MinGW（UCRT）工具链提供上述全部函数（含 `round/roundf`）；
  GeneratedCSyntaxValid 的 gcc `-fsyntax-only` 覆盖所有六类 S/D 生成形式。

## 5. 边界情况

- **NaN**：sqrt(NaN)=NaN，各舍入函数对 NaN 返回 NaN；与 ARM 一致。
- **负值**：sqrt(负有限值) 按 host 返回 NaN（ARM 同样返回 NaN 并置
  Invalid Operation，FPSR 未建模）。
- **±Inf**：sqrt(+Inf)=+Inf；舍入函数对 ±Inf 原样返回。
- **±0.0**：sqrt(+0)=+0、sqrt(−0)=−0（host IEEE 实现一致）。
- **超大数/非规格化数**：舍入函数直接返回原值；sqrt 依赖 host IEEE
  默认 FTZ 关闭。
- H（半精度）与 FRINTX/FRINTI（受 FPCR 控制并产生 Invalid 的变体）、
  FRINT32/64X/Z（定宽变体）不在本任务范围。

## 6. 迁移影响

| 文件 | 变更 |
|------|------|
| include/switchrecomp/ir/ir.h | 枚举追加 `Sqrt/FrintN/FrintP/FrintM/FrintZ/FrintA`；IRBuilder 追加 6 个 Create 声明 |
| src/ir/ir.cpp | opcode ↔ 字符串映射追加 6 项；6 个 Builder 实现 |
| src/translator/translator.cpp | 新增 DecodeFSqrt、模板 DecodeFrint；译码表 12 项（6 指令 × S/D） |
| src/codegen/codegen.cpp | 六类 opcode 的 libm 单参调用降级 |
| tests/fixtures/translator/golden/{sqrt,frintn,frintp,frintm,frintz,frinta}_s.json | 新增 6 个 S golden |
| tests/integration/translator/test_golden.cpp | kSamples 注册 6 个 S 样本 |
| tests/unit/translator/test_decode.cpp | 新增 12 个 S/D decode 用例 + 2 个字段用例 |
| tests/unit/codegen/test_codegen.cpp | 新增 8 个 lowering 字符串断言；GeneratedCSyntaxValid 覆盖六类 S/D |
| tests/integration/test_e2e_fp_round.cpp | 新增 FSQRT、FRINT 四变体两个端到端用例（UBSan 编译，无 g++ 时 GTEST_SKIP） |
| tests/CMakeLists.txt | 注册新 e2e 文件 |

公共头既有签名：无变更。
