# ADR-0001：初始架构决策

- **状态**：Accepted（草案，待评审）
- **日期**：2026-10-08
- **决策者**：SwitchRecomp 架构组

## 背景

SwitchRecomp 是一个全新的 ARM64 原生重编译研究框架。工作目录为空，需要从
零建立技术路线、模块划分与工程规范。本 ADR 记录 Phase 0 阶段的关键架构决策。

## 决策

### 1. 编程语言

**决策**：宿主代码用 C++17，生成代码用 C11。

**理由**：
- C++17 跨平台支持好，有 `std::variant`、`std::optional`、结构化绑定等。
- 生成 C11 可被 gcc/clang/msvc 全部编译，便于 AOT 调试。
- 不选 Rust：团队 C++ 经验更丰富，且与现有 GPU 生态（Vulkan C++ bindings）集成更顺。
- 不选 C++20/23：MinGW UCRT64 的 GCC 16 虽支持，但降低工具链要求更稳妥。

### 2. 重编译策略：AOT（生成 C）优先于 JIT

**决策**：Phase 1–4 只做 AOT（ARM64 → IR → C → 宿主编译），JIT 留到 Phase 5 评估。

**理由**：
- 生成的 C 可读、可调试，研究友好。
- 可直接利用宿主编译器的优化（-O2/-O3）。
- 不依赖宿主 ISA 知识，一套 IR backend 走天下。
- 缺点：编译慢、无法动态重编译 → Phase 5 用 JIT 补性能。

### 3. IR 设计：自研 SSA-like IR

**决策**：不自用 LLVM IR，自研轻量 IR。

**理由**：
- LLVM 依赖重、编译慢、学习曲线陡，不适合研究迭代。
- 自研 IR 可精确控制 ARM64 语义（标志位、向量）。
- IR 序列化用 JSON + 二进制双格式，便于 golden test。
- 预留：Phase 5 可评估 IR → LLVM IR 作为额外 backend。

### 4. 模块分层与依赖规则

**决策**：ir 为中心，translator/codegen 只依赖 ir，runtime/gpu 只依赖 platform+ir。

**理由**：
- 保证每个模块可独立测试（translator 不需要 runtime 就能测）。
- 避免循环依赖。
- 详细矩阵见 [modules.md](../architecture/modules.md)。

### 5. GPU 后端

**决策**：先做 Vulkan 后端接口抽象，不急于实现 Maxwell 翻译。

**理由**：
- Vulkan 跨平台，是现代 GPU 事实标准。
- Maxwell → SPIR-V 翻译极复杂，Phase 4 只做接口 + 简单命令转发。
- 接口与实现分离，后续可加 GL/D3D12。

### 6. 构建系统

**决策**：CMake + Ninja，支持 MSVC 与 MinGW UCRT64。

**理由**：
- C++ 生态事实标准。
- 前期经验（suyu/yuzu 编译）证明 MinGW UCRT64 + CMake + Ninja 可用。

### 7. 测试框架

**决策**：GoogleTest 做单元测试，自研 golden test 框架做翻译正确性。

**理由**：
- GTest 成熟、跨平台。
- Golden test：对每条 ARM64 指令，记录"输入字节码 → 期望 IR JSON"，回归时比对。

## 后果

### 正面
- 模块解耦，可并行开发。
- AOT 路径可调试性强。
- 依赖少，启动快。

### 负面
- 自研 IR 意味着工具链（优化器、调试器）也要自研。
- AOT 编译慢，Phase 5 前性能不是重点。
- C 后端生成的代码运行时开销依赖 runtime 辅助函数。

## 待决事项（Phase 1–3 中评审）

1. 标志位建模：惰性传播 vs 显式 Flag 值？（Phase 1 末评审）
2. 间接跳转处理：翻译期跳转表 vs runtime 查表？（Phase 3 评审）
3. fastmem 实现：Windows 下用 guard page 还是纯软件查表？（Phase 3 评审）
4. IR → LLVM IR 是否值得做？（Phase 5 评审）

## 参考

- ARM Architecture Reference Manual (ARMv8-A)
- QEMU TCG 设计（参考其 IR 与翻译策略）
- LLVM IR 文档（参考其 SSA 形式）
