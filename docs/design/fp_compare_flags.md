# RC-FP-002 设计：FCMP 浮点比较与 FCVT 转换

状态：已实现
日期：2026-10-08
作者：arm64-recompiler

## 1. 现状：整数 CMP → 标志 → B.cond 的降级方式

基于当前代码（RC-FP-002 开始前）：

1. **CMP（整数）**：[DecodeCmpImm](../../../src/translator/translator.cpp) 把
   `CMP <Rn>, #imm`（SUBS 别名）译码为普通 `Sub(Rn, imm)`，代码注释明确写道
   "flags ignored in Phase 1"。即 NZCV 既不进入 IR，也不写入 GuestContext。
2. **B.cond**：[DecodeBCond](../../../src/translator/translator.cpp) 提取
   `cond`（bits 3:0）后发出 `CondBranch(Imm(cond))`，**分支目标（imm19）被丢弃**。
3. **CondBranch 的 codegen**：[LowerInstruction](../../../src/codegen/codegen.cpp)
   把条件码立即数经 `RegIndex`（对非寄存器常量返回 0）误当作寄存器索引，生成占位代码
   `if (ctx->x[0]) goto bb_1; else goto bb_0;`。
4. 标签：codegen 仅在每个 BasicBlock 前生成 `bb_<idx>` 标签；翻译阶段每个函数只产生
   一个 BasicBlock，因此任何真实分支目标都无法表达——当前没有任何端到端用例执行过
   B.cond（b_cond 仅有 golden fixture，且其 codegen 单测使用手工构造的 IR）。

结论：整数比较与条件分支在 RC-FP-002 时是**未贯通的占位链路**；RC-FIX-002 已把
整数 CMP 接入同一 NZCV 通路（见第 7 节），无条件 B 的目标解析也一并补齐。

## 2. ARM 语义：FCMP 复用 NZCV 机制

ARMv8-A 中 FCMP 与整数 SUBS 一样**写同一组 NZCV 条件标志**，B.cond 不区分标志来源：

| 标志 | FCMP 置位条件（ARM ARM "Floating-point compare"） |
|------|---------------------------------------------------|
| N    | LessThan（a < b，有序）                            |
| Z    | Equal（a == b；+0.0 与 -0.0 相等）                 |
| C    | GreaterThan or Equal（a >= b，有序）               |
| V    | Unordered（任一操作数为 NaN）                      |

NaN 情况下 N=0、Z=0、C=0、V=1。B.cond 的条件码按下表解释标志（N=bit31、Z=bit30、
C=bit29、V=bit28），其中 LT/LE/GT/GE 通过 N 与 V 的比较实现 unordered 语义：

| cond | 名称 | 表达式 |
|------|------|--------|
| 0 | EQ | Z |
| 1 | NE | !Z |
| 2 | CS/HS | C |
| 3 | CC/LO | !C |
| 4 | MI | N |
| 5 | PL | !N |
| 6 | VS | V |
| 7 | VC | !V |
| 8 | HI | C & !Z |
| 9 | LS | !C \| Z |
| 10 | GE | N == V |
| 11 | LT | N != V |
| 12 | GT | !Z & (N == V) |
| 13 | LE | Z \| (N != V) |
| 14 | AL | 1 |
| 15 | NV（编码位，实际同 AL 处理） | 0 |

RC-FP-002 承诺 EQ/NE/LT/LE/GT/GE；由于实现直接生成完整 NZCV 位与完整条件映射，
CS/CC/MI/PL/VS/VC/HI/LS 对 FCMP 来源的标志同样可用（VS/VC 即可用于检测 NaN）。
RC-FIX-002 后整数 CMP 也写入完整 NZCV（第 7 节），全部 14 个有效条件码对两种来源
均可用。

## 3. IR 设计

新增两个 opcode（公共头仅做枚举追加，不改任何既有签名）：

- `Opcode::Compare`：浮点比较。操作数 `{Rn(FReg), Rm(FReg 或立即数 0)}`，
  指令类型 `Void`（结果即写入 `ctx->nzcv` 的标志，不产生 SSA 值）。
- `Opcode::Convert`：类型转换。操作数 `{dst 寄存器常量, src 操作数}`，
  指令类型 = 目标类型（`S32/S64` 或 `F32`），由 (src 类型, dst 类型) 决定语义：
  - F32 → S32/S64：FCVTZS（toward zero）；
  - S32/S64 → F32：SCVTF（有符号整数转浮点）。

JSON 序列化字符串：`"Compare"`、`"Convert"`。

B.cond 仍复用 `Opcode::CondBranch`，但 DecodeBCond 现在发出两个操作数：
`{U32 cond, U64 target_word_index}`，以与 CBZ/CBNZ（单操作数）及手工构造的
CondBranch（codegen 单测，单操作数）区分。

### 分支目标解析

guest 中分支偏移按 `target = PC + sign_extend(imm19 << 2)` 计算。翻译主循环记录每个
guest 字发出的**第一条 IR 指令的全函数线性序号**；全部译码后做一次 post-pass，把
B.cond 的目标从"guest 字索引"替换为"IR 指令线性序号"。这样即使分支前出现复合指令
（LDP/MADD 等每字发出多条 IR）也能正确解析。

codegen 在函数内为**每条 IR 指令**生成序号标签 `glabel<N>:`；B.cond 生成
`if (<条件表达式>) goto glabel<N>;`，条件不成立时自然 fallthrough，与 ARM 语义一致。

## 4. 生成 C 的形式

FCMP（`fcmp s0, s1`）：

```c
ctx->nzcv = (uint32_t)(((uint32_t)(ctx->f[0] < ctx->f[1]) << 31)
                     | ((uint32_t)(ctx->f[0] == ctx->f[1]) << 30)
                     | ((uint32_t)(ctx->f[0] >= ctx->f[1]) << 29)
                     | ((uint32_t)(((ctx->f[0] != ctx->f[0])
                                 || (ctx->f[1] != ctx->f[1]))) << 28));
```

普通 C 比较在 NaN 时返回 false，与 ARM ordered 谓词一致；`x != x` 用于自检测 NaN
（生成代码不使用 -ffast-math）。`FCMP Sn,#0.0`（Rm=31）第二操作数为立即数 0 标记，
渲染为 `((float)0)`。

B.cond（`b.lt glabelN`）：

```c
if ((((ctx->nzcv >> 31) & 1u) != ((ctx->nzcv >> 28) & 1u))) goto glabelN;
```

FCVTZS（`fcvtzs w0, s0`，RC-FIX-002 起为显式夹取，详见第 7.2 节）：

```c
ctx->x[0] = (uint64_t)(isnan(ctx->f[0]) ? 0
    : (ctx->f[0] >= 2147483648.0f ? 2147483647
    : (ctx->f[0] <= -2147483648.0f ? (-2147483647 - 1)
    : (int32_t)ctx->f[0])));
```

（64 位目标用 double 阈值 `9223372036854775808.0` 与 `(int64_t)` 结果；INT64_MIN
写作 `(-9223372036854775807LL - 1)`。正常路径的 C 强制转换截断方向为 toward zero，
与 FCVTZS 一致。）

SCVTF（`scvtf s0, w0`）：

```c
ctx->f[0] = (float)(int32_t)ctx->x[0];
```

（源为 X 寄存器时用 `(int64_t)`；源寄存器以 `S32/S64` 类型的寄存器索引常量携带宽度。）

## 5. 边界情况

- **NaN**：比较结果全部 false、V=1；EQ 为假、NE 为真、LT/LE/GT/GE 均为假，符合 ARM
  unordered 语义。FCMP 不产生异常（FCMPE 信号性变体本任务不实现）。
- **±0.0**：`==` 判定相等，Z=1。
- **溢出/饱和**：RC-FIX-002 起 FCVTZS 生成显式有序夹取（第 7.2 节），NaN→0、
  +Inf/超上界→INT_MAX、-Inf/超下界→INT_MIN，与 ARM 饱和语义一致；浮点→整数的 C
  强制转换只在已证明范围内执行，消除了原先的未定义行为。
- **非规格化数**：按 host 默认 FTZ 关闭处理。
- 仅支持单精度源/目的（S 寄存器）与有符号变体（FCVTZS/SCVTF）；FCVTZU/UCVTF、
  双精度、半精度及 F64 转换留待后续。

## 7. RC-FIX-002：整数 CMP → NZCV → B.cond 与饱和转换

### 7.1 译码

- `CMP Rn, Rm`（SUBS shifted register 别名，imm6=0、Rd=31）由新增 DecodeCmpReg
  译码为 `Compare`；指令类型携带比较宽度：64 位 `U64`、32 位 `U32`。真实
  `SUBS <Rd>`（Rd≠31）仍由 DecodeSubReg 按原 `Sub` 降级（保留结果、不产生标志）。
- `CMP Rn, #imm`（SUBS immediate，Rd=31）由 DecodeCmpImm 译码为 `Compare`；
  Rd≠31 的 SUBS 保持原 `Sub` 降级。
- 比较宽度由 sf（bit31）决定；32 位 CMP 时 codegen 全部谓词按 32 位解释。
- 已知缺口：带移位的寄存器 CMP（imm6≠0，如 `cmp x0,x1,lsl #3`）不匹配该表项，
  仍走无标志的 Sub 路径；先应用移位再比较留待后续。
- 无条件 B 现在计算 `target = PC + sign_extend(imm26 << 2)` 并发出带目标地址操作数
  的 Branch；post-pass 同时解析 Branch（单 U64 操作数）与 B.cond（双操作数）。
- 顺带修复预存缺陷：DecodeMovz 原先把 MOVZ 建模为 `Add(0, imm)`，目标寄存器被
  隐式固定为 x0（`mov x1,#imm` 实际写入 x0）；现改为 `Mov(Rd, imm)`，写入真实 Rd。
  MOVN 仍有同类问题（Not 的目标同样隐式为 x0），留待后续。

### 7.2 整数 NZCV 生成

整数 CMP 与浮点 FCMP 写同一个 `ctx->nzcv`（N=bit31、Z=bit30、C=bit29、V=bit28），
因此 AppendCondExpr 与全部条件码无需区分标志来源：

| 标志 | 实现 |
|------|------|
| N | `(intN_t)a < (intN_t)b` |
| Z | `(uintN_t)a == (uintN_t)b` |
| C | `(uintN_t)a >= (uintN_t)b`（SUBS 的 C = NOT borrow） |
| V | 有符号减法溢出。令无符号回绕结果 `R = a - b`，则 `V = (((a ^ b) & (a ^ R)) >> (N-1)) & 1` |

减法只在无符号整数中进行，不存在 signed-integer-overflow UB；N/Z/C/V 全部按 ARM
语义精确实现（非"V 置 0"近似）。条件映射沿用既有表，LT=`N!=V`、GE=`N==V` 等对
整数来源同样成立。

### 7.3 FCVTZS 饱和决策

- 阈值：32 位目标用 f32 字面量 `±2147483648.0f`（2^31 可被 f32 精确表示，该区间
  f32 步长 256，任何 ≥2^31 的 f32 都应夹取）；64 位目标把 f32 提升为 double 后与
  `±9223372036854775808.0` 比较（2^63 同样是 f32 可精确表示的 2 的幂，提升无损）。
- 结果常量：INT_MAX 直接书写；INT_MIN 写作 `(-2147483647 - 1)` /
  `(-9223372036854775807LL - 1)`，避免超范围整型字面量及实现定义的窄化转换。
- 整个三元表达式为整型，以 `(uint64_t)` 显式拓宽后写入 `ctx->x[]`；负饱和值读回时
  按 `(int64_t)ctx.x[0]` 解释（x0 = 0xFFFFFFFF80000000 → -2147483648）。
- 生成代码顶部新增 `#include <math.h>`（isnan）。

## 6. 迁移影响

| 文件 | 变更 |
|------|------|
| include/switchrecomp/ir/ir.h | 枚举追加 `Compare`、`Convert` |
| src/ir/ir.cpp | opcode ↔ 字符串映射追加两项 |
| src/translator/translator.cpp | 新增 DecodeFCmp/DecodeFcvtzs/DecodeScvtf 及译码表项；DecodeBCond 增加目标操作数；主循环增加目标 post-pass |
| src/codegen/codegen.cpp | Compare/Convert 降级；B.cond 形式的 CondBranch 降级；每指令 `glabelN` 标签 |
| tests/fixtures/translator/golden/b_cond.json | 随 DecodeBCond 输出变化更新 |
| tests/fixtures/translator/golden/fcmp_s.json | 新增 |
| tests/fixtures/translator/golden/fcvtzs_s.json | 新增 |
| tests/fixtures/translator/golden/scvtf_s.json | 新增 |
| tests/unit/translator/test_decode.cpp | 新增 6 条 decode 单测 |
| tests/integration/test_e2e_fcmp.cpp | 新增两个端到端用例 |
| tests/CMakeLists.txt | 注册新 e2e 文件 |

RC-FIX-002 追加：

| 文件 | 变更 |
|------|------|
| src/translator/translator.cpp | 新增 DecodeCmpReg/DecodeSubsReg 及译码表项；DecodeCmpImm 对 Rd=31 产出 Compare；DecodeMovz 改发 Mov 到真实 Rd；DecodeB 计算目标地址；post-pass 增加无条件 Branch 解析 |
| src/codegen/codegen.cpp | 整数 Compare NZCV 降级（EmitIntegerCompareNZCV）；FCVTZS 显式饱和夹取；Branch 按 glabel 降级；生成代码加 `#include <math.h>` |
| tests/fixtures/translator/golden/cmp_imm.json、b.json、movz_imm.json | 随译码输出更新 |
| tests/unit/translator/test_decode.cpp | 更新 CmpImmediate/Branch；新增 CmpRegister/CmpRegister32/SubsRegisterNotCmp/SubsImmediateNotCmp |
| tests/unit/codegen/test_codegen.cpp | 新增 6 条夹取/整数 NZCV/标签单测；GeneratedCSyntaxValid 覆盖整数 Compare |
| tests/integration/test_e2e_intcmp.cpp | 新增：3 个整数分支用例 + 2 个饱和用例（UBSan 编译） |

公共头既有签名：无变更。
