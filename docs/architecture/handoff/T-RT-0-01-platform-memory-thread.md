# T-RT-0-01 — platform 层最小实现（内存 + 线程）

**负责 Agent**：Runtime Agent
**优先级**：高
**阶段**：Phase 0

## 项目背景

SwitchRecomp 是一个 Switch ARM64 重编译研究框架（C++17）。模块分层：
`common → platform → ir → {translator, codegen, runtime, gpu}`。
你负责 `platform` 和 `runtime` 模块。

## 任务目标

将 `src/platform/memory.cpp` 和 `src/platform/thread.cpp` 从当前 stub 替换为**真实实现**，
支持 Windows（优先）与 POSIX。

## 输入（现有文件，勿改头文件）

- `include/switchrecomp/platform/memory.h` — `AllocPages / FreePages / ProtectPages / PageSize`，`MemProtect` 枚举
- `include/switchrecomp/platform/thread.h` — `Thread(Entry)`、`Mutex`、`MutexGuard`，pImpl 模式
- `src/platform/memory.cpp`、`src/platform/thread.cpp`（当前是返回 nullptr/空实现的 stub）

## 输出

- `src/platform/memory.cpp`：
  - Windows：`VirtualAlloc / VirtualFree / VirtualProtect`
  - POSIX：`mmap(MAP_ANONYMOUS|MAP_PRIVATE) / munmap / mprotect`
- `src/platform/thread.cpp`：
  - 优先用 `std::thread` / `std::mutex`（最简跨平台），或原生 API。

## 修改目录

`src/platform/`

## 依赖

仅 `sr_common`（types）

## 验收标准

1. `AllocPages(size, ReadWrite)` 返回非空、页对齐、零初始化的内存，可正常读写。
2. `FreePages` 成功释放。
3. `ProtectPages(ptr, size, Read)` 后写入触发访问违规（不要求测试捕获异常，只要求行为正确）。
4. `PageSize()` 返回真实页大小。
5. `Thread`：启动函数执行后 `Join()` 返回。
6. `Mutex` + `MutexGuard`：10 线程 × 10000 次自增结果 == 100000。

## 测试方法

创建以下测试文件并加入 `tests/CMakeLists.txt` 的 `sr_tests` 源列表（链接 `sr_platform`）：

- `tests/unit/platform/test_memory.cpp`
- `tests/unit/platform/test_thread.cpp`

本地构建（MSYS2 UCRT64）：

```
cmake -B build -G Ninja ^
  -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe ^
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe
cmake --build build
ctest --test-dir build --output-on-failure
```

## 注意事项

- **禁止**修改 `include/switchrecomp/platform/*.h` 公共接口。
- **禁止**依赖 `ir / runtime / gpu` 任何模块（依赖矩阵：platform 只能依赖 common）。
- pImpl 的 `Impl` 结构体定义放在 `.cpp` 内。
- 实现完成后运行 `python scripts/check_deps.py` 确认无违规依赖。
