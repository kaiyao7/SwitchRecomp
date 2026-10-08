# ARM64 指令编码表（Phase 1 核心 20 条）

> 版本：v0.1
> 来源：ARM Architecture Reference Manual, ARMv8-A（AArch64）
> 机器可读版本：[`tools/arm64_table.json`](../../tools/arm64_table.json)

## 说明

- AArch64 指令固定 32 位，小端存储。
- 每条指令的 `mask` / `value` 用于识别：`(insn & mask) == value`。
- `sf` 位：1 = 64 位寄存器（X），0 = 32 位寄存器（W）。
- 寄存器 `ZR`（31）在数据处理中表示零寄存器。

## 指令清单

| # | 助记符 | 类别 | mask | value |
|---|--------|------|------|-------|
| 1 | ADD (imm) | data_proc_imm | `0x7F800000` | `0x11000000` |
| 2 | SUB (imm) | data_proc_imm | `0x7F800000` | `0x51000000` |
| 3 | ADD (shifted reg) | data_proc_reg | `0x7F200000` | `0x0B000000` |
| 4 | SUB (shifted reg) | data_proc_reg | `0x7F200000` | `0x4B000000` |
| 5 | AND (shifted reg) | logical_reg | `0x7F200000` | `0x0A000000` |
| 6 | ORR (shifted reg) | logical_reg | `0x7F200000` | `0x2A000000` |
| 7 | EOR (shifted reg) | logical_reg | `0x7F200000` | `0x4A000000` |
| 8 | MOVZ (MOV imm alias) | wide_imm | `0x7F800000` | `0x52800000` |
| 9 | MOVN (MVN alias) | wide_imm | `0x7F800000` | `0x12800000` |
| 10 | LSL (imm, UBFM alias) | bitfield | `0x7F800000` | `0x53000000` |
| 11 | LSR (imm, UBFM alias) | bitfield | `0x7F800000` | `0x53000000` |
| 12 | ASR (imm, SBFM alias) | bitfield | `0x7F800000` | `0x13000000` |
| 13 | LDR (unsigned offset) | load | `0xFFC00000` | `0xB9400000` |
| 14 | STR (unsigned offset) | store | `0xFFC00000` | `0xB9000000` |
| 15 | B | branch | `0xFC000000` | `0x14000000` |
| 16 | BL | branch_with_link | `0xFC000000` | `0x94000000` |
| 17 | B.cond | branch_cond | `0xFF000010` | `0x54000000` |
| 18 | CMP (imm, SUBS alias) | data_proc_imm | `0x7F800000` | `0x71000000` |
| 19 | CBZ | compare_branch | `0x7E000000` | `0x34000000` |
| 20 | CBNZ | compare_branch | `0x7E000000` | `0x35000000` |
| 21 | RET | branch_reg | `0xFFFFFC1F` | `0xD65F0000` |
| 22 | NOP | hint | `0xFFFFFFFF` | `0xD503201F` |

## 解码策略建议

1. **分组快速分派**：先按高 8 位（bits 31–24）分组到主类别（数据处理立即数/寄存器、加载存储、分支、系统）。
2. **组内细分**：用 `opc`、`op` 等位区分同组指令。
3. **别名处理**：汇编器别名（MOV→MOVZ, MVN→MOVN, CMP→SUBS, LSL/LSR/ASR→BFM）在硬件编码层是同一指令，解码时按真实编码识别即可。
4. **标志位**：`S` 位决定是否更新 NZCV；CMP 是 `S=1, Rd=ZR` 的 SUBS，必须设置标志。

## 条件码（cond 字段，4 位）

| cond | 含义 | NZCV 条件 |
|------|------|-----------|
| 0 | EQ | Z==1 |
| 1 | NE | Z==0 |
| 2 | CS/HS | C==1 |
| 3 | CC/LO | C==0 |
| 4 | MI | N==1 |
| 5 | PL | N==0 |
| 6 | VS | V==1 |
| 7 | VC | V==0 |
| 8 | HI | C==1 && Z==0 |
| 9 | LS | C==0 \|\| Z==1 |
| 10 | GE | N==V |
| 11 | LT | N!=V |
| 12 | GT | Z==0 && N==V |
| 13 | LE | Z==1 \|\| N!=V |
| 14 | AL | always |
| 15 | NV | 保留（同 AL） |

## 待办（Phase 1 实现时补充）

- [ ] 为每条指令补充具体字段提取的位运算表达式
- [ ] 汇编器别名到真实编码的映射表
- [ ] 每条指令的示例字节码（用于 golden test）
