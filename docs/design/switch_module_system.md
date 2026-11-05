# Switch 二进制模块系统（Module System）研究文档

> 版本：v0.1
> 状态：草案 —— 将 NSO 模块加载/重定位/布局的理解，沉淀为 SwitchRecomp 的 runtime 层参考实现与规格。

## 1. 定位

本文档描述的是 **二进制模块系统**，即任天堂 Switch 游戏可执行镜像（NSO）如何被操作系统加载器 `rtld` 解析、重定位、按依赖顺序排布成进程地址空间的机制。它**不是**游戏引擎内部的高层"模块/子系统"框架（如 `KingSystem`/`ksys`、`sead`、`agl`）——那是另一层，需要另行逆向 `main` 二进制。

本层是"为什么 Switch 游戏能在没有模拟器的情况下被静态重编译成原生可执行文件"的底层地基：三个 NSO（`main` / `sdk` / `subsdk0`）各自带 MOD0 与 dynamic section，模块间通过 `DT_NEEDED` + 导出符号互相引用，`rtld` 在加载时把这些引用解析为绝对地址。

## 2. 核心事实

以下结论全部来自对真实 Switch NSO 的测量，不是推测：

1. **模块**：典型商业标题由三个模块构成：`main`、`sdk`、`subsdk0`。`rtld`（运行时加载器）被丢弃，链接改由宿主完成。
2. **加载顺序固定**：`main` → `subsdk0..9` → `sdk`。每个模块基址 2 MiB 对齐。
3. **地址上限 2^39**：IPC 缓冲区只能编码 39 位地址，所有模块必须落在 2^39 之下。
4. **guest 地址 == host 地址**：静态重编译后，guest 地址就是宿主进程地址，所以重定位就是把正确的绝对地址写进 guest（即 host）内存。
5. **更新合并的模块复用**：同一标题的小版本之间 `sdk`、`subsdk0`、`rtld` 常字节级一致，只有 `main` 不同；大版本可能整体替换（引入新 SDK、RELR 重定位、新音频 renderer）。

## 3. NSO 容器格式

NSO（`NSO0`）是 Switch 的可执行镜像容器。头部 256 字节（`0x100`）：

| 偏移 | 大小 | 字段 |
|---|---|---|
| `0x00` | 4 | Magic `"NSO0"`（LE `0x304F534E`） |
| `0x04` | 4 | 版本（0） |
| `0x08` | 4 | 保留 |
| `0x0C` | 4 | Flags：bit i = 段 i 是否 LZ4 压缩（bit 6 = 20.0.0+ ExecuteOnlyMemory，bit 7 = 22.0.0+ zstd 压缩） |
| `0x10` | 4 | TextFileOffset |
| `0x14` | 4 | TextMemoryOffset |
| `0x18` | 4 | TextSize（解压后大小） |
| `0x1C` | 4 | ModuleNameOffset（= sizeof(header)） |
| `0x20` | 4 | RoFileOffset |
| `0x24` | 4 | RoMemoryOffset |
| `0x28` | 4 | RoSize（解压后大小） |
| `0x2C` | 4 | ModuleNameSize |
| `0x30` | 4 | DataFileOffset |
| `0x34` | 4 | DataMemoryOffset |
| `0x38` | 4 | DataSize（解压后大小） |
| `0x3C` | 4 | BssSize |
| `0x40` | 32 | ModuleId（build id，最长 32 字节） |
| `0x60` | 4 | TextFileSize（压缩后大小） |
| `0x64` | 4 | RoFileSize（压缩后大小） |
| `0x68` | 4 | DataFileSize（压缩后大小） |
| `0x100` | var | 各段压缩/原始数据 |

段数据按 `*MemoryOffset` 放进内存镜像；BSS 紧跟 Data 段末尾，清零。

### 3.1 LZ4 段压缩

`flags & (1 << i)` 为 1 时，段用 **LZ4 raw block**（无 4 字节大小前缀）压缩。压缩数据长度由 `0x60 + i*4` 给出，解压后长度由段头的 `dec_size` 给出。

LZ4 raw block 格式（已验证）：`{token, [literals], [offset(2B LE)], [match 长度扩展], ...}`。token 高 4 位 = 字面量长度（15 表示需扩展字节继续累加），低 4 位 = 匹配长度 − 4（15 表示需扩展）；最后一段只有字面量、无 offset。匹配复制按字节进行，允许重叠（RLE 依赖这一点）。

## 4. MOD0 与 dynamic section

解压后的内存镜像，**偏移 4** 处是一个 `u32`，指向 MOD0 结构。MOD0：

```
+0x00  "MOD0"（4 字节 magic）
+0x04  i32 dynamic   （相对 MOD0 的偏移）
+0x08  i32 bss_start
+0x0C  i32 bss_end
+0x10  i32 eh_start
+0x14  i32 eh_end
+0x18  i32 module_obj
```

dynamic section 是 `{u64 tag, u64 val}` 的序列，`DT_NULL(0)` 结束。关键 tag：

| tag | 值 | 含义 |
|---|---|---|
| `DT_NEEDED` (1) | strtab 偏移 | 依赖的模块名 |
| `DT_STRTAB` (5) | 地址 | 字符串表 |
| `DT_SYMTAB` (6) | 地址 | 符号表 |
| `DT_RELA` (7) / `DT_RELASZ` (8) | 地址 / 大小 | RELA 重定位表（每项 24 字节） |
| `DT_JMPREL` (23) / `DT_PLTRELSZ` (2) | 地址 / 大小 | PLT 重定位表 |
| `DT_HASH` (4) | 地址 | SysV hash（`nchain` 即符号数） |
| `DT_GNU_HASH` (0x6FFFFEF5) | 地址 | GNU hash（部分模块无 `DT_HASH`） |
| `DT_RELR` (0x24) / `DT_RELRSZ` (0x23) | 地址 / 大小 | 打包的 RELATIVE 重定位 |

## 5. 符号表

`Elf64_Sym` 24 字节：`{u32 name, u8 info, u8 other, u16 shndx, u64 value, u64 size}`。

符号数量来源：有 `DT_HASH` 时取 `nchain`（`DT_HASH + 4` 处的 u32）；否则用 `DT_GNU_HASH`：

```
+0x00  nbuckets (u32)
+0x04  symoffset (u32)         # 哈希表覆盖的起始符号下标
+0x08  bloom_size (u32)
+0x0C  bloom_shift (u32)
+0x10  bloom[bloom_size] (u64)
       buckets[nbuckets] (u32)
       chains[] (u32)          # chains[i] 对应符号 symoffset+i；LSB=链尾
```

符号数 = `max(buckets)` 之后沿 chain 走到 LSB 置位的位置 + 1（`symoffset` 之前无 `DT_HASH` 时为 0 起始）。**导出符号**：`shndx != 0`；**导入符号**：`shndx == 0`。

## 6. 重定位

`DT_RELA` 与 `DT_JMPREL` 都是 `Elf64_Rela`（24 字节）：`{u64 r_offset, u64 r_info, s64 r_addend}`，其中 `type = r_info & 0xFFFFFFFF`，`sym = r_info >> 32`。

`DT_RELR` 是打包的 `R_AARCH64_RELATIVE`：每 8 字节一个条目；**偶数**条目（bit0=0）是目标地址，addend 取自该地址处原字；**奇数**条目（bit0=1）是位图，bit k+1 对应 `where + k*8`（`where` 从上一直接地址 +8 起算，每张位图覆盖 63 字）。

| 类型 | 值 | 语义 |
|---|---|---|
| `R_AARCH64_ABS64` | 257 | `S + A` |
| `R_AARCH64_GLOB_DAT` | 1025 | `S` |
| `R_AARCH64_JUMP_SLOT` | 1026 | `S` |
| `R_AARCH64_RELATIVE` | 1027 | `B + A`（RELR 时 A 为原地字） |

## 7. 多模块布局与 rtld 语义

1. 按 `main → subsdk0..9 → sdk` 顺序取模块；`rtld` 不参与、由宿主链接替代。
2. 首个基址 `0x7000000000`，每个模块 2 MiB 对齐，最终不超过 2^39。
3. 模块间引用通过导出符号表解析：构建全局 `name → (module, address)` 表；导入符号的 `R_AARCH64_*` 重定位写入解析后的绝对地址。
4. 重定位在段仍为 ReadWrite 时写入 `.text`/`.rodata`/`.data`，随后再降为最终权限（W^X）。

## 8. 与 SwitchRecomp 现状的差距

SwitchRecomp 当前 runtime 只有 `LoadElf` / `LoadNro`（单镜像、无重定位、无 NSO、无模块排布）。README 的 P4 明确列出空缺："Real three-segment NRO/NSO loading, dynamic overlays, indirect-branch runtime fallback"。

本参考实现补齐：

| 缺口 | 参考实现 |
|---|---|
| NSO0 解析 + LZ4 段解压 | `ParseNso` / `Lz4BlockDecompress` |
| MOD0 / dynamic / DT_NEEDED / 符号 / 重定位表 | `ParseNso` |
| 多模块布局（加载顺序 / 对齐 / 地址上限 / 丢弃 rtld） | `LoadModuleSystem` |
| 跨模块符号解析 + 重定位应用 | `LoadModuleSystem` |

## 9. 参考实现 API

见 `include/switchrecomp/runtime/nso.h`：

- `ParseNso(span<const u8> data, std::string_view name) → Result<NsoImage>`：纯解析，解压并产出元数据。
- `LoadNso(mm, data, load_base, name) → Result<LoadedImage>`：单模块三段加载（对齐现有 `LoadNro`）。
- `LoadModuleSystem(mm, vector<ModuleInput>) → Result<ModuleSystem>`：布局 + 重定位 + 跨模块解析。

## 10. 测试策略

全部用**非专有 fixture**（测试内构造合成 NSO，见 `tests/unit/runtime/test_nso_loader.cpp`），不引入任何游戏二进制：

- 未压缩 / LZ4 压缩段都覆盖，LZ4 用预计算向量验证（含 RLE、匹配扩展、纯字面量）。
- MOD0 + `DT_NEEDED` + `DT_HASH`/`DT_GNU_HASH` 符号计数。
- `DT_RELA`/`DT_RELR` 重定位解析与四类 `R_AARCH64_*` 的写回值。
- 双模块 fixture：一个导出、一个导入，验证跨模块符号解析与布局对齐。

## 11. 开放问题

- NSO 的 entry point 如何确定（对游戏是 rtld 经 `Entry` 符号跳转；参考实现暂以 text 段起始为 entry，待符号级确认）。
- 动态 overlay / 间接分支运行时回退（README P4 的另一半）不在本文档范围。
- 高层引擎模块系统（KingSystem/ksys/sead/agl）是独立的下一步研究。

## 12. 引擎能力研究备注

> 记录：作为后续引擎逆向研究的方向性事实，待深入验证。

- **模拟流体渲染**：底层引擎具备**极短运算量**的模拟流体渲染能力（simulated fluid rendering），能以很低的每帧计算开销产出流体效果。
- **动态分辨率**：动态分辨率（dynamic resolution）的实现依据 **英伟达（NVIDIA）与欧洲 nerd 公开资料**。
