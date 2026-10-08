# T-RS-3-01 — Switch SVC 与内存布局资料整理

**负责 Agent**：Research Agent
**优先级**：高
**阶段**：Phase 0（为 T-RT-3-02 提供输入）

## 项目背景

SwitchRecomp 需要为 Runtime 层整理 Switch 用户态 SVC 表与进程地址空间布局，
供 Runtime Agent 实现 SVC handler 使用。只整理公开资料（switchbrew 等），
不涉及密钥/加密/破解内容。

## 任务目标

产出两份 Markdown 文档。

## 输出

1. `docs/design/switch_svc.md`
2. `docs/design/switch_memory_layout.md`

## 修改目录

`docs/design/`

## 内容要求

### switch_svc.md

- 表格列：SVC 号（十六进制）| 名称 | 参数（X0–X7，注明 in/out）| 返回值 | 行为摘要
- 至少覆盖 0x00–0x3F 范围的主要 SVC（≥ 30 条），包括但不限于：
  - SetHeapSize, SetMemoryPermission, MapMemory, UnmapMemory, QueryMemory
  - CreateThread, StartThread, ExitThread, SleepThread, GetThreadPriority, SetThreadPriority
  - Mutex, WaitSynchronization, SignalEvent, ClearEvent, CreateEvent, CloseHandle, ResetSignal
  - SendSyncRequest, WaitProcessWideKeyAtomic
  - GetProcessId, GetThreadId, Break, OutputDebugString, GetInfo
  - FlushEntireDataCache, FlushDataCache, GetThreadIdForDebug, GetDebugEvent, ContinueDebugEvent
  - CreateSession, CloseSession
- 注明 ABI：X0–X7 传参，X0 返回值，SVC 号由指令立即数指定。

### switch_memory_layout.md

- 39-bit 用户态地址空间图（0x0000_0000_0000 – 0x0000_007F_FFFF_FFFF）
- 各区域（≥ 6 个）：
  - 主程序（text / rodata / data / bss）
  - 堆（heap）
  - 栈（stack）
  - 镜像区（alias / stack / heap / legacy mirror）
  - TLS
  - IPC 缓冲区（0x7000_0000 附近）
- 每段注明：地址范围、权限（R / RW / RX）、用途

## 验收标准

1. SVC 表 ≥ 30 条，每条参数与行为清晰。
2. 内存布局各区域地址范围与 switchbrew 公开资料一致。

## 测试方法

文档评审即可。Runtime Agent 后续用此表实现 SVC 分派，
能正确识别 5 个核心 SVC 即视为资料合格。

## 参考来源

- switchbrew wiki（公开）
- Atmosphère 开源项目（仅参考公开 SVC 号与签名，不复制专有代码）
