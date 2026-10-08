# SwitchRecomp 总体架构

> 版本：v0.1（Phase 0 初稿）
> 状态：草案，待各 Agent 评审

## 1. 项目目标

SwitchRecomp 是一个面向**合法拥有的软件、公开研究材料和自有测试程序**的
Switch ARM64 原生重编译 / 兼容性研究框架。

核心研究目标：
- ARM64 指令 → 中间表示（IR）→ C/C++ 源代码 → 原生可执行文件的重编译流水线
- 运行时兼容层（内存、线程、同步、IPC、文件系统）
- GPU 命令与 shader 抽象层
- 性能分析与优化

非目标（Non-goals）：
- 不提供游戏 ROM 分发或破解工具
- 不追求短期内完整商业兼容性
- 不直接 fork 现有模拟器整体代码（可借鉴公开研究）

## 2. 当前阶段

**Phase 0：工程初始化。** 工作目录为空，无 git 仓库、无源码、无构建产物。
本阶段输出为目录骨架、接口定义、架构决策记录与任务拆分。

## 3. 设计原则

1. **分层解耦**：IR 是核心，translator/codegen/runtime/gpu 只依赖 IR 与 platform，不互相依赖。
2. **最小公共接口**：模块间只通过 `include/switchrecomp/` 下的公共头文件交互；实现细节放 `src/`。
3. **可测试优先**：每个模块都要有单元测试，复杂翻译路径要有 golden tests。
4. **渐进式实现**：先跑通"一条 ARM64 指令 → IR → C → 运行"的最小闭环，再扩展。
5. **研究友好**：保留 IR dump、C 源码输出、trace 等调试接口，不做黑盒。
6. **跨平台**：宿主支持 Windows (MinGW/MSVC)、Linux；C++17，避免平台专有扩展。

## 4. 模块总览

```
                        ┌─────────────┐
                        │   tools/    │  CLI, disasm, IR viewer
                        └──────┬──────┘
                               │
   ┌──────────┐   ┌─────┐   ┌──▼───┐   ┌─────────┐   ┌──────────┐
   │ platform │──▶│ ir  │◀──│ trans│──▶│ codegen │──▶│  native  │
   └────┬─────┘   └─────┘   └──────┘   └─────────┘   └────┬─────┘
        │                                                  │
        │           ┌─────────┐   ┌─────┐                 │
        └──────────▶│ runtime │◀──│ gpu │◀────────────────┘
                    └─────────┘   └─────┘
```

| 模块 | 职责 | 允许依赖 | 禁止依赖 |
|------|------|---------|---------|
| `ir` | 中间表示数据结构、序列化、验证 | `common`, `platform`(仅基础类型) | 其他全部 |
| `translator` | ARM64 解码 → IR | `ir`, `common` | `runtime`, `gpu`, `codegen` |
| `codegen` | IR → C/C++ 源码 | `ir`, `common` | `translator`, `runtime`, `gpu` |
| `runtime` | 内存/线程/同步/IPC/FS 兼容层 | `ir`(仅类型), `platform` | `translator`, `codegen` |
| `gpu` | GPU 命令处理、shader 翻译、后端抽象 | `runtime`(内存), `platform` | `translator`, `codegen` |
| `platform` | OS 抽象（内存、线程、文件、时间） | 无（纯系统库） | 其他全部 |
| `common` | 通用工具（log、error、span、bit） | 无 | 其他全部 |

详细依赖规则见 [modules.md](./modules.md)。

## 5. 重编译流水线

```
ARM64 binary
   │
   ▼  translator
IR Module (in-memory / serialized)
   │
   ▼  codegen
C/C++ source files (.c / .cpp)
   │
   ▼  host compiler (gcc/clang/msvc)
native object / shared library
   │
   ▼  runtime loader
running process (linked with runtime + gpu)
```

详细流水线与 IR 设计见 [pipeline.md](./pipeline.md)。

## 6. 关键技术决策

详见 [ADR-0001](../adr/ADR-0001-initial-architecture.md)：
- 语言：C++17（宿主），生成 C11 代码
- IR：自研 SSA-like IR，序列化用 JSON/二进制双格式
- 后端：先做 C 后端（可移植、可调试），JIT 后端留作 Phase 5
- GPU：先做 Vulkan 后端抽象，接口与后端解耦

## 7. 路线图

见 [roadmap.md](./roadmap.md)，分为 Phase 0–6。

## 8. 文档索引

- [modules.md](./modules.md) — 模块职责与依赖
- [pipeline.md](./pipeline.md) — 重编译流水线与 IR 设计
- [interfaces.md](./interfaces.md) — 模块最小公共接口
- [tasks.md](./tasks.md) — 五个 Agent 第一批任务
- [test-strategy.md](./test-strategy.md) — 测试策略
- [roadmap.md](./roadmap.md) — 项目路线图
- [ADR-0001](../adr/ADR-0001-initial-architecture.md) — 架构决策记录
