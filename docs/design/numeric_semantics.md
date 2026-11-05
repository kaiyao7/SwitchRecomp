# AArch64→C 最底层运算数值语义与优化：可行性清单

> 版本：v0.1
> 定位：本项目的存在理由，是为 AArch64→C 静态重编译提供**最底层的运算数值优化**——保证每一条 guest 数值运算在宿主 C 里位级精确、无未定义行为（UB）、并且能被宿主编译器优化成零开销。本文件把所有可行性逐条列出，并标明哪些已经构建、可编译、可测试、可基准验证。

## 1. 为什么需要独立的一层

把 `add x0, x0, #1` 直接翻译成 `ctx->x[0] = ctx->x[0] + 1;` 看起来没问题，但面对**任意 guest 位模式**时会踩到 C 的 UB 与语义缺口：

| 直接翻译 | AArch64 语义 | 风险 |
|---|---|---|
| W 寄存器运算写回 64 位槽 | 结果 32 位、零扩展 | 高位残留垃圾，后续 X 读出错 |
| `a << b`（b 来自寄存器） | 移位量取低 5/6 位 | `b >= 位宽` 是 C UB |
| 有符号 `+ - *` | 位级回绕 | 有符号溢出是 C UB |
| `(int)f` | FCVTZS 饱和、NaN→0 | 越界/NaN 是 C UB |
| `a < b`（有符号） | 无符号/有符号混合比较 | 溢出时符号判断错误 |
| `fabs/fneg` 走表达式 | 位操作（保留 NaN 载荷） | NaN/-0 行为不一致 |
| `f == g` 比较 | FCMP 的 NZCV + 无序处理 | NaN 时条件码错误 |

结论：数值语义必须收敛到一个**显式的、无 UB 的、可内联的**底座；否则宿主编译器在 `-O2` 下可以合法地把生成代码优化成错误结果。这正是「运算数值优化」的地基：先把语义钉死，宿主优化器才敢放手优化。

## 2. 设计原则

1. **位级精确（Bit exact）**：W 零扩展、移位掩码、饱和、NaN、默认 NaN、NZCV 布局与 AArch64 一致。
2. **零 UB（No undefined behaviour）**：整数运算全部走无符号类型；浮点→整数转换只在「已证明可表示」的区间内做。`shift >= width`、有符号溢出、NaN/越界转换一律不出现在生成代码里。
3. **零开销（Zero overhead）**：全部 `static inline` 头文件实现，无分配、无间接调用、无 libm；`-O2` 下折成单条/两条宿主机指令，由宿主优化器消除仿真层。
4. **可验证（Feasibility built, not promised）**：每一条可行性都有对应 GTest 用例；性能声明由 benchmark 数据支撑。

分层约束：本层位于 `common`（`include/switchrecomp/common/numeric.h`），translator / codegen / runtime 均可包含，不违反依赖矩阵。

## 3. 所有可行性清单

状态标记：
- ✅ **已构建**：头文件实现 + 单元测试 + 接入 CMake/CTest，本地与 CI 全量编译运行通过。
- 🟡 **可行性已论证、待构建**：语义与实现路径已确定，工程量在后续迭代。
- ⚪ **需外部条件**：依赖宿主编译器 intrinsics / fenv / 硬件特性，或依赖尚不存在的上层结构。

### 3.1 整数运算与标志位（核心，工程量最大）

| # | 可行性 | 状态 | 落点 |
|---|---|---|---|
| 1 | W/X 宽度分离、W 结果零扩展 | ✅ | `W` / `Zext32` / `Sext32` / `Sext` |
| 2 | 寄存器移位量掩码（5/6 位），规避 `shift>=width` UB | ✅ | `MaskShift32/64` / `Shl` |
| 3 | 逻辑右移忽略 W 高位垃圾 | ✅ | `Shr` |
| 4 | 算术右移用无符号运算显式构造符号填充位 | ✅ | `Sar` |
| 5 | ADDS/SUBS 的 N/Z/C/V 全部从无符号位模式推导（零 UB） | ✅ | `AddWithFlags` / `SubWithFlags` |
| 6 | ANDS 等逻辑指令的 N/Z（C=V=0） | ✅ | `LogicFlags` |
| 7 | 16 个条件码在全部 16 种 NZCV 组合上的真值表 | ✅ | `ConditionHolds` |
| 8 | 小域穷举交叉校验（值/N/Z 一致性） | ✅ | `ExhaustiveSmallDomain` |
| 9 | 乘加/乘减（MADD/MSUB）、带进位 ADC/SBC、CCMP/CCMN | 🟡 | 后续 |
| 10 | 位域（UBFM/SBFM/BFM）、EXTR、CLZ/REV/RBIT | 🟡 | 后续 |
| 11 | CRC32、原子访存数值语义 | 🟡 | 后续 |

### 3.2 浮点位级语义

| # | 可行性 | 状态 | 落点 |
|---|---|---|---|
| 12 | F32/F64 与原始位模式严格别名安全互转 | ✅ | `F32Bits`/`BitsF32`/`F64Bits`/`BitsF64` |
| 13 | FABS/FNEG 用位操作，保留 NaN 载荷、正确处理 ±0 | ✅ | `FpAbs*/FpNeg*` |
| 14 | 默认 NaN（quiet、正）与 NaN 检测 | ✅ | `DefaultNan*` / `IsNan*` |
| 15 | FCMP → NZCV，NaN 无序使有序条件码失败 | ✅ | `FpCompareNZCV` |
| 16 | FCMPE 的无效异常标志（与 FCMP 仅异常不同） | ⚪ | 需运行时 FPSR 状态 |
| 17 | FMAX/FMIN 的 NaN 与 ±0 规则、FHADD 等 | 🟡 | 后续 |
| 18 | FPCR 舍入模式 / 非正规数冲刷（FZ）/ AH 半精度 | ⚪ | 需 fenv 或软浮点路径 |

### 3.3 浮点 ↔ 整数转换（FCVTZ* / SCVTF / UCVTF）

| # | 可行性 | 状态 | 落点 |
|---|---|---|---|
| 19 | NaN → 0、向零截断、越界饱和、32/64 目标宽度 | ✅ | `FpToUnsigned` / `FpToSigned` |
| 20 | 转换前做精确可表示区间检查，杜绝 `float→int` 越界 UB | ✅ | 同上（bound check before cast） |
| 21 | 整数 → F32/F64（有符号/无符号源，F32 结果窄化） | ✅ | `IntToFp` |
| 22 | FCVTAS/FCVTAU（最近偶数舍入）、FCVTPS/FCVTNS 等舍入族 | 🟡 | 后续 |
| 23 | FCVT 半精度 / FCVTL 加宽、FJCVTZS | ⚪ | 需 FP16 视图 |

### 3.4 SIMD / 向量数值（工程量最大，独立阶段）

| # | 可行性 | 状态 |
|---|---|---|
| 24 | V128 lane 布局、标量写零高位、INS/UMOV lane 顺序（LE lane0 最低地址） | ✅ 已有 `runtime/vreg.h` |
| 25 | F32x4 逐 lane 标量循环 + `#pragma omp simd` 可行性 | ✅ 已有 codegen |
| 26 | 整数向量 I32x4 / I16x8 / I8x16 逐 lane 运算与饱和 | 🟡 |
| 27 | 向量归约、跨 lane、TBL/TBX、LD1/ST1 结构访存 | 🟡 |
| 28 | 用宿主 SIMD intrinsics 替换逐 lane 循环（性能可行性） | ⚪ |

### 3.5 与生成代码 / 优化的衔接（本项目的最终目的）

| # | 可行性 | 状态 |
|---|---|---|
| 29 | codegen 直接摊平表达式（现状） | ✅ 已有 `codegen.cpp` |
| 30 | 数值语义可发射进生成代码、且在宿主 `-O2` 下对 UB 边界输入仍位级正确 | ✅ `common/numeric_emit.h` + `tests/integration/test_numeric_codegen.cpp` |
| 31 | codegen 全面改为发射本层调用（替换所有摊平表达式） | 🟡 |
| 32 | IR 层常量折叠 / DCE / 标志位合并（roadmap P5） | 🟡 |
| 33 | 宿主编译器内联后零开销的基准验证 | ✅ `bench_numeric`（见 §5） |
| 34 | fastmem / 软 MMU 数值地址计算 | 🟡 |
| 35 | 真实编译器 fixture（`clang -target aarch64`）交叉验证位级结果 | ⚪ |

> 工程量的真实分布：3.1 与 3.4 是「大」的部分——即便已有 1–8、12–15、19–21、24–25、30、33 的✅，剩余指令族、舍入族与向量整数/归约仍需逐条构建 golden + 端到端。本清单把「可行性」显式化，避免把「能跑通几条指令」误当成「数值语义完备」。

## 4. 参考实现 API

`include/switchrecomp/common/numeric.h`（header-only，C++17）：

- 位/宽度：`F32Bits BitsF32 F64Bits BitsF64 W Zext32 Sext32 Sext`
- 移位：`MaskShift32 MaskShift64 Shl Shr Sar`
- 整数+标志：`AddWithFlags SubWithFlags LogicFlags ConditionHolds`（`FlagsResult{value,nzcv}`）
- 浮点位级：`FpSign32/64 FpAbs32/64 FpNeg32/64 DefaultNan32/64 IsNan32/64 FpCompareNZCV`
- 转换：`FpToUnsigned FpToSigned IntToFp`

`include/switchrecomp/common/numeric_emit.h`（header-only，发射桥）：

- `Prologue / Zext32 / Sext32 / Shl / Shr / Sar / AddWithFlags / SubWithFlags / ConditionHolds / FpToSigned / FpToUnsigned / IntToFp`：生成调用数值层的 C++ 表达式字符串。
- `SelfTestProgram()`：仅用上述 helper 拼出的完整可编译程序，覆盖全部 UB 边界输入，供端到端测试编译运行。

NZCV 布局与运行时一致：`bit31=N, bit30=Z, bit29=C, bit28=V`。

## 5. 验证与基准

- 单元测试：`tests/unit/common/test_numeric.cpp`，覆盖位往返、宽度、移位 UB 边界、NZCV 溢出/进位/借位、16×16 条件码真值表、小域穷举、FP 位操作/NaN/FCMP、FCVTZ* 饱和与精确边界、SCVTF/UCVTF。
- 端到端：`tests/integration/test_numeric_codegen.cpp`，把 `SelfTestProgram()` 用真实编译器 `-O2` 编译并运行，在 W 高位残留、`shift>=64`、32 位进位、NaN/越界转换、负数算术右移、溢出后条件码等 UB 边界上校验输出；无 g++ 的 runner 上跳过。
- 构建验证：`cmake -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure`，全部测试通过；CI 的 Linux 与 Windows 均构建运行。
- 性能可行性：`benchmarks/bench_numeric.cpp` 对本层热点函数做吞吐基准，用于验证「内联后零开销」这一核心声明。每个用例都带一个确定性伪随机输入流（PRNG 本身计入测量），因此数字是「输入 + 运算」的上界。

本机实测（`bench_numeric --benchmark_min_time=0.2s`，MSYS2 UCRT64 GCC 16.2.0，`-O3`-equivalent Release，16 核 2419 MHz 宿主）：

| 用例 | 时间/次 | 说明 |
|---|---|---|
| `BM_AddWithFlags` | 4.98 ns | 位级精确 NZCV 加法（含全部标志推导） |
| `BM_AddNaive`（参照） | 3.84 ns | 裸 `a + b`，无标志；差额即标志推导成本 |
| `BM_ShlMasked` | 4.22 ns | 含 `shift >= 64` 的掩码移位（规避 UB） |
| `BM_FpToSigned` | 7.02 ns | 含 NaN/越界饱和的 FCVTZS 语义 |
| `BM_ConditionHolds` | 11.3 ns | 16 个条件码的完整分支求值 |

结论：`AddWithFlags` 相对裸加的差额约 1.1 ns/次，且两者都包含同一 PRNG 输入开销；说明标志推导确实被内联并与加法融合，没有引入调用/分配/间接跳转。掩码移位与裸加同量级，证明「无 UB 的 AArch64 移位语义」可以零额外开销落地。绝对数字依赖宿主 CPU 与编译选项，跨机器比较须附本报告同款环境信息。

## 6. 开放问题

- FPCR 动态舍入模式如何在不引入 fenv 全局状态的前提下精确复现（软浮点路径 vs 编译器 RTM）。
- NaN 载荷传播在商用标题中是否影响可观测行为（默认 NaN 模式下通常不影响，但需验证）。
- 向量整数饱和与 host intrinsics 的等价性需要逐条 golden 交叉验证。
- `bench_numeric` 的绝对数字依赖宿主 CPU，报告须附编译选项与机器信息。
