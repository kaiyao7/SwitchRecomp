# T-RS-4-01 — Maxwell GPU 命令与 shader 资料整理

**负责 Agent**：Research Agent
**优先级**：中
**阶段**：Phase 0（为 T-GPU-4-02 提供输入）

## 项目背景

SwitchRecomp 的 GPU 模块需要处理 Nvidia Maxwell FIFO 命令并最终翻译 shader。
本任务整理入门资料，供 Graphics Agent 实现命令处理器骨架使用。

## 任务目标

产出两份 Markdown 文档。

## 输出

1. `docs/design/maxwell_commands.md`
2. `docs/design/maxwell_shader_isa.md`

## 修改目录

`docs/design/`

## 内容要求

### maxwell_commands.md

- FIFO 命令结构：每个 32-bit word 的位域
  - method[11:0]、subchannel[15:13]、opcode（长包/非长包）、count[29:16]
  - 非长包（opcode=0）：立即数 [15:0]
  - 长包（opcode=1）：count 个 word 跟随
- 常见 method 号（3D 类），列出 ≥ 10 个：
  - NoOperation(0x0), SetObject(0x0), Bind(0x15), Upload[0x40-0xFF],
    DrawArrays(0x84C), DrawIndex(0x84F), 等
- subchannel 到 class 类型的映射：3D=0x902D, COPY=0xA140, 等

### maxwell_shader_isa.md

- 指令编码基础：每条指令 8 字节（或 16 字节双发射），位域概述
- 至少 10 条常见指令的编码与伪代码：
  - MOV, ADD, MUL, MAD, F2F, F2I, I2F, LDG, STG, BRA, EXIT, RET
- 每条含：助记符、编码位域、操作数类型、伪代码摘要

## 验收标准

1. FIFO 命令位域描述准确，能指导解析一个 word。
2. shader ISA 每条指令有清晰位域表。

## 测试方法

文档评审。Graphics Agent 后续用此资料解析简单命令缓冲区。

## 参考来源

- Nvidia Maxwell 公开文档
- switchbrew GPU 页面
- envytools / envyas 开源反汇编器（仅参考公开 ISA 编码）
