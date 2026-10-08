# SIMD / AdvSimd 架构设计

状态：**已批准（架构师，2026-10-08）**；阶段 1（统一向量寄存器银行）已实现
日期：2026-10-08
作者：arm64-recompiler

本文描述 SwitchRecomp 的 AdvSimd（AArch64 NEON）支持架构。阶段 1 已落地
（sr_vreg 统一银行、GuestContext 改造、codegen 标量 S/D 发射迁移、别名语义测试）；
阶段 2–4 按第 6 节计划推进。
关联文档：[fp_compare_flags.md](fp_compare_flags.md)、[fp_rounding.md](fp_rounding.md)、
[arm64_instructions.md](arm64_instructions.md)。

---

## 架构师决策

以下 6 条决策于 2026-10-08 由架构师批准，作为阶段 1 的实施依据：

1. **一次性迁移**：删除 fp[]/f[32]/d[32]，v[32] 为唯一 SIMD&FP 真相，
   f/d 读写全部改走 sr_v_* 接口；不保留兼容 accessor 路径（避免双重真相）。
2. **允许 vreg.h 依赖**：GuestContext（runtime.h）与 codegen 生成的 fallback
   纯 C struct 均依赖 `switchrecomp/runtime/vreg.h`；该头是纯 C11/C++11
   双兼容的稳定 ABI，随 runtime 头一起发布。
3. **VectorBackend 延后**：路线 B（intrinsics 加速层，CodegenOptions 增加
   VectorBackend: Auto|Intrinsic）不在阶段 1–4 引入，待基准数据满足白名单
   准入（收益 ≥ 2×）后再单独评审。
4. **aarch64 CI 暂缓**：不新增 aarch64 交叉编译 / QEMU 用户态 CI；intrinsics
   路径在没有实际 CI 覆盖前不得启用（B 白名单初始为空）。
5. **无新选项也正确**：阶段 1–4 不增加任何编译/运行时选项，路线 A（逐 lane
   标量 C，pragma 不支持时退化正确）在现有构建矩阵下默认正确。
6. **阶段 2 仅 FP**：阶段 2 只实现 FP 向量数据路径（MOVI/FDUP/INS/UMOV +
   F32×4 FADD）；向量整数算术与整数 lane 视图不进阶段 2 验收。

---

## 1. 目标与范围

### 1.1 总体目标

为 SwitchRecomp 的「ARM64 指令 → IR → C 源码 → host 编译执行」流水线引入
AArch64 AdvSimd 支持，使 guest 的向量数据处理指令与现有标量指令走同一套
译码/IR/codegen 架构，不引入独立的向量执行子系统。

### 1.2 首个验收目标（最高优先级）

**自有 ARM64 测试程序向量闭环**：由本项目用 AArch64 汇编（自有 guest 向量
程序，手工编码机器码）驱动完整链路：

```
guest AdvSimd 机器码 → Translate(IR) → GenerateC(C 源码)
                    → host g++ 编译 → 运行 → 打印/断言每个 lane 的结果
```

- 不依赖 Switch 系统软件、不依赖真实 Switch 硬件；
- 与现有 E2E（test_e2e_float.cpp 等）同一组织方式：无 g++ 时 `GTEST_SKIP`；
- 第一个闭环程序：`MOVI + FDUP + FADD(Vd.4S)`，host 侧 main 打印 4 个 lane。

### 1.3 支持的 AdvSimd 子集

| 类 | 指令（代表） | 阶段 |
|----|--------------|------|
| 寄存器银行 | V0–V31 128-bit，B/H/S/D/Q 与 lane 视图统一 | 阶段 1 |
| 数据移动 | MOVI（向量立即数）、FMOV（广义）、FDUP（broadcast）、INS（lane insert）、UMOV（lane extract） | 阶段 2 |
| 向量算术（FP） | FADD/FSUB/FMUL（F32×4），后续 F64×2 | 阶段 2 |
| 向量访存 | LD1/ST1（single-structure，1 个寄存器） | 阶段 3 |
| 向量 1-source（FP） | FSQRT、FRINTN/P/M/Z/A（F32×4，后续 F64×2） | 阶段 4 |
| 向量比较 | FCMEQ/FCMGT/FCMLT（lane mask 结果；非 NZCV） | 阶段 4 |

### 1.4 明确不在范围

- AArch32（A32/T32）NEON/VFP——标量写 S 的语义在 AArch32 与 AArch64 不同
  （见 2.3），本框架可直接声明「不支持 AArch32 执行状态」；
- 半精度（H/FP16）、BF16、crypto、SVE/SVE2、多寄存器 LD1/ST1 结构形式、
  gather/scatter、向量规约（ADDV 等）——留待后续迭代；
- FPCR/FPSR 的完整建模（沿用现状：fpcr/fpsr 字段保留但不参与语义；
  FRINTN/FRINTA 为固定舍入模式，不依赖 FPCR.RMode）。

---

## 2. ARM 寄存器模型

### 2.1 V 寄存器与别名

AArch64 提供 **32 个 128-bit SIMD&FP 寄存器 V0–V31**，所有视图是同一物理
寄存器的别名：

| 名称 | 宽度 | 访问位 | 典型含义 |
|------|------|--------|----------|
| Bn | 8 bit | Vn[7:0] | 字节标量 |
| Hn | 16 bit | Vn[15:0] | 半精度/半字标量 |
| Sn | 32 bit | Vn[31:0] | 单精度标量（S0 = Q0 低 32 位） |
| Dn | 64 bit | Vn[63:0] | 双精度标量 |
| Qn / Vn | 128 bit | Vn[127:0] | 全寄存器/向量 |

向量 lane 视图（lane k 占据 `Vn[k*esize +: esize]`）：

| 视图 | lane 数 × 元素宽 |
|------|------------------|
| Vn.16B / Vn.8B | 16/8 × 8 bit |
| Vn.8H / Vn.4H | 8/4 × 16 bit |
| Vn.4S / Vn.2S | 4/2 × 32 bit |
| Vn.2D / Vn.1D | 2/1 × 64 bit |

- lane 顺序：**lane 0 在最低地址/最低位**（小端序，Switch 为小端）；
- 注意：寄存器**编号与宽度无关**，V0 的所有别名共享同一编号 0。

### 2.2 标量形式与向量 Q 形式的关系

- 标量 1-source / 2-source 浮点族（现有译码表中的 0x1E2/0x1E6 族，如
  FADD Sd,Sn,Sm = `0x1E202800`）使用 S/D 名访问寄存器低位；
- 向量形式由编码中 **Q bit（bit 30）** 决定操作宽度：
  - **Q = 1**：128-bit V 形式（如 `FADD Vd.4S,Vn.4S,Vm.4S`，基址 `0x4E202800`）；
  - **Q = 0**：64-bit 形式（如 `FADD Vd.2S,...`，基址 `0x0E202800`）。
- 阶段 2 起，译码器需区分「标量 FP 空间（0x1E/0x1E6）」与「AdvSimd 向量
  空间（0x0E/0x4E/0x6E…）」，二者掩码不冲突。

### 2.3 关键语义：标量写 S 时高位如何处理

**正确规则（AArch64）**：标量写只修改被命名宽度的低位，**未访问的高位清零**。

| 写操作 | 被修改位 | 其余位 |
|--------|----------|--------|
| 写 Bn | [7:0] | [127:8] 清零 |
| 写 Hn | [15:0] | [127:16] 清零 |
| 写 Sn | [31:0] | **[127:32] 清零（96 bit）** |
| 写 Dn | [63:0] | **[127:64] 清零（64 bit）** |

即：**AArch64 下写 S0 会把 V0 的高 96 位清零，不存在「标量写 S 保持高位」**。
该规则见 ARM ARM 的 SIMD&FP register file 章节（"unused upper bits are
ignored on reads and set to zero on writes"）；多份二手资料一致：

- ARM 官方文档原文（转引自 CSDN 技术博客）：「如果 SIMD 指令和浮点指令对标量
  数据进行操作，那么它仅访问……低位。而没被使用的高位则在读时被忽略，
  **在写时被清零**」；
- 《Porting to 64-bit Arm》白皮书（Chris Shore, Arm）：32-bit 写清除寄存器
  上半部分；
- AArch64 NEON 概览资料：「向向量寄存器写入 64 位或更少位的值会导致高位被
  清零」。

**必须区分的例外/差异**：

1. **AArch32（ARMv7）语义相反**：AArch32 下写 S 寄存器（如 VMOV.F32）保持
   D/Q 高位不变（旧 VFP 打包模型，S0/S1 共同映射到 D0）。本框架不支持
   AArch32，不受此规则影响，但文档与测试必须写清，防止把 AArch32 经验带入。
2. **向量 64-bit 形式（Q=0，如 FADD Vd.2S）**：ARM ARM 对其高 64 bit 的措辞是
   **UNKNOWN**（既不保证保持、也不保证清零，允许实现自由）。guest 不得依赖
   该高位。**本框架选择将其清零**——清零是 UNKNOWN 的合法确定性超集，且与
   标量规则一致，可简化 codegen 与测试。
3. 读操作（任意宽度）总是忽略未访问高位。

**对实现的硬性要求**：统一寄存器银行后，所有标量 S/D 写（含 FMOV Sd,Sn、
FMOV Sd,#imm、FADD/FSUB/FMUL/FDIV、FNEG/FABS/FSQRT/FRINT*/FCSEL）都必须通过
「写低位 + 清高位」的接口完成，不能直接对 32-bit 引用赋值而保留旧高位。

---

## 3. 统一寄存器银行设计

### 3.1 推荐数据结构

新增（阶段 1）纯 C11 的向量寄存器类型与内联访问接口，建议放在
`include/switchrecomp/runtime/vreg.h`：

```c
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct sr_vreg {
    alignas(16) uint8_t bytes[16];
} sr_vreg;

/* ---- 标量读（strict-aliasing 安全：定长 memcpy 被编译器优化为单条 load）---- */
static inline float    sr_v_get_s(const sr_vreg* r) {
    float v;    memcpy(&v, r->bytes, 4); return v;
}
static inline double   sr_v_get_d(const sr_vreg* r) {
    double v;   memcpy(&v, r->bytes, 8); return v;
}
/* sr_v_get_h / sr_v_get_b 同理（uint16_t/uint8_t，FP16 阶段再补 float16 视图）*/

/* ---- 标量写：写字段宽度，其余高位清零（ARMv8 AArch64 规则）---- */
static inline void sr_v_set_s(sr_vreg* r, float v) {
    memcpy(r->bytes, &v, 4);
    memset(r->bytes + 4, 0, 12);   /* [127:32] = 0 */
}
static inline void sr_v_set_d(sr_vreg* r, double v) {
    memcpy(r->bytes, &v, 8);
    memset(r->bytes + 8, 0, 8);    /* [127:64] = 0 */
}

/* ---- lane 视图（lane 为编译期/小值域整数）---- */
static inline float  sr_v_get_lane_s(const sr_vreg* r, int lane) {
    float v; memcpy(&v, r->bytes + lane * 4, 4); return v;
}
static inline void   sr_v_set_lane_s(sr_vreg* r, int lane, float v) {
    memcpy(r->bytes + lane * 4, &v, 4);   /* INS 语义：其余 lane 保持 */
}
/* d/h/b lane 版本同构 */

/* ---- 全 128-bit 读写（MOVI、向量 load/store、Q 形式结果）---- */
static inline void sr_v_load16(sr_vreg* r, const void* src) {
    memcpy(r->bytes, src, 16);
}
static inline void sr_v_store16(const sr_vreg* r, void* dst) {
    memcpy(dst, r->bytes, 16);
}
```

设计要点：

- **接口用纯 C11 函数而非 C++ 引用 accessor**：现有 codegen 生成的源码同时要在
  两条路径上合法——`#ifdef __cplusplus` 下 include runtime.h，`#else` 下使用
  GenerateC 输出的 fallback 纯 C struct；并且
  [GeneratedCSyntaxValid](../../tests/unit/codegen/test_codegen.cpp)
  以 **gcc（C 编译器）-fsyntax-only** 校验。纯 C inline 函数在两条路径上
  完全一致，codegen 只需一条发射路径；
- **memcpy/memset 定长形式**：strict-aliasing 安全、跨编译器（GCC/Clang/MSVC）
  语义明确；在 -O1 以上即被折叠为单条 load/store/zero，零额外开销；
- **lane 写（INS）保持其余 lane**，标量写（set_s/d）清高位，两条写路径的区别
  直接对应 ARM 语义，防止混淆；
- C++ 侧如需 `float&` 风格视图，可在 C++ 包装层另行提供，但 **codegen 不依赖
  它**（引用赋值无法表达清高位）。

### 3.2 GuestContext 改造

[GuestContext](../../include/switchrecomp/runtime/runtime.h) 目标形态：

```cpp
struct GuestContext {
    u64 x[31];
    u64 sp;
    u64 pc;
    u32 nzcv;
    sr_vreg v[32];          // 取代 fp[32*16]
    u32 fpcr;
    u32 fpsr;
    u64 shared_counter;
    u64 thread_handle;
    // 删除 float f[32]; 与 double d[32];
};
```

### 3.3 f / d 的处理：一次性迁移（推荐）

两个备选：

| 方案 | 内容 | 评价 |
|------|------|------|
| **一次性迁移（推荐）** | 删除 f[32]/d[32]/fp[512]，v[32] 为唯一真相；f/d 的读写全部改走 sr_v_* 接口 | 当前 f、d、fp 三个银行互不连通（fp[] 无任何消费者；f/d 仅作标量 scratch），不存在「同一寄存器两种类型交替使用」的存量代码，迁移本质是机械替换 |
| 兼容 accessor 路径 | 保留 f/d 数组，另加同步钩子 | 制造双重真相与同步遗漏风险；f/d 数组无法表达 Q/lane 视图，向量支持仍需再迁一次。**不推荐** |

选择一次性迁移的依据：

- 全仓 Grep 确认 `fp[]`（0x…）在 src 中无任何读写点，仅出现在 codegen 生成的
  C 文本里；
- `fpcr/fpsr` 仅在 runtime.h 与 codegen 生成文本中出现，无逻辑依赖；
- 所有 f/d 使用点均为「标量算法 scratch + 测试打印」，已在 3.4 列全。

### 3.4 迁移影响清单（精确到文件与用例）

**生产代码**：

| 文件 | 变更点 |
|------|--------|
| include/switchrecomp/runtime/runtime.h | 删除 fp/f/d 三字段，新增 `sr_vreg v[32]`，include vreg.h |
| include/switchrecomp/runtime/vreg.h | **新增**（3.1 的接口） |
| [src/codegen/codegen.cpp](../../src/codegen/codegen.cpp) | (a) 生成头：fallback 纯 C struct 中 `fp/f/d` → `sr_vreg v[32]`，并 include vreg.h；(b) Stringify 中寄存器常量渲染（约 L99–L110）改为 `sr_v_get_s(&ctx->v[N])` / `sr_v_get_d(...)`；(c) 所有标量写 lowering 改为 `sr_v_set_s/d(&ctx->v[N], ...)`：Add/Sub/Mul/Div（L355–L387）、Mov（L388–L400）、Neg/Abs（L401–L422）、Sqrt/Frint*（L423–L456）、Select（L457–L476）；(d) Load/Store 的 F32/F64 分支（L505–L549）改为经 sr_v_set_s/d 与 sr_v_get_* |
| [src/translator/translator.cpp](../../src/translator/translator.cpp) | **无逻辑变更**：寄存器常量仍以 F32/F64 类型携带索引（IsFRegConstant/IsDRegConstant 机制不变），仅更新注释 |

**测试代码（直接使用点，Grep 全量）**：

| 文件 | 用例 | 用法 |
|------|------|------|
| [test_e2e_float.cpp](../../tests/integration/test_e2e_float.cpp) | `E2E.FloatAddScalars` | 追加 main 中 `ctx.f[0]` → `sr_v_get_s(&ctx.v[0])` |
| [test_e2e_fcmp.cpp](../../tests/integration/test_e2e_fcmp.cpp) | `E2E.FCmpBranchLessThan` | `ctx.f[2]` |
| 同上 | `E2E.FConvertTruncateAndBack` | `ctx.f[0]` |
| [test_e2e_fp64.cpp](../../tests/integration/test_e2e_fp64.cpp) | `E2EFP64.AddDoubles` | `ctx.d[0]` |
| [test_e2e_fp_misc.cpp](../../tests/integration/test_e2e_fp_misc.cpp) | `E2EFPMisc.AbsDouble` | `ctx.d[0]` |
| 同上 | `E2EFPMisc.ConditionalSelect` | `ctx.f[2]` |
| 同上 | `E2EFPMisc.ConvertDoubleToSingle` | `ctx.f[0]` |
| [test_e2e_fp_round.cpp](../../tests/integration/test_e2e_fp_round.cpp) | `E2EFPRound.SqrtSingle` | `ctx.f[0]` |
| 同上 | `E2EFPRound.FrintVariants` | `ctx.f[1]..ctx.f[4]` |
| [test_codegen.cpp](../../tests/unit/codegen/test_codegen.cpp) | 字符串断言（当前 L247–L487，约 20 条）：FCVTZS 夹取、Neg/Abs、Select、Convert、Sqrt、Frint 各 lowering | 断言目标字符串整体改为 sr_v_* 形式 |

**不变项**：

- tests/fixtures/translator/golden/*.json 全部**不变**——JSON 记录的是带 F32/F64
  类型的寄存器索引常量，寄存器银行物理布局不进入 IR JSON；
- translator golden 集成测试（AllInstructionsMatchFixture）与 decode 单测
  预期不变；
- ConstantCache（regs/fregs/dregs/small_imm）不变。

**阶段 1 新增测试**：寄存器别名语义专项（见 6.1）。

---

## 4. IR 向量类型与 opcode

### 4.1 类型系统：V128 + lane kind

在 [ir.h](../../include/switchrecomp/ir/ir.h) 的 TypeId 增加 `V128`，
并在 Type 上增加 lane 元数据：

```cpp
enum class TypeId : u8 { /* ...existing..., */ V128 };

enum class LaneKind : u8 {
    None,
    F32x4,   // 4 lanes, esize 32, float
    F64x2,   // 2 lanes, esize 64, double
    I32x4,   // 4 lanes, signed 32
    I64x2,
    I16x8,
    I8x16,
    // F16x8 / BF16x8 后续
};

struct Type {
    TypeId id;
    LaneKind lane = LaneKind::None;
    static Type V128(LaneKind k) { return {TypeId::V128, k}; }
    // 既有工厂保持 lane=None，调用点零改动
};
```

- 元数据放在 **Type 而非 instruction**：operand Stringify 需要 lane 布局
  （lane 数、esize），且同一指令的 dst/src lane kind 一致；
- LaneKind 可派生 lanes / esize / 元素 TypeId，由一张小表集中描述。

### 4.2 向量常量

Constant 当前仅承载 u64。扩展为可承载 128-bit payload：

```cpp
class Constant : public Value {
    u64 value_;
    u64 value_hi_ = 0;   // V128 常量的高 64 位
public:
    Constant(u64 lo, u64 hi, Type type);   // 向量常量工厂
};
```

### 4.3 JSON 表示

类型与常量序列化建议（to_json/from_json 需对应扩展）：

```json
{
  "op": "Add",
  "type": "V128:F32x4",
  "operands": [
    { "kind": "reg", "index": 0, "type": "V128:F32x4" },
    { "kind": "reg", "index": 1, "type": "V128:F32x4" },
    { "kind": "reg", "index": 2, "type": "V128:F32x4" }
  ]
}
```

MOVI 的向量立即数：

```json
{ "kind": "constant", "type": "V128:F32x4", "value_lo": "0x....", "value_hi": "0x...." }
```

标量 JSON 表示保持现状（`"F32"`、`"F64"`），旧 golden 无需改动。

### 4.4 opcode 组织：复用为主、新增为辅（推荐）

**推荐：复用现有标量 opcode + lane 元数据**，而非为每条指令引入独立向量
opcode。理由：

1. AdvSimd 向量算术的**逐 lane 语义与标量版本完全相同**（FADD/FMUL/FSQRT/
   FRINT*/FNEG/FABS/FCVT…），没有新的运算定义，只有并行度；
2. codegen 已按 `i->type()` 分派（F32/F64/整数），增加 V128 分支是同一模式的
   延伸，降级表无需乘上「标量/向量」二维；
3. 独立向量 opcode 会造成 30+ 个枚举/字符串/Builder/降级重复（ARM AdvSimd 仅
   算术就有 Add/Sub/Mul 及各种变体），golden、verify、序列化全部膨胀。

复用/新增清单：

| opcode | 向量用途 | 说明 |
|--------|----------|------|
| Add / Sub / Mul / Div | F32×4、F64×2 逐 lane 算术 | type=V128:lane kind；操作数仍为 `{dst 寄存器常量, src1, src2}` 显式形式 |
| Neg / Abs | 逐 lane FNEG/FABS | `{dst, src}` |
| Sqrt / FrintN / FrintP / FrintM / FrintZ / FrintA | 逐 lane 1-source | 固定舍入模式同标量版 |
| Mov | (a) 全 V 寄存器拷贝；(b) **MOVI**：`Mov(V128 寄存器常量, V128 常量)` | MOVI 无需独立 opcode |
| Load / Store | **LD1/ST1**（16-byte 连续访存，结果/值类型 V128） | 向量访存语义见 4.6 |
| **Broadcast（新增）** | **FDUP**：`Vd.4S, Sn`（标量广播）与 `Vd, Vn.S[lane]`（lane 广播） | 操作数 `{dst VReg 常量, src}`；src 为 FReg 或 `{VReg, lane 索引常量}` |
| **InsertLane（新增）** | **INS**：`Vd.S[i], Wm` / `Vd.S[i], Vn.S[j]` | `{dst, lane 常量, src}`；仅改目标 lane |
| **ExtractLane（新增）** | **UMOV**：`Wd, Vn.S[i]`（及整数 lane） | `{dst XReg 常量, src VReg, lane 常量}` |
| **CmpMask（新增）** | **FCMEQ/FCMGT/FCMLT**（FCMLE/FCMGE 为别名）：输出 lane 为全 1/全 0 的掩码向量 | `{dst VReg, 谓词常量(U8), lhs, rhs/0 标记}` |

关于 FCMP 的特别说明：**不存在「向量 FCMP 写 NZCV」指令**。向量比较的结果是
lane 掩码（CmpMask）；NZCV 仍只由标量 FCMP 写。阶段 4 的「FCMP 向量族」即指
CmpMask 族。

IRBuilder 对应增加：`CreateBroadcast / CreateInsertLane / CreateExtractLane /
CreateCmpMask`，以及向量常量工厂；既有 Create* 签名全部不变。

### 4.5 MOVI / FMOV / FDUP / INS / UMOV 语义要点

- **MOVI**：向量立即数。早期子集只需要 `MOVI Vd.4S, #imm`：imm8 经现有
  VFPExpandImm 展开为 f32 位模式后复制到 4 个 lane（等价于对 V128 常量的
  Mov）；MOVI 的 8/16-bit pattern、LSL shift、MVNI、`.2D` 64-bit pattern
  形式后续按需添加；
- **FMOV（广义）**：标量寄存器间/立即数（已实现，阶段 1 改为 set_s 语义）；
  `FMOV Wd, Sn` / `FMOV Sd, Wn`（GP↔V 标量搬运，位模式直拷）；
  `FMOV Vd.S[i], Wn`（等价 INS）；`FMOV Wn, Vd.S[i]`（等价 UMOV）——译码时
  可统一归并到 InsertLane/ExtractLane，避免冗余 opcode；
- **FDUP**：标量→全 lane（src 用 B/H/S/D 名）或 lane→全 lane；
- **INS**：只写指定 lane，其余 lane 与高位按其 lane-element 宽度保持（INS 是
  vector 元素插入，高位规则随向量操作——我们统一保持其余字节）；
- **UMOV**：从 lane 提取到 GP，零扩展写入目标 W（32-bit 形式）。

### 4.6 向量访存语义（LD1/ST1）

- `LD1 {Vd.4S}, [Xn]` / `ST1 {Vd.4S}, [Xn]`：连续加载/存储 16 字节；
- 地址经 GuestToHost 取得 host 指针；生成代码建议以定长 memcpy 表达
  （sr_v_load16/store16），host 编译器在 -O2 折叠为单条向量 load/store；
- **支持非对齐地址**（LD1/ST1 默认允许非对齐，除非 guest 显式开启对齐检查）；
- lane↔内存：小端逐元素连续映射，lane 0 在最低地址；
- 多寄存器结构形式（`LD1 {V0.4S,V1.4S,V2.4S,V3.4S},[Xn]`）与 post-index
  形式后续迭代。

---

## 5. codegen 路线候选

三条候选路线；生成代码均位于每个 fn 的 C 函数体内，不改变 GenerateC 的整体
组织（头、includes、glabel、函数包装），只改变向量指令的降级文本。

### 5.1 路线 A：逐 lane 标量 C + 自动向量化（pragma 辅助）

发射形式（以 FADD V0.4S, V1.4S, V2.4S 为例）：

```c
#pragma omp simd
for (int sr_i = 0; sr_i < 4; ++sr_i) {
    sr_v_set_lane_s(&ctx->v[0], sr_i,
        sr_v_get_lane_s(&ctx->v[1], sr_i)
      + sr_v_get_lane_s(&ctx->v[2], sr_i));
}
```

- **正确性风险**：
  - 语义逐 lane 复用已被现有标量 E2E 验证过的运算，正确性基础最好；
  - FRINTA：逐 lane `roundf` 即 ties-away，精确；FRINTN 逐 lane `rintf`（默认
    ties-to-even），限制与 [fp_rounding.md](fp_rounding.md) 相同；
  - NaN lane：普通 C 算子 NaN 透传，与标量路径一致；不使用 -ffast-math；
  - 整数 lane：有符号加法在 C 中存在溢出 UB——向量整数 ADD 为回绕语义，
    codegen 需以无符号/位运算发射或依赖编译器对向量循环的处理；饱和类指令
    （SQADD/UQADD）必须显式实现，不能直接用 C 算子（早期阶段不涉及）；
  - lane helper 的定长 memcpy/memset 在循环内带变量偏移时，编译器在
    常量 trip count（4/2）下通常完全展开为直接 lane 访问。
- **跨平台/跨编译器**：纯 C11 + 标准库，任何编译器（含 MSVC）正确；
  `#pragma omp simd` 在不支持时被忽略（GCC 给出 unknown-pragma 警告），
  **语义退化为标量循环，仍然正确**——这是该路线最重要的安全属性。
- **性能（-O2 预期）**：
  - GCC 12 起 -ftree-vectorize 在 -O2 默认开启，简单 lane 循环会被自动向量化
    为 addps/mulps 等（旧版 GCC 需 -O3）；配合 `#pragma omp simd` 与
    `-fopenmp-simd` 可在任意 -O 级别强制向量化；
  - trip count 为编译期常量 4/2，通常直接展开，简单算术的实测预期与手写
    intrinsics 基本一致；
  - **libm 调用（rintf/roundf/sqrtf…）逐 lane 出现时，自动向量化失效**
    （libmvec 需要 -ffast-math 等条件），FRINT/FSQRT 在该路线下等价于 4 次
    标量调用，是主要性能短板。
- **与现有架构契合度**：最高。LowerInstruction 增加「type==V128 → 发射循环
  模板」分支即可，Stringify/寄存器常量机制不动。
- **CI 矩阵成本**：零新增；不依赖新工具或新编译选项即可正确运行。
- **调试可读性**：好，lane 循环与普通 C 无异，可断点、可打印。

### 5.2 路线 B：host intrinsics + 中央映射表

发射形式：

```c
#if defined(__x86_64__) && defined(__SSE4_1__)
    _mm_storeu_ps(ctx->v[0].bytes,
        _mm_add_ps(_mm_loadu_ps(ctx->v[1].bytes),
                   _mm_loadu_ps(ctx->v[2].bytes)));
#elif defined(__aarch64__)
    vst1q_f32(ctx->v[0].bytes,
        vaddq_f32(vld1q_f32(ctx->v[1].bytes),
                  vld1q_f32(ctx->v[2].bytes)));
#else
    /* 路线 A 的逐 lane 代码 */
#endif
```

中央映射表以 `(Opcode, LaneKind)` 为键，分 x86/aarch64 两栏。需要的主要映射：

- x86（immintrin.h）：`_mm_add_ps/_mm_sub_ps/_mm_mul_ps/_mm_div_ps`
  （SSE）、`_mm_sqrt_ps`（SSE）、`_mm_round_ps`（FRINTN/P/M/Z；
  **SSE4.1**）、FCMP mask：`_mm_cmp_ps`（EQ/LT/LE/…）、loadu/storeu；
  F64×2：`_mm_add_pd/...pd`、`_mm_round_pd`；
- aarch64（arm_neon.h）：`vaddq_f32/vaddq_f64/...`、`vsqrtq_*`、
  `vrndnq/vrndpq/vrndmq/vrndzq/vrndaq_f32`（FRINTA 有**直接原生指令**）、
  `vceqq_f32/vcgtq_f32`、`vld1q/vst1q`。

- **正确性风险**：
  - **x86 没有 ties-away 舍入指令**：`ROUNDPS` 仅支持 nearest / +∞ / −∞ /
    toward-zero 四种模式，FRINTA 必须手写。推荐符号位混合序列：

    ```c
    /* 正数 lane 取 CEIL、负数 lane 取 FLOOR，按符号位 blendv */
    __m128 signbit = _mm_set1_ps(-0.0f);
    __m128 up = _mm_round_ps(x, _MM_FROUND_CEIL);
    __m128 dn = _mm_round_ps(x, _MM_FROUND_FLOOR);
    __m128 r  = _mm_blendv_ps(up, dn, _mm_and_ps(x, signbit));
    ```

    正确性论证：±0.5/±1.5/±2.5 → 远离 0；非半数（0.4/0.6/−0.4/−0.6）
    ceil/floor 给出正确最近整数；NaN 透传；±0/±Inf 原样；blendv 与 roundps
    均为 SSE4.1。需为上述全部边界值设置专门 E2E；
  - 整数溢出/饱和：x86 对 32-bit 无饱和加指令（仅 16-bit paddsw/paddusb），
    SQADD/UQADD 的 32/64-bit 形式需软件序列——早期子集不涉及，但中央表要
    预留；普通 lane ADD 回绕语义 x86 算子直接匹配；
  - NaN lane：cmpps/算术 lane 行为与 ARM 默认 NaN 模式一致（payload 位差异
    不可观察）；
  - MXCSR 与 FPCR 的 FTZ/异常掩码差异：当前不建模 FPSR、默认环境下功能
    不可观察。
- **跨平台/跨编译器**：immintrin.h 在 GCC/Clang/MSVC/MinGW 均可用；arm_neon.h
  在 GCC/Clang/MSVC-arm64 可用。**特性门控是硬约束**：x86-64 默认目标仅
  SSE2，`roundps/blendvps` 需 `__SSE4_1__`（host 编译加 -msse4.1 或对生成
  代码做宏检测），否则必须落到 A 路径；
- **性能**：最优，每条指令一一映射；避免 4 次 libm 调用（FRINT/FSQRT 收益
  最大）；V128 固定宽度，不主动使用 256-bit AVX（语义上仍处理 128-bit 数据）；
- **与现有架构契合度**：中。需要新增映射表、平台宏组织、intrinsics include
  发射、以及 v[].bytes 与 __m128 之间的 loadu/storeu 桥接；
- **CI 矩阵成本**：高。两套后端任一未被 CI 覆盖即会腐烂；x86 路径本机可测，
  aarch64 路径需交叉编译 + QEMU 用户态或真机，否则只能靠评审维持；
- **调试可读性**：中，intrinsics 名称可读，但宏分支与映射表增加跳转成本。

### 5.3 路线 C：GNU/Clang C 向量扩展

发射形式：

```c
typedef float sr_v4sf __attribute__((vector_size(16)));

*(sr_v4sf*)ctx->v[0].bytes =
    *(sr_v4sf*)ctx->v[1].bytes + *(sr_v4sf*)ctx->v[2].bytes;
```

- **正确性风险**：算术算子直接映射为打包指令，NaN lane 行为正常；向量整型
  溢出在 GCC/Clang 中按回绕处理（编译器直接发射 SIMD 指令，不产生 UB 陷阱）；
  **舍入族无通用语言算子**：FSQRT 可借 `__builtin_sqrt` 向量形式（GCC/Clang
  支持），但 FRINT* 无通用 builtin，仍需回到路线 B（intrinsics）或 A
  （libm）；
- **跨平台/跨编译器**：GCC、Clang（含 MinGW、Linux/arm64、Darwin）支持；
  **MSVC 不支持**。GCC 在 aarch64 host 上会自动把向量算子降级为 NEON 指令，
  即「一套语法跨 x86/arm64」，这是相对路线 B 的主要优势；
- **性能**：与 intrinsics 等同（addps/vadd 直接生成），且**不依赖优化器是否
  自动向量化**，任何 -O 级别都得到打包指令，性能可预期性强；
- **与现有架构契合度**：中。需发射 typedef 与指针 cast；**strict-aliasing
  风险**：把 uint8_t bytes 指针 cast 为向量类型指针再解引用，是典型
  type-punning，缓解手段：通过 memcpy 做载入/写回（增加一层 helper），或使用
  `__attribute__((may_alias))` typedef；
- OpenMP C 数组语法（`a[0:N]` 风格数组片段，源自 Cilk 数组表示法）非 C 标准
  内容、主要见于 Intel 工具链，不建议作为框架依赖；
- **CI 矩阵成本**：低（当前 CI 的 MinGW g++ 覆盖），但锁死「生成代码仅
  GCC/Clang 可编译」的假设；
- **调试可读性**：最好，一条 C 表达式对应一条向量指令。

### 5.4 候选对比

| 维度 | A 逐 lane | B intrinsics | C 向量扩展 |
|------|-----------|--------------|------------|
| 正确性兜底 | 最强（已验证语义复用） | 强（FRINTA 需手写） | 中（FRINT 缺口） |
| 编译器无关性 | 高（C11，含 MSVC） | 高（三家 intrinsic） | 低（无 MSVC） |
| 简单算术性能 -O2 | 高（依赖自动向量化） | 最高 | 最高（不依赖优化器） |
| FRINT/FSQRT 性能 | 低（逐 lane libm） | 最高 | 需借 A/B |
| 架构契合/改动面 | 小 | 大 | 中 |
| CI 新增成本 | 无 | 高（双后端） | 低 |
| 不支持环境下的行为 | 退化正确 | 宏回退 A | 编译失败 |

### 5.5 明确推荐：默认 A，精确语义/热点用 B 兜底（混合）

1. **阶段 1–4 全部以路线 A 落地**：它复用已验证的标量语义、编译器无关、
   无 CI 新增成本，且在不支持 pragma 的环境下退化正确，满足「自有向量程序
   闭环」验收没有任何障碍；
2. **路线 B 作为受控加速层**，通过 CodegenOptions（建议增加 VectorBackend：
   `Auto | Intrinsic`）与中央映射表引入，维护一份**按 (Opcode, LaneKind) 的
   启用白名单**，初始为空。首批候选：
   - **FRINTA（及 FRINT 族）x86 路径**：既需要精确 ties-away（roundps 不支持），
     又是逐 lane libm 的性能最差处，语义与性能双重理由；
   - **FSQRT 向量族**：视 benchmark 结果决定；
3. **每条 B 路径必须满足边界条件**：(a) 编译期特性宏存在（如 `__SSE4_1__`/
   `__aarch64__`），否则 `#else` 走 A；(b) 同一 guest 程序在 A/B 两条路径下
   E2E 输出逐 lane 等价（含全部 NaN/舍入边界）；(c) benchmark 实测收益 ≥ 2×；
4. **路线 C 不作为框架默认**：MSVC 不在当前 CI 但 C 后端标榜可移植性，且 FRINT
   仍需求 B；若未来编译器矩阵明确收敛到 GCC/Clang，可用 C 重写 B 中纯算术
   部分（省掉 per-arch 表），作为后续可选优化重新评估。

---

## 6. 分阶段计划

每阶段可直接作为 TaskSpec 蓝本，字段：范围 / 输入 / 输出 / 验收 / 测试。

### 6.1 阶段 1：寄存器统一

- **范围**：sr_vreg + vreg.h；GuestContext 字段统一；codegen 全部标量 S/D 降级
  改走 sr_v_*；删除 fp/f/d；更新全部受影响测试字符串。
- **输入**：现有代码库与全部标量 E2E。
- **输出**：以 v[32] 为唯一 SIMD&FP 真相的代码库。
- **验收**：全部既有单测/集成/E2E 保持绿（golden JSON 零改动）；
  GeneratedCSyntaxValid（gcc C）通过。
- **测试（新增别名语义专项）**：
  - 写 s0 后读 v0.bytes[4..15] 全 0；写 d0 后读 bytes[8..15] 全 0；写 h/b
    对应高位全 0；
  - 标量序列：写 s1（非 v0）不影响 v0；FMOV Sd,Sn 后高位为 0；
  - 混合：先写 d0 再写 s0，验证 d0 高 32 位已随 s0 写被清零。

### 6.2 阶段 2：向量 IR + MOVI/FDUP/INS/UMOV + 4-lane FADD

- **范围**：TypeId::V128 + LaneKind + Type/JSON 扩展；128-bit 常量；
  Broadcast/InsertLane/ExtractLane opcode 与 Builder；译码 MOVI/FDUP/INS/UMOV
  与 FADD Vd.4S；codegen 路线 A 的向量循环模板。
- **输入**：手工编码的 guest 向量程序机器码（VFPExpandImm 复用）。
- **输出**：可执行的 4-lane 向量加法闭环。
- **验收**：guest 程序 `MOVI + FADD(Vd.4S)` 运行后 host main **打印 4 个 lane**，
  与手工期望值逐 lane 相等；JSON golden（movi/fdup/ins/umov/fadd_4s）。
- **测试**：新 E2E 文件（如 test_e2e_simd.cpp，无 g++ 时 skip）；decode 单测
  覆盖 Q bit 与 lane 字段抽取；IR 序列化单测覆盖 "V128:F32x4"。

### 6.3 阶段 3：向量访存 LD1/ST1

- **范围**：Load/Store V128 降级（LD1/ST1 single-structure）；地址经
  GuestToHost；定长 memcpy 发射；非对齐支持。
- **输入**：guest 程序先 ST1 将向量写入 guest 缓冲区，再做 LD1 读回。
- **输出**：向量与内存间的双向搬移闭环。
- **验收**：ST1 后 guest 内存 16 字节内容正确（lane 顺序小端），LD1 读回后
  逐 lane 等于原向量；非对齐地址用例通过。
- **测试**：E2E（含对齐/非对齐两个用例）、golden JSON。

### 6.4 阶段 4：FSQRT/FRINT/FCMP 向量族

- **范围**：FSQRT 与 FRINTN/P/M/Z/A 的 V128 降级（路线 A 逐 lane）；CmpMask
  （FCMEQ/FCMGT/FCMLT 及别名）与掩码结果消费（配合 INS/UMOV/FDUP 做断言）；
  F64×2 形式。
- **输入**：覆盖舍入边界的 guest 向量程序。
- **输出**：舍入/平方根/比较掩码向量闭环。
- **验收**：逐 lane 结果正确——FRINTA 精确 ties-away（±.5 边界）、FRINTN
  ties-to-even、FSQRT 含 ±0/Inf/NaN、掩码 lane 全 0/全 1 且 NaN 输入行为正确；
  UBSan 编译通过（整数 lane 路径）。
- **测试**：E2E 多用例 + decode/golden；此阶段结束后，按 5.5 规则评估是否将
  FRINTA x86 路径升级为路线 B（首个 B 白名单条目）。

---

## 7. 风险与缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| 寄存器别名回归（标量写未清高位、lane 写误伤其余 lane） | 混合标量/向量程序错误 | 阶段 1 别名矩阵测试（B/H/S/D × 读高位）；code review 中检查每条标量写是否走 set_*；set_lane_* 与 set_* 命名/语义分离 |
| AArch32 语义误用（写 S 保持高位） | 移植/编码错误 | 文档明确声明不支持 AArch32；译码表不接 A32/T32；相关用例命名标注 AArch64-only |
| host 编译器版本差异 | 性能波动或编译失败 | CI 固定 MinGW UCRT GCC 版本；记录 GCC<12 需 -O3 或 `-fopenmp-simd`；route A 保证无新选项也正确；roundps 路径以 `__SSE4_1__` 门控 + A 回退；记录 clang on Windows 为可选观测项 |
| x86 ties-away 实现错误 | FRINTA 向量结果错 | 采用 5.2 的 ceil/floor 符号 blend 序列；E2E 覆盖 ±0.5/±1.5/±2.5、非半数、NaN、±0、Inf、超大值；先在标量 host 程序中对拍 roundf |
| 自动向量化未触发导致性能低于预期 | 吞吐不达标 | pragma + 常量 trip count；CI 检查关键 E2E 生成汇编是否含打包指令（可选）；性能不达标时按白名单升级路线 B，正确性不受影响 |
| strict-aliasing（尤其未来路线 C） | -O2 下错误结果 | 阶段 1 即以 memcpy helper 定调；路线 C 若启用必须 may_alias 或 memcpy |
| 整数饱和/溢出（后续指令类） | lane 结果错 | 早期子集回避；SQADD/UQADD 引入时逐宽度显式实现 + UBSan/对拍 |
| B 路径双后端腐烂 | aarch64 映射失修 | B 白名单制度：未在 CI 实际执行的路径不得加入白名单；aarch64 需 QEMU 用户态/真机 CI 后才启用 |

**性能 benchmark 计划**：

- 独立 microbench（不进功能 CI，按需运行）：固定长度向量数组（如 1M 个 V128），
  操作集 FADD/FMUL/FSQRT/FRINTA/CmpMask/LD1+ST1；
- 对照组：路线 A（-O2 与 -O3 各一次）、路线 B（启用时）、标量算子 ×4 基线；
- 指标：ns/指令、lane 吞吐（lanes/cycle）、自动向量化命中率（检查汇编）；
- 平台：x86-64（UCRT64 GCC，可选 clang）；aarch64 如有机则真机测量
  （QEMU 仅做正确性，不做性能计时）；
- 结论以「是否达到 B 白名单准入（≥2×）」的形式回流 codegen 决策。

---

## 8. 资料来源

1. **Arm Architecture Reference Manual for A-profile architecture（ARM ARM，
   DDI 0487，最新版）**：SIMD&FP register file（别名与写高位规则）、
   FADD/FMOV/FDUP/INS/UMOV/MOVI（vector immediate）/LD1/ST1 各条伪代码、
   FRINTA/FRINTN 舍入定义。
2. **Arm A-profile Instruction Set Architecture XML（DDI 0602）**：
   指令编码与伪代码机器可读形式（交叉核对字段位段）。
3. **QEMU（9.x）**：`target/arm/translate-a64.c`、`a64.decode`、
   `disas_fp_1src/2src`、LD1/ST1 与 MOVI 译码组织（编码交叉核对参照）。
4. **Arm, Chris Shore：《Porting to 64-bit Arm》白皮书（2014）**：
   寄存器映射变化与 32-bit 写清高位规则。
5. **Arm Neon Intrinsics Reference**
   （developer.arm.com/architectures/instruction-sets/intrinsics）：
   arm_neon.h 函数与 FRINT 族 intrinsic。
6. **Arm Neon Programmer's Guide**：lane/向量编程模型。
7. **Intel® 64 and IA-32 SDM（Vol.1/Vol.2）**：ADDPS/MULPS/SQRTPS/ROUNDPS
   （四种舍入模式、无 ties-away）、CMPPS、BLENDVPS 与 SSE/SSE4.1 特性集。
8. **GCC 手册**：C 语言向量扩展（vector_size）、OpenMP 实现状态、
   `#pragma GCC ivdep`、向量整型回绕行为。
9. **Clang 文档 / 发行说明**：OpenMP 与 `-fopenmp-simd`（Clang 6 起）、
   向量扩展与 pragma 支持。
10. **Microsoft Learn：ARM64 ABI conventions**：V 寄存器视图（v/q/d/s/h/b）
    与 Windows on ARM64 约束。
11. **switchbrew（Switch Brew Wiki）**：Switch 硬件（Tegra X1 / Cortex-A57，
    小端）与指令集环境背景。
12. Berger 等：《Translation Validation for LLVM's AArch64 Backend》
    （ARM-TV, 2025）：AArch64 后端正确性风险背景参考。

---

## 9. 架构师决策点

已于 2026-10-08 全部裁决，见文首「架构师决策」小节（6 条）。阶段 1 据此
实现完毕；阶段 2 起按第 6 节计划与决策 6 的范围（仅 FP）推进。
