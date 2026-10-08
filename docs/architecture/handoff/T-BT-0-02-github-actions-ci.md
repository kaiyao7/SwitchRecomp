# T-BT-0-02 — GitHub Actions CI 骨架（剩余部分）

**负责 Agent**：Build/Test Agent
**优先级**：高
**阶段**：Phase 0

## 项目背景

SwitchRecomp 已有可构建的 CMake 工程：
- 本地已用 MinGW UCRT64 GCC 16 验证构建通过，14 个单元测试全绿。
- GTest 已通过 `FetchContent` 集成。
- 依赖检查脚本 `scripts/check_deps.py` 已通过（违规返回非零）。
- 各模块为独立 static library target，依赖关系由 CMake `target_link_libraries` 强制。

## 任务目标

创建 `.github/workflows/ci.yml`，在 Windows（MSVC）与 Linux（gcc）上跑
**构建 + 测试 + 依赖检查**。

## 输入

- 根 `CMakeLists.txt`、各模块 `CMakeLists.txt`
- `scripts/check_deps.py`（python3）

## 输出

`.github/workflows/ci.yml`

## 修改目录

`.github/workflows/`

## CI 规格

### 触发
- push 到 `main`
- 所有 PR

### Job 1: `deps-check`
```
python scripts/check_deps.py
```
失败即阻断合并。

### Job 2: `build-windows`
- runner: `windows-latest`
- 步骤：
  ```
  cmake -B build -G "Visual Studio 17 2022" -A x64
  cmake --build build --config Release
  ctest --test-dir build -C Release --output-on-failure
  ```

### Job 3: `build-linux`
- runner: `ubuntu-latest`
- 步骤：
  ```
  sudo apt-get update
  sudo apt-get install -y cmake ninja-build g++ python3
  cmake -B build -G Ninja
  cmake --build build
  ctest --test-dir build --output-on-failure
  ```

## 验收标准

1. 推送测试分支后 3 个 job 全部通过（构建 + 测试）。
2. `deps-check` 失败能阻断合并。

## 注意事项

- 本地已验证的是 MinGW UCRT64；CI 的 Windows job 用 **MSVC**，需注意：
  - 公共头文件用了 `using sr::common::u32;` 等，MSVC 兼容。
  - `<memory>` / `<variant>` / `<cstdint>` 等 C++17 头 MSVC 支持。
- 若 MSVC 编译有新错误，修复需在**不破坏 GCC 16 构建**的前提下进行。
- GTest 当前用 `FetchContent` 从 GitHub 下载 googletest 1.14.0 tarball。
  若 CI runner 下载失败，可改用 `apt install libgtest-dev` 或 CPM 缓存。
- **不要**改依赖矩阵，**不要**改公共接口（`include/switchrecomp/` 下的头文件）。
- 若 CI 跑通后发现问题，把修复限定在构建配置或平台兼容宏层面。
