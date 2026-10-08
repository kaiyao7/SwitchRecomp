# ARM64 → IR → C/C++ → Native 重编译流水线

> 版本：v0.1

## 1. 流水线概览

```
┌──────────┐   translator   ┌────────┐   codegen    ┌────────┐   host CC    ┌────────┐   runtime
│ ARM64 ELF│ ─────────────▶ │ IR     │ ───────────▶ │ C/C++  │ ───────────▶ │ native │ ────────▶ running
│ /bin     │                │ Module │              │ source │              │ .so/.o │
└──────────┘                └────────┘              └────────┘              └────────┘
```

四阶段：**解码翻译 → IR 优化 → C 代码生成 → 宿主编译**。

## 2. 阶段详细设计

### 2.1 translator：ARM64 → IR

**输入**：
- ARM64 字节码缓冲区（`span<const u8>`）
- 起始 guest 地址（`u64`）
- 可选：符号表、函数边界提示

**输出**：`ir::Module`（一个或多个 `ir::Function`）

**步骤**：
1. **取指解码**：按 4 字节对齐读取 ARM64 指令，用查表法（`decode_arm64(u32 insn)`）得到指令类型与操作数。
2. **基本块切分**：遇到分支（b/bl/b.cond/cbnz/...）或异常指令（svc/brk）切分基本块。
3. **语义 lowering**：每条 ARM64 指令 → 一条或多条 IR 指令。
   - 例：`add x0, x1, x2` → `%v = ir::Add(u64, %x1, %x2)`
   - 标志位：`adds` 额外计算 NZCV，存为 `ir::FlagSet`
4. **函数构造**：以 `ret` 或间接跳转结尾的基本块链作为一个函数。
5. **输出 IR Module**。

**关键设计**：
- 标志位惰性传播：只有后续指令消费 NZCV 时才计算，避免无谓指令。
- 不做控制流恢复（Phase 1 只做线性扫描 + 简单分支跳转表）。
- 间接跳转（`br x16`）暂作为 `ir::IndirectBranch` 占位，Phase 3 由 runtime 处理。

### 2.2 IR 优化（Phase 2+，可选）

- 常量折叠、死代码消除
- 标志位合并
- 寄存器分配提示

Phase 1–2 跳过优化，先确保正确性。

### 2.3 codegen：IR → C

**输入**：`ir::Module`
**输出**：C 源码字符串 / 文件

**步骤**：
1. **生成 guest 状态结构**：
   ```c
   typedef struct { uint64_t x[31], sp, pc; uint32_t nzcv; /* fpsimd... */ } GuestContext;
   ```
2. **函数签名**：每个 `ir::Function` → `void fn_<addr>(GuestContext* ctx)`。
3. **指令降级**：
   - `ir::Add(u64, a, b)` → `ctx->x[dst] = a + b;`
   - `ir::Load(addr)` → `*(uint64_t*)sr_rt_guest_to_host(ctx, addr)`
   - `ir::Branch(bb)` → `goto bb_<id>;`
   - `ir::Call(addr)` → `sr_rt_call(ctx, addr);`
4. **控制流**：基本块 → label + goto。
5. **运行时辅助**：访存、调用、SVC 都通过 `sr_rt_*` 函数，由 runtime 提供。

**关键设计**：
- 生成纯 C11，不依赖 C++ 特性（便于最大兼容性）。
- 所有 guest 内存访问必须经过 `sr_rt_guest_to_host` 转换层（安全 + fastmem 切换）。
- 输出文件可被 `gcc -O2 -c` 直接编译。

### 2.4 宿主编译

- 调用宿主编译器（gcc/clang/msvc）将生成的 `.c` 编译为 `.o` 或 `.so`。
- 链接 runtime 静态库 + gpu 后端。
- 由 `tools/recomp_cli` 编排整个流程。

## 3. IR 设计概要

### 3.1 类型系统

```
Type = Void | U8 | U16 | U32 | U64 | S8 | S16 | S32 | S64
     | F16 | F32 | F64 | Vec2x32 | Vec4x32 | Ptr
```

### 3.2 核心数据结构

```cpp
namespace ir {
class Value;          // SSA 值（常量/参数/指令结果）
class Instruction;    // 单条 IR 指令
class BasicBlock;     // 基本块（指令列表 + 终结指令）
class Function;       // 函数（基本块列表 + 参数）
class Module;         // 模块（函数列表 + 全局）
}
```

### 3.3 指令 opcode（Phase 1 最小集）

算术：`Add, Sub, And, Or, Xor, Not, Shl, Shr, Sar, Mul`
访存：`Load, Store`
控制流：`Branch, CondBranch, Switch, Return, Call, IndirectBranch`
标志：`FlagSet, FlagGet`
转换：`Cast, Trunc, ZExt, SExt`
特殊：`Nop, Undefined, Svc`

### 3.4 序列化

- JSON：可读，用于调试与 golden test
- 二进制：紧凑，用于缓存
- 接口：`module.to_json()` / `Module::from_json()`

## 4. 最小闭环（Phase 1 验收目标）

输入一条 ARM64 指令：
```
0x91000400   // add x0, x0, #1
```

流水线产出：
1. IR：`%0 = Add(u64, %x0_in, 1)`
2. C：`ctx->x[0] = ctx->x[0] + 1;`
3. 编译运行后 guest x0 正确 +1。

这是第一个端到端测试，详见 [test-strategy.md](./test-strategy.md)。

## 5. 性能考量（Phase 5 再深入）

- translator：解码表用 `std::array<Handler, 65536>` 按高 16 位索引，O(1)。
- codegen：字符串拼接用 `fmt::memory_buffer`，避免重复分配。
- 生成的 C 代码：用 `goto` 直接跳转基本块，避免 switch 开销；启用 `-O2`。
- runtime 访存：fastmem 模式下 guest 地址直接映射到宿主，零开销；softmmu 模式下查表。

## 6. 与 JIT 的关系

本项目优先 **AOT 重编译**（生成 C 再编译），理由：
- 可调试（生成 C 可读）
- 可移植（不依赖宿主 ISA 知识）
- 可利用宿主编译器优化

JIT（IR → 宿主机器码）作为 Phase 5 可选路径，接口预留 `codegen::JITBackend`。
