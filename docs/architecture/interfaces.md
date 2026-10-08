# 模块最小公共接口

> 版本：v0.1
> 所有接口均为最小骨架，仅声明类型与方法签名，不包含实现。
> 实现由对应 Agent 在各自模块目录中完成。

## 1. common

**文件**：`include/switchrecomp/common/types.h`

```cpp
#pragma once
#include <cstdint>
#include <cstddef>

namespace sr::common {
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8  = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;

// 简单 span（C++17 无 std::span）
template <typename T>
class span {
public:
    span(T* data, usize size) : data_(data), size_(size) {}
    T* data() const { return data_; }
    usize size() const { return size_; }
    T& operator[](usize i) { return data_[i]; }
    const T& operator[](usize i) const { return data_[i]; }
private:
    T* data_;
    usize size_;
};
} // namespace sr::common
```

**文件**：`include/switchrecomp/common/result.h`

```cpp
#pragma once
#include <variant>
#include <string>

namespace sr::common {
struct Error {
    std::string message;
};

template <typename T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error err) : data_(std::move(err)) {}
    bool ok() const { return std::holds_alternative<T>(data_); }
    T& value() { return std::get<T>(data_); }
    const T& value() const { return std::get<T>(data_); }
    const Error& error() const { return std::get<Error>(data_); }
private:
    std::variant<T, Error> data_;
};
} // namespace sr::common
```

## 2. platform

**文件**：`include/switchrecomp/platform/memory.h`

```cpp
#pragma once
#include "switchrecomp/common/types.h"

namespace sr::platform {

enum class MemProtect : u32 {
    None = 0, Read = 1, Write = 2, Exec = 4,
    ReadWrite = Read | Write,
    ReadExec = Read | Exec,
    ReadWriteExec = Read | Write | Exec,
};

// 分配页对齐内存
void* AllocPages(usize size, MemProtect prot);
void  FreePages(void* ptr, usize size);
bool  ProtectPages(void* ptr, usize size, MemProtect prot);
usize PageSize();

} // namespace sr::platform
```

**文件**：`include/switchrecomp/platform/thread.h`

```cpp
#pragma once
#include "switchrecomp/common/types.h"
#include <functional>

namespace sr::platform {

class Thread {
public:
    using Entry = std::function<void()>;
    explicit Thread(Entry entry);
    ~Thread();
    void Join();
private:
    struct Impl;
    Impl* impl_;
};

class Mutex {
public:
    Mutex();
    ~Mutex();
    void Lock();
    void Unlock();
private:
    struct Impl;
    Impl* impl_;
};

} // namespace sr::platform
```

## 3. ir

**文件**：`include/switchrecomp/ir/ir.h`

```cpp
#pragma once
#include "switchrecomp/common/types.h"
#include <string>
#include <vector>
#include <memory>

namespace sr::ir {

enum class TypeId : u8 {
    Void, U8, U16, U32, U64, S8, S16, S32, S64, F32, F64, Ptr
};

struct Type {
    TypeId id;
    static Type U32() { return {TypeId::U32}; }
    static Type U64() { return {TypeId::U64}; }
    // ...
};

enum class Opcode : u16 {
    Nop, Add, Sub, And, Or, Xor, Shl, Shr, Sar, Mul,
    Load, Store,
    Branch, CondBranch, Return, Call, IndirectBranch,
    Cast, Svc, Undefined,
};

class Value {
public:
    virtual ~Value() = default;
    Type type() const { return type_; }
protected:
    Type type_;
};

class Instruction : public Value {
public:
    Opcode opcode() const { return opcode_; }
    const std::vector<Value*>& operands() const { return operands_; }
protected:
    Opcode opcode_;
    std::vector<Value*> operands_;
};

class BasicBlock {
public:
    const std::vector<Instruction*>& instructions() const { return instrs_; }
private:
    std::vector<Instruction*> instrs_;
};

class Function {
public:
    u64 guest_address() const { return guest_addr_; }
    const std::vector<BasicBlock*>& blocks() const { return blocks_; }
private:
    u64 guest_addr_;
    std::vector<BasicBlock*> blocks_;
};

class Module {
public:
    std::vector<Function*>& functions() { return funcs_; }
    std::string to_json() const;
    static std::unique_ptr<Module> from_json(const std::string& json);
    bool verify() const;
private:
    std::vector<Function*> funcs_;
};

} // namespace sr::ir
```

## 4. translator

**文件**：`include/switchrecomp/translator/translator.h`

```cpp
#pragma once
#include "switchrecomp/common/types.h"
#include "switchrecomp/common/span.h"
#include "switchrecomp/common/result.h"
#include "switchrecomp/ir/ir.h"
#include <memory>

namespace sr::translator {

struct TranslateOptions {
    u64 base_address = 0;
    bool strict_decode = false;  // 遇到未实现指令时报错 vs 占位
};

// 翻译一段 ARM64 字节码为 IR Module
common::Result<std::unique_ptr<ir::Module>> Translate(
    common::span<const u8> code,
    const TranslateOptions& options);

} // namespace sr::translator
```

## 5. codegen

**文件**：`include/switchrecomp/codegen/codegen.h`

```cpp
#pragma once
#include "switchrecomp/common/result.h"
#include "switchrecomp/ir/ir.h"
#include <string>

namespace sr::codegen {

struct CodegenOptions {
    std::string module_name = "guest";
    bool emit_comments = true;
};

// 将 IR Module 生成为 C 源码
common::Result<std::string> GenerateC(const ir::Module& module,
                                      const CodegenOptions& options);

} // namespace sr::codegen
```

## 6. runtime

**文件**：`include/switchrecomp/runtime/runtime.h`

```cpp
#pragma once
#include "switchrecomp/common/types.h"

namespace sr::runtime {

struct GuestContext {
    u64 x[31];
    u64 sp;
    u64 pc;
    u32 nzcv;
    // fpsimd... (Phase 3)
};

// guest 虚拟地址 → 宿主指针（softmmu 查表 / fastmem 直映）
void* GuestToHost(GuestContext* ctx, u64 guest_addr, usize size, bool write);

// 执行一次 guest 调用（用于间接跳转 / bl）
void  CallGuest(GuestContext* ctx, u64 target_addr);

// SVC 分派
void  HandleSvc(GuestContext* ctx, u32 svc_id);

} // namespace sr::runtime
```

## 7. gpu

**文件**：`include/switchrecomp/gpu/gpu.h`

```cpp
#pragma once
#include "switchrecomp/common/types.h"

namespace sr::gpu {

class GPUBackend {
public:
    virtual ~GPUBackend() = default;
    virtual void Initialize() = 0;
    virtual void Shutdown() = 0;
    // 提交一个 GPU 命令列表（占位）
    virtual void SubmitCommandBuffer(common::u64 addr, common::usize size) = 0;
};

// 创建 Vulkan 后端（Phase 4 实现）
GPUBackend* CreateVulkanBackend();

} // namespace sr::gpu
```

## 8. 接口稳定性约定

- 公共头文件位于 `include/switchrecomp/<module>/`，是模块间唯一契约。
- 修改公共接口必须更新 ADR 并通知所有依赖方。
- 实现细节放 `src/<module>/`，不得被其他模块 `#include`。
- CI 检查：`include/` 下的头文件只能 `#include` 允许依赖模块的 `include/` 头文件。
