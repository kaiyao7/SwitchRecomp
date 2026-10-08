# 第 1 批跨域任务交接卡

> 以下任务不在本 Agent 职责范围内（runtime/graphics/build-test/非 ARM64 资料），
> 移交给对应 Agent。每张卡按 tasks.md 规范：目标/输入/输出/修改目录/依赖/验收标准/测试方法。

---

## T-RT-0-01 — platform 层最小实现（内存 + 线程）

**负责 Agent**：Runtime Agent

### 目标
实现 `platform::AllocPages / FreePages / ProtectPages / PageSize` 与 `Thread` / `Mutex` 的真实功能（替换当前 stub）。

### 输入
- 接口：`include/switchrecomp/platform/memory.h`、`thread.h`
- 现有 stub：`src/platform/memory.cpp`、`thread.cpp`

### 输出
- `src/platform/memory.cpp`：Windows 用 `VirtualAlloc/VirtualFree/VirtualProtect`；POSIX 用 `mmap/munmap/mprotect`
- `src/platform/thread.cpp`：Windows 用 `CreateThread` 或 `std::thread`；POSIX 用 `pthread`

### 修改目录
`src/platform/`

### 依赖
- common（types）

### 验收标准
1. `AllocPages(size, ReadWrite)` 返回页对齐非空指针，可读写。
2. `FreePages(ptr, size)` 释放成功。
3. `ProtectPages` 改为只读后写入触发访问违规（Windows SEH / POSIX SIGSEGV）。
4. `PageSize()` 返回系统页大小（Windows 4096）。
5. `Thread` 启动函数执行后 `Join()` 返回，且函数已执行。
6. `Mutex` + `MutexGuard` 多线程并发自增结果正确。

### 测试方法
- `tests/unit/platform/test_memory.cpp`：分配→写→改只读→验证（用 SEH 捕获写入异常）。
- `tests/unit/platform/test_thread.cpp`：10 线程 × 10000 次自增，最终 == 100000。

### 跨模块接口注意事项
- `AllocPages` 返回的内存必须页对齐且初始化为 0（Windows VirtualAlloc 自动零初始化；mmap 需 MAP_ANONYMOUS）。
- 头文件不动，仅实现 `.cpp`。

---

## T-RS-3-01 — Switch SVC 与内存布局资料整理

**负责 Agent**：Research Agent

### 目标
整理 Switch 用户态 SVC 列表与进程虚拟地址空间布局，供 Runtime Agent 实现 T-RT-3-02 使用。

### 输入
- Switchbrew 等公开资料（仅研究用途）

### 输出
1. `docs/design/switch_svc.md`：SVC 号 → 名称 → 参数 → 行为摘要表
2. `docs/design/switch_memory_layout.md`：进程地址空间各段（代码、堆、栈、共享内存、TLS）的地址范围与权限

### 修改目录
`docs/design/`

### 依赖
无

### 验收标准
1. SVC 表至少覆盖 0x00–0x3F 范围内的主要 SVC（SetHeapSize, SetMemoryPermission, MapMemory, CreateThread, StartThread, WaitSynchronization, SendSyncRequest, CloseHandle 等）。
2. 内存布局图含：主程序段（text/rodata/data/bss）、堆区、栈区、镜像区（alias/stack/heap/legacy）、TLS、IPC 缓冲区。
3. 每条 SVC 注明参数个数与返回值寄存器约定（X0–X7 传参，X0 返回）。

### 测试方法
- 文档评审：Runtime Agent 用此表实现 T-RT-3-02，能正确分派 5 个核心 SVC 即视为通过。

### 注意
- 只整理公开资料，不包含任何专有密钥或加密细节。

---

## T-RS-4-01 — Maxwell GPU 命令与 shader 资料整理

**负责 Agent**：Research Agent

### 目标
整理 Maxwell FIFO 命令格式与 shader ISA 入门资料，供 Graphics Agent 实现 T-GPU-4-02。

### 输入
- Nvidia Maxwell 公开文档、switchbrew GPU 页面

### 输出
1. `docs/design/maxwell_commands.md`：FIFO 命令结构（method 号、subchannel、参数、长包/非长包格式）
2. `docs/design/maxwell_shader_isa.md`：shader ISA 编码入门（至少 10 条常见指令：MOV, ADD, MUL, MAD, F2I, I2F, LD, ST, BRA, EXIT）

### 修改目录
`docs/design/`

### 依赖
无

### 验收标准
1. FIFO 命令条目清晰：能说明一条命令的 32-bit word 如何拆分为 method/subchannel/count/参数。
2. shader ISA 条目每条含：助记符、编码位域、操作数类型、伪代码。

### 测试方法
- 文档评审：Graphics Agent 用此资料实现 T-GPU-4-02，能解析简单命令缓冲区。

---

## T-BT-0-02（剩余部分）— GitHub Actions CI 骨架

**负责 Agent**：Build/Test Agent

### 目标
建立 GitHub Actions CI，在 Windows（MSVC）与 Linux（gcc）上跑通构建 + 测试。（GTest 集成部分已完成）

### 输入
- 现有 CMake 工程（已验证 MinGW UCRT64 构建 + 14 个测试通过）
- `scripts/check_deps.py`（已验证通过）

### 输出
`.github/workflows/ci.yml`，包含：
1. 触发：push 到 main、PR
2. Job 1：Windows-latest，MSVC，`cmake -B build -G "Visual Studio 17 2022"` → 构建 → ctest
3. Job 2：Ubuntu-latest，gcc，`cmake -B build -G Ninja` → 构建 → ctest
4. 可选 Job 3：依赖检查 `python scripts/check_deps.py`

### 修改目录
`.github/workflows/`

### 依赖
- T-BT-0-01（已完成）

### 验收标准
1. 推送分支后 CI 跑通，两个平台构建成功，测试全绿。
2. 依赖检查 job 失败能阻断合并。

### 测试方法
- 推送测试分支，观察 GitHub Actions 结果。

### 注意
- 当前本地已验证的工具链是 MinGW UCRT64 (GCC 16)，CI 上 Windows 用 MSVC 需额外验证编译兼容性（GCC 16 的 `using` 引入类型、`<cstdint>` 等 MSVC 应兼容）。
- 若 GitHub runner 访问 googletest.tar.gz 下载受限，考虑用 CPM 缓存或 vendoring。

---

## 已由本 Agent 完成（供参考）

| 任务 | 交付物 | 状态 |
|------|--------|------|
| T-RC-1-01 IR 核心数据结构 | `include/switchrecomp/ir/ir.h`, `src/ir/ir.cpp`, `tests/unit/ir/test_ir_build.cpp` | ✅ 9 测试全绿 |
| T-RS-1-01 ARM64 指令表 | `tools/arm64_table.json`, `docs/design/arm64_instructions.md` | ✅ JSON 验证通过 |
| T-BT-0-01 CMake 骨架 | `CMakeLists.txt`, `src/*/CMakeLists.txt` | ✅ 构建成功 |
| T-BT-0-02b 依赖检查脚本 | `scripts/check_deps.py` | ✅ 检查通过 |
