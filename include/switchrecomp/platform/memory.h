#pragma once

#include "switchrecomp/common/types.h"

namespace sr::platform {

using sr::common::u32;
using sr::common::usize;

enum class MemProtect : u32 {
    None = 0,
    Read = 1,
    Write = 2,
    Exec = 4,
    ReadWrite = Read | Write,
    ReadExec = Read | Exec,
    ReadWriteExec = Read | Write | Exec,
};

// Allocate page-aligned, zero-initialized memory.
void* AllocPages(usize size, MemProtect prot);

// Free memory allocated by AllocPages.
void FreePages(void* ptr, usize size);

// Change page protection.
bool ProtectPages(void* ptr, usize size, MemProtect prot);

// System page size in bytes.
usize PageSize();

} // namespace sr::platform
