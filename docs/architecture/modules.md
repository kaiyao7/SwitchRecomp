# SwitchRecomp 模块职责与依赖关系

> 版本：v0.1

## 1. 模块清单

### 1.1 common（通用基础库）

**职责**：跨模块通用工具，不含业务逻辑。
- 日志（`Log`）
- 错误类型（`Result`、`Error`）
- 基础类型（`span`、`bit` 操作、`aligned_storage`）
- 字符串、文件路径、CRC/hash

**允许依赖**：C++ 标准库
**禁止依赖**：ir / translator / codegen / runtime / gpu / platform
**目录**：`src/common`、`include/switchrecomp/common`

### 1.2 platform（OS 抽象层）

**职责**：封装宿主操作系统差异，提供最小 API 集合。
- 内存：`VirtualAlloc`/`mmap` 封装，页权限（RWX），大页
- 线程：`Thread`、`Mutex`、`ConditionVariable`、`Semaphore`
- 文件：`File`、`MappedFile`、目录遍历
- 时间：`SteadyClock`、高精度计时
- 动态库加载：`DynamicLibrary`

**允许依赖**：common、系统 API（Win32 / POSIX）
**禁止依赖**：ir / translator / codegen / runtime / gpu
**目录**：`src/platform`、`include/switchrecomp/platform`

### 1.3 ir（中间表示）

**职责**：整个重编译流水线的中心数据结构。
- 类型系统：`Type`（u8/u16/u32/u64/s*/f*/vec/ptr）
- 值：`Value`（SSA 值，含常量、参数、指令结果）
- 指令：`Instruction` 及 opcode 表（算术、访存、控制流、调用）
- 基本块：`BasicBlock`
- 函数：`Function`
- 模块：`Module`
- 序列化：`to_json` / `from_json`、二进制 dump
- 验证：`verify(module)` 检查 SSA 正确性、类型一致

**允许依赖**：common、platform（仅基础类型，如 PageSize）
**禁止依赖**：translator / codegen / runtime / gpu
**目录**：`src/ir`、`include/switchrecomp/ir`

### 1.4 translator（ARM64 → IR）

**职责**：将 ARM64 指令流翻译为 IR。
- ARM64 指令解码器（取指、查表、字段提取）
- 指令语义 → IR 指令 lowering
- 标志位（NZCV）建模与惰性传播
- 基本块切分（分支、异常边界）
- 函数边界检测（启发式 + 符号）

**允许依赖**：ir、common
**禁止依赖**：codegen / runtime / gpu（保证翻译器可独立测试）
**目录**：`src/translator`、`include/switchrecomp/translator`

### 1.5 codegen（IR → C/C++）

**职责**：将 IR 降级为宿主可编译的 C/C++ 源码。
- IR → C 代码生成（表达式、语句、函数签名）
- 运行时辅助函数调用约定（`sr_rt_*`）
- 寄存器/状态映射（guest regs → C struct）
- 控制流还原（branch → goto/if）
- 输出可被 gcc/clang/msvc 直接编译的 `.c` 文件

**允许依赖**：ir、common
**禁止依赖**：translator / runtime / gpu
**目录**：`src/codegen`、`include/switchrecomp/codegen`

### 1.6 runtime（运行时兼容层）

**职责**：为重编译后的原生代码提供 Switch 运行时环境。
- 内存管理：guest 虚拟地址空间、页表、mirror、fastmem
- CPU 状态：`GuestContext`（x0–x30, sp, pc, pstate, fpsimd）
- 线程：guest thread → host thread 映射、调度
- 同步：mutex、condvar、semaphore、event（Switch SVC 语义）
- IPC：HLE 服务骨架（仅接口，逐步填充）
- 文件系统：VFS 抽象
- SVC handler：`svc` 指令 → runtime 函数

**允许依赖**：platform、ir（仅类型定义）、common
**禁止依赖**：translator / codegen / gpu（runtime 不关心代码怎么来的）
**目录**：`src/runtime`、`include/switchrecomp/runtime`

### 1.7 gpu（GPU 抽象层）

**职责**：处理 Switch GPU（Maxwell）命令，抽象为跨后端接口。
- GPU 命令处理器（FIFO、macro、method 分发）
- Shader 翻译（Maxwell → SPIR-V，占位/研究）
- 后端抽象：`GPUBackend` 接口
- 后端实现：Vulkan（优先）、OpenGL、D3D12（后续）
- 内存：通过 runtime 的 guest memory 读取纹理/顶点

**允许依赖**：runtime（内存访问）、platform、common
**禁止依赖**：translator / codegen
**目录**：`src/gpu`、`include/switchrecomp/gpu`

## 2. 依赖关系图

```
common ◀── platform ◀── runtime ◀── gpu
  ▲          ▲           ▲
  │          │           │
  └──── ir ──┘           │
        ▲   ▲            │
        │   │            │
   translator  codegen    │
```

**核心规则**：
- `ir` 是枢纽，`translator` 和 `codegen` 只看 `ir`。
- `runtime` 不依赖 `translator`/`codegen`：运行时不需要知道代码怎么生成的。
- `gpu` 只通过 `runtime` 访问 guest 内存，不直接碰 `translator`/`codegen`。
- `platform` 在最底层，纯 OS 封装。

## 3. 禁止依赖矩阵

行 = 依赖方，列 = 被依赖方。✗ = 禁止。

|            | common | platform | ir | translator | codegen | runtime | gpu |
|------------|:------:|:--------:|:--:|:----------:|:-------:|:-------:|:---:|
| common     |   —    |    ✗     | ✗  |     ✗      |    ✗    |    ✗    |  ✗  |
| platform   |   ✓    |    —     | ✗  |     ✗      |    ✗    |    ✗    |  ✗  |
| ir         |   ✓    |    ✓     | —  |     ✗      |    ✗    |    ✗    |  ✗  |
| translator |   ✓    |    ✗     | ✓  |     —      |    ✗    |    ✗    |  ✗  |
| codegen    |   ✓    |    ✗     | ✓  |     ✗      |    —    |    ✗    |  ✗  |
| runtime    |   ✓    |    ✓     | ✓  |     ✗      |    ✗    |    —    |  ✗  |
| gpu        |   ✓    |    ✓     | ✗  |     ✗      |    ✗    |    ✓    |  —  |

## 4. 边界执行机制

- 每个模块的 `CMakeLists.txt` 只链接允许依赖的 target。
- CI 中运行 `scripts/check_deps.py` 静态检查 `#include` 白名单。
- Code review 时检查依赖矩阵。
