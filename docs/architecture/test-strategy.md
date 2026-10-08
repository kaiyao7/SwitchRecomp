# SwitchRecomp 测试策略

> 版本：v0.1

## 1. 测试金字塔

```
        /\          E2E tests (端到端：字节码 → 运行结果)
       /  \
      /____\        Integration tests (模块协作：translator+codegen)
     /      \
    /________\      Unit tests (单模块：每条指令、每个函数)
```

- **单元测试**：占比最大，覆盖每个模块的内部逻辑。
- **集成测试**：覆盖模块间协作（如 translator → codegen → runtime）。
- **端到端测试**：少量，覆盖完整流水线。

## 2. 测试分类

### 2.1 单元测试（tests/unit）

- `ir/`：IR 构建、序列化、验证器。
- `translator/`：每条 ARM64 指令的解码与 IR 生成。
- `codegen/`：每条 IR 指令的 C 代码生成。
- `runtime/`：内存管理、线程、同步。
- `platform/`：OS 抽象层。
- `gpu/`：命令处理器、后端接口。

### 2.2 集成测试（tests/integration）

- `translator+codegen`：字节码 → IR → C，检查生成的 C 可编译。
- `translator+runtime`：字节码 → IR → C → 编译 → 运行，检查 guest 状态正确。
- `runtime+gpu`：guest 内存中放置命令，GPU 读取并处理。

### 2.3 回归测试（tests/regression）

- 每个修复的 bug 必须添加一个回归测试。
- 命名：`test_regression_<issue_id>.cpp`。

### 2.4 Golden 测试（tests/fixtures + tests/integration）

- 对 translator：输入字节码 → 期望 IR JSON，逐字节比对。
- 对 codegen：输入 IR → 期望 C 源码，逐行比对（忽略空白）。
- Golden 文件存 `tests/fixtures/<module>/`。
- 更新 golden：`ctest -R golden --update`（需实现更新逻辑）。

### 2.5 Fuzz 测试

- translator fuzz：随机 4 字节序列输入 `Translate`，确保不崩溃、不越界。
- 用 `libFuzzer`（clang）或自实现简单 fuzz loop。
- Phase 6 引入，CI 中跑短时间（60s）。

## 3. 测试框架

- **单元/集成**：GoogleTest（GTest + GMock）。
- **Golden**：自研轻量比对工具（读文件 + 字符串比对）。
- **Fuzz**：libFuzzer（clang）。
- **Benchmark**：Google Benchmark（Phase 5）。

## 4. 覆盖率

- 目标：核心模块（ir / translator / codegen / runtime）≥ 80%。
- 工具：`gcov` / `llvm-cov` + `lcov` / `genhtml`。
- CI 上传覆盖率报告，下降 >5% 报警（Phase 6）。

## 5. CI 测试矩阵

| 平台 | 编译器 | 配置 | 测试 |
|------|--------|------|------|
| Windows | MSVC 2022 | Debug/Release | 单测 + 集成 |
| Windows | MinGW UCRT64 GCC 16 | Release | 单测 |
| Linux | GCC 13 | Debug/Release | 单测 + 集成 + fuzz |
| Linux | Clang 17 | Release + ASan | 单测 |

- ASan 只在 Linux clang 跑，检测内存错误。
- Vulkan 测试在无 GPU 的 CI 中 skip（不 fail）。

## 6. 性能测试（Phase 5）

- `benchmarks/` 下用 Google Benchmark 写。
- 至少包含：
  - translator 吞吐（MIPS：百万指令/秒）
  - codegen 吞吐（KB IR/s）
  - 生成代码运行时（Dhrystone ARM64 版）
- 结果存 `benchmarks/results/`，CI 对比基线。

## 7. 测试编写规范

1. 每个 PR 必须包含对应测试（新功能）或回归测试（bug 修复）。
2. 测试不依赖外部网络、不依赖固定路径、可重复运行。
3. 测试命名：`TEST(<Module>, <Scenario>)`。
4. 避免在测试中写实现细节断言，聚焦行为。
5. Golden 文件必须人工 review 后提交。

## 8. 首个端到端测试（Phase 2 验收）

输入字节码（ARM64）：
```
0x91000400   // add x0, x0, #1
0xD65F03C0   // ret
```

测试流程：
1. `translator::Translate(bytes)` → IR Module
2. `codegen::GenerateC(module)` → C 源码
3. 编译 C 源码 + runtime → 可执行
4. 运行，初始 x0=0，期望最终 x0=1
5. 断言 x0 == 1

此测试通过标志着 Phase 2 完成。
