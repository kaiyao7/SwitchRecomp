# 五个 Agent 第一批任务

> 版本：v0.1
> 每个任务包含：目标、输入、输出、修改目录、依赖、验收标准、测试方法。
> 任务编号规则：`<Agent>-<Phase>-<序号>`。

---

## Agent：Recompiler（ARM64/IR）

### T-RC-1-01：IR 核心数据结构实现

- **目标**：实现 `ir::Type / Value / Instruction / BasicBlock / Function / Module`。
- **输入**：`docs/architecture/interfaces.md` 中的 IR 接口定义。
- **输出**：
  - `src/ir/ir.cpp` 及内部实现头文件
  - IR 构建 API（创建指令、添加操作数、组装基本块/函数/模块）
- **修改目录**：`src/ir/`、`include/switchrecomp/ir/`
- **依赖**：common（types, result）
- **验收标准**：
  - 能程序化构建一个含 1 个函数、2 个基本块、3 条指令的 Module。
  - `module.verify()` 对结构正确的 Module 返回 true，对破坏 SSA 的返回 false。
- **测试方法**：`tests/unit/ir/test_ir_build.cpp` 用 GTest 构建并验证。

### T-RC-1-02：IR JSON 序列化

- **目标**：实现 `Module::to_json()` 与 `Module::from_json()`。
- **输入**：T-RC-1-01 的 Module 对象。
- **输出**：`src/ir/serialize.cpp`。
- **修改目录**：`src/ir/`
- **依赖**：T-RC-1-01
- **验收标准**：`to_json` → `from_json` → `to_json` 输出与原始 JSON 一致（round-trip）。
- **测试方法**：`tests/unit/ir/test_ir_serialize.cpp` round-trip 测试。

### T-RC-1-03：ARM64 解码器骨架 + 5 条指令

- **目标**：实现指令取指与查表解码框架，支持 `add / sub / mov / b / nop`。
- **输入**：ARMv8-A ARM 指令编码表；字节码样本。
- **输出**：`src/translator/decode.cpp`、`src/translator/decode_table.inl`。
- **修改目录**：`src/translator/`、`include/switchrecomp/translator/`
- **依赖**：T-RC-1-01（ir::Module 构建 API）
- **验收标准**：给定 5 条指令的字节码，`Translate` 返回对应 IR，`verify()` 通过。
- **测试方法**：`tests/unit/translator/test_decode.cpp` 断言每条指令的 IR opcode 与操作数。

### T-RC-1-04：扩展至 20 条核心指令 + Golden 数据

- **目标**：在 T-RC-1-03 基础上扩展到 20 条指令（见 roadmap Phase 1 清单），并生成 golden 文件。
- **输入**：T-RC-1-03 框架；ARM 手册。
- **输出**：`tests/fixtures/translator/golden/*.json`（每条指令一个 golden）。
- **修改目录**：`src/translator/`、`tests/fixtures/translator/`
- **依赖**：T-RC-1-03
- **验收标准**：20 条指令全部通过 golden 比对。
- **测试方法**：`tests/integration/translator/test_golden.cpp` 遍历 golden 目录比对。

---

## Agent：Runtime

### T-RT-0-01：platform 层最小实现（内存 + 线程）

- **目标**：实现 `platform::AllocPages / FreePages / ProtectPages / PageSize` 与 `Thread`/`Mutex`。
- **输入**：`include/switchrecomp/platform/memory.h`、`thread.h` 接口。
- **输出**：`src/platform/memory_win.cpp`、`memory_posix.cpp`、`thread_win.cpp`、`thread_posix.cpp`。
- **修改目录**：`src/platform/`
- **依赖**：common
- **验收标准**：
  - 分配 1 页内存，写入数据，改保护为只读，写入触发异常（Windows SEH / POSIX signal）。
  - 创建线程，主线程 join 后子线程已执行完毕。
- **测试方法**：`tests/unit/platform/test_memory.cpp`、`test_thread.cpp`。

### T-RT-3-01：GuestContext 与内存管理骨架

- **目标**：定义完整 `GuestContext`（含 fpsimd），实现 softmmu 页表骨架。
- **输入**：`include/switchrecomp/runtime/runtime.h`。
- **输出**：`src/runtime/guest_context.h`、`src/runtime/memory_manager.cpp`。
- **修改目录**：`src/runtime/`、`include/switchrecomp/runtime/`
- **依赖**：T-RT-0-01
- **验收标准**：
  - `GuestContext` 大小固定（assert sizeof）。
  - 能创建 guest 地址空间，map 一段内存，`GuestToHost` 返回正确宿主指针。
- **测试方法**：`tests/unit/runtime/test_memory_manager.cpp`。

### T-RT-3-02：SVC handler 骨架

- **目标**：实现 `HandleSvc` 分派框架，支持 0–3 号 SVC 的占位实现。
- **输入**：Switch SVC 表（公开资料）。
- **输出**：`src/runtime/svc_table.cpp`。
- **修改目录**：`src/runtime/`
- **依赖**：T-RT-3-01
- **验收标准**：guest 执行 `svc #0` 时 runtime 调用对应 handler 且不崩溃。
- **测试方法**：`tests/integration/runtime/test_svc.cpp`（配合 recomp_cli 运行含 svc 的字节码）。

---

## Agent：Graphics

### T-GPU-4-01：GPU 后端接口 + Vulkan 初始化

- **目标**：实现 `GPUBackend` 接口与 Vulkan 后端的 Instance/Device 初始化。
- **输入**：`include/switchrecomp/gpu/gpu.h`；Vulkan SDK。
- **输出**：`src/gpu/vulkan_backend.cpp`。
- **修改目录**：`src/gpu/`
- **依赖**：platform（无 Vulkan 时需安装 Vulkan SDK）
- **验收标准**：
  - `CreateVulkanBackend()->Initialize()` 在有 Vulkan 的环境返回成功。
  - 无 Vulkan 环境返回明确错误而非崩溃。
- **测试方法**：`tests/unit/gpu/test_vulkan_init.cpp`（环境无 Vulkan 时 skip）。

### T-GPU-4-02：命令处理器骨架

- **目标**：实现 GPU FIFO 命令的读取与 method 分发骨架（空 method 实现）。
- **输入**：Maxwell FIFO 文档（公开）。
- **输出**：`src/gpu/command_processor.cpp`。
- **修改目录**：`src/gpu/`
- **依赖**：runtime（GuestToHost 读取命令列表）
- **验收标准**：给定一段 guest 命令缓冲区，能遍历并调用对应 method 占位。
- **测试方法**：`tests/unit/gpu/test_command_processor.cpp`。

---

## Agent：Build/Test

### T-BT-0-01：CMake 工程骨架

- **目标**：建立可构建的 CMake 工程，各模块为独立 target，依赖关系由 CMake 强制。
- **输入**：模块依赖矩阵（`docs/architecture/modules.md`）。
- **输出**：根 `CMakeLists.txt`、各模块 `CMakeLists.txt`、`cmake/` 工具脚本。
- **修改目录**：`CMakeLists.txt`、`src/*/CMakeLists.txt`、`cmake/`
- **依赖**：无
- **验收标准**：
  - `cmake -B build -G Ninja && cmake --build build` 成功。
  - 每个模块 target 只链接允许依赖的 target（CMake target_link_libraries 控制）。
- **测试方法**：本地构建 + CI 构建。

### T-BT-0-02：GTest 集成 + CI 骨架

- **目标**：集成 GoogleTest（CPM 或 FetchContent），建立 GitHub Actions CI。
- **输入**：T-BT-0-01。
- **输出**：`tests/CMakeLists.txt`、`.github/workflows/ci.yml`。
- **修改目录**：`tests/`、`.github/`
- **依赖**：T-BT-0-01
- **验收标准**：
  - 一个空测试 `tests/unit/test_smoke.cpp` 能被 ctest 运行并通过。
  - CI 在 Windows（MSVC）与 Linux（gcc）上跑通构建 + 测试。
- **测试方法**：推送分支，看 CI 结果。

### T-BT-0-02b：依赖白名单检查脚本

- **目标**：写 Python 脚本检查 `include/` 与 `src/` 的 `#include` 是否符合依赖矩阵。
- **输入**：依赖矩阵。
- **输出**：`scripts/check_deps.py`。
- **修改目录**：`scripts/`
- **依赖**：无
- **验收标准**：对违规 `#include` 报错退出码非零。
- **测试方法**：`scripts/check_deps.py` 在 CI 中运行。

---

## Agent：Research

### T-RS-1-01：ARM64 指令编码表整理

- **目标**：整理 Phase 1 所需 20 条指令的编码、字段、伪代码，输出为机器可读格式。
- **输入**：ARMv8-A ARM（公开）。
- **输出**：`docs/design/arm64_instructions.md` 或 `tools/arm64_table.json`。
- **修改目录**：`docs/design/` 或 `tools/`
- **依赖**：无
- **验收标准**：每条指令含：mnemonic、opcode 位域、操作数类型、影响的标志位、伪代码摘要。
- **测试方法**：Recompiler Agent 用此表实现 T-RC-1-04，能正确解码即视为通过。

### T-RS-3-01：Switch SVC 与内存布局资料整理

- **目标**：整理 Switch 用户态 SVC 列表与进程虚拟地址空间布局。
- **输入**：Switch 开发公开资料（switchbrew 等）。
- **输出**：`docs/design/switch_svc.md`、`docs/design/switch_memory_layout.md`。
- **修改目录**：`docs/design/`
- **依赖**：无
- **验收标准**：列出 SVC 号、名称、参数、行为摘要；内存布局含各段地址范围与权限。
- **测试方法**：Runtime Agent 用此资料实现 T-RT-3-02，能识别主要 SVC。

### T-RS-4-01：Maxwell GPU 命令与 shader 资料整理

- **目标**：整理 Maxwell FIFO 命令格式与 shader ISA 入门资料。
- **输入**：Nvidia Maxwell 公开文档、switchbrew。
- **输出**：`docs/design/maxwell_commands.md`、`docs/design/maxwell_shader_isa.md`。
- **修改目录**：`docs/design/`
- **依赖**：无
- **验收标准**：FIFO 命令结构（method 号、subchannel、参数）清晰；shader ISA 至少覆盖 10 条常见指令编码。
- **测试方法**：Graphics Agent 用此资料实现 T-GPU-4-02。

---

## 任务依赖图

```
T-BT-0-01 ─▶ T-BT-0-02 ─▶ (所有 Agent 的测试可运行)
T-RS-1-01 ─▶ T-RC-1-03 ─▶ T-RC-1-04
T-RC-1-01 ─▶ T-RC-1-02 ─▶ T-RC-1-03
T-RT-0-01 ─▶ T-RT-3-01 ─▶ T-RT-3-02
T-RS-3-01 ──────────────▶ T-RT-3-02
T-RS-4-01 ─▶ T-GPU-4-02
T-GPU-4-01 ─▶ T-GPU-4-02
T-RT-3-01 ─▶ T-GPU-4-02
```

**可并行**：T-BT-0-01、T-RC-1-01、T-RT-0-01、T-RS-1-01、T-RS-3-01、T-RS-4-01 之间无依赖，可同时启动。
