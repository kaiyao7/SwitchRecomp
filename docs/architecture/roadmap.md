# SwitchRecomp 项目路线图

> 版本：v0.1
> 状态：草案

## Phase 0：工程初始化（当前）

**目标**：建立可构建、可测试的空骨架。

**交付物**：
- [x] 目录结构
- [x] 架构文档、模块依赖、接口定义
- [x] ADR-0001
- [x] 路线图
- [ ] 可编译的 CMake 工程（各模块空 target）
- [ ] CI 骨架（GitHub Actions：Windows + Linux 构建）
- [ ] GTest 集成，跑通一个空测试

**验收**：`cmake -B build && cmake --build build && ctest` 全绿。

---

## Phase 1：IR 与基础 ARM64 指令

**目标**：跑通"一条 ARM64 指令 → IR"的闭环。

**交付物**：
- IR 核心数据结构（Type/Value/Instruction/BasicBlock/Function/Module）
- IR JSON 序列化 + 验证器
- ARM64 解码器骨架
- 实现 20 条核心指令：`add/sub/and/or/eor/mov/mvn/lsl/lsr/asr/ldr/str/b/bl/b.cond/cmp/cbz/cbnz/ret/nop`
- Golden test 框架 + 20 条指令的 golden 数据

**验收**：
- 对每条指令，`translator::Translate` 产出的 IR JSON 与 golden 文件逐字节匹配。
- `ir::Module::verify()` 通过。

**风险**：标志位建模选择；解码器查表性能。

---

## Phase 2：基础代码生成

**目标**：跑通"IR → C → 编译 → 执行"的端到端闭环。

**交付物**：
- codegen C 后端
- runtime 最小骨架（GuestContext、GuestToHost 占位）
- `tools/recomp_cli`：字节码 → IR → C → 编译 → 运行
- 端到端测试：`add x0, x0, #1` 执行后 x0 正确

**验收**：
- `recomp_cli --input test.bin --run` 输出 `x0 = 1`（假设初始 x0=0）。
- 生成的 C 文件用 `gcc -O2 -c` 编译无警告。

**风险**：控制流还原质量；运行时调用约定。

---

## Phase 3：Runtime

**目标**：建立可用的运行时兼容层。

**交付物**：
- GuestContext 完整化（含 fpsimd）
- 内存管理：页表 + softmmu 查表
- 线程：guest thread → host thread
- 同步原语：mutex/condvar/semaphore（Switch SVC 语义）
- SVC handler 骨架
- 文件系统 VFS 抽象

**验收**：
- 能在 guest 代码中执行 `svc` 并被 runtime 正确拦截。
- 多线程 guest 程序能正确同步（用自有测试程序验证）。

**风险**：Windows 下 fastmem 可行性；SVC 语义完整性。

---

## Phase 4：Graphics Abstraction

**目标**：建立 GPU 命令处理与后端抽象。

**交付物**：
- GPU 命令处理器骨架（FIFO、method 分发）
- `GPUBackend` 接口 + Vulkan 后端初始化
- Shader 翻译占位接口（不实现完整 Maxwell→SPIR-V）
- 纹理/顶点内存读取（通过 runtime）

**验收**：
- 能初始化 Vulkan 后端并提交空命令缓冲区。
- 能从 guest 内存读取 4KB 纹理数据。

**风险**：Maxwell 文档稀缺；Vulkan 跨驱动兼容性。

---

## Phase 5：性能优化

**目标**：在保持正确的前提下提升性能。

**交付物**：
- IR 优化器（常量折叠、DCE、标志位合并）
- fastmem（guard page 或直接映射）
- 代码生成优化（寄存器分配提示、热点函数内联）
- benchmark 框架 + 数据
- 评估 JIT 后端必要性

**验收**：
- 自有 benchmark（如 Dhrystone ARM64 版本）相对 Phase 3 提升 ≥ 50%。
- 出 benchmark 报告（CPU/内存/缓存开销）。

**风险**：优化破坏正确性；fastmem 在 Windows 的稳定性。

---

## Phase 6：完整测试体系

**目标**：建立持续、可回归的测试与质量保障。

**交付物**：
- 单元测试覆盖率 ≥ 80%（核心模块）
- 集成测试：端到端指令集覆盖（≥ 100 条 ARM64 指令）
- 回归测试：已知 bug 的回归用例
- Fuzz 测试：translator 随机字节码 fuzz
- CI：每次 PR 跑构建 + 单测 + golden test + fuzz（短时间）
- 性能回归：benchmark 进 CI，下降 >10% 报警

**验收**：
- CI 全绿，覆盖率达标。
- 每个修复的 bug 都有对应回归测试。

---

## 里程碑总览

| Phase | 主题 | 关键产出 | 预计周期 |
|-------|------|---------|---------|
| 0 | 工程初始化 | 可构建空骨架 | 1 周 |
| 1 | IR + 基础指令 | 20 条指令翻译 + golden test | 2–3 周 |
| 2 | 代码生成 | 端到端 add 指令跑通 | 2 周 |
| 3 | Runtime | 内存/线程/同步/SVC | 3–4 周 |
| 4 | Graphics | GPU 命令 + Vulkan 后端 | 3–4 周 |
| 5 | 性能优化 | IR 优化 + fastmem + benchmark | 3 周 |
| 6 | 测试体系 | 覆盖率 + fuzz + CI | 2 周 |

**总预计**：约 16–19 周（可并行，实际取决于人力）。
