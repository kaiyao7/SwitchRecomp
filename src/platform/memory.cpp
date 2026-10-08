// platform memory implementation
#include "switchrecomp/platform/memory.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#endif

namespace sr::platform {

void* AllocPages(usize size, MemProtect prot) {
#ifdef _WIN32
    DWORD protect = PAGE_READWRITE;
    switch (prot) {
        case MemProtect::Read: protect = PAGE_READONLY; break;
        case MemProtect::ReadWrite: protect = PAGE_READWRITE; break;
        case MemProtect::ReadExec: protect = PAGE_EXECUTE_READ; break;
        case MemProtect::ReadWriteExec: protect = PAGE_EXECUTE_READWRITE; break;
        default: protect = PAGE_READWRITE; break;
    }
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, protect);
#else
    int flags = PROT_NONE;
    switch (prot) {
        case MemProtect::None: flags = PROT_NONE; break;
        case MemProtect::Read: flags = PROT_READ; break;
        case MemProtect::ReadWrite: flags = PROT_READ | PROT_WRITE; break;
        case MemProtect::ReadExec: flags = PROT_READ | PROT_EXEC; break;
        case MemProtect::ReadWriteExec: flags = PROT_READ | PROT_WRITE | PROT_EXEC; break;
        default: flags = PROT_READ | PROT_WRITE; break;
    }
    void* p = mmap(nullptr, size, flags, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    return p;
#endif
}

void FreePages(void* ptr, usize size) {
    if (!ptr) return;
#ifdef _WIN32
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}

bool ProtectPages(void* ptr, usize size, MemProtect prot) {
    if (!ptr) return false;
#ifdef _WIN32
    DWORD protect = PAGE_READWRITE;
    switch (prot) {
        case MemProtect::None: protect = PAGE_NOACCESS; break;
        case MemProtect::Read: protect = PAGE_READONLY; break;
        case MemProtect::ReadWrite: protect = PAGE_READWRITE; break;
        case MemProtect::ReadExec: protect = PAGE_EXECUTE_READ; break;
        case MemProtect::ReadWriteExec: protect = PAGE_EXECUTE_READWRITE; break;
        default: protect = PAGE_READWRITE; break;
    }
    DWORD old_protect;
    return VirtualProtect(ptr, size, protect, &old_protect) != 0;
#else
    int flags = PROT_NONE;
    switch (prot) {
        case MemProtect::None: flags = PROT_NONE; break;
        case MemProtect::Read: flags = PROT_READ; break;
        case MemProtect::ReadWrite: flags = PROT_READ | PROT_WRITE; break;
        case MemProtect::ReadExec: flags = PROT_READ | PROT_EXEC; break;
        case MemProtect::ReadWriteExec: flags = PROT_READ | PROT_WRITE | PROT_EXEC; break;
        default: flags = PROT_READ | PROT_WRITE; break;
    }
    return mprotect(ptr, size, flags) == 0;
#endif
}

usize PageSize() {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#else
    long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? static_cast<usize>(ps) : 4096u;
#endif
}

} // namespace sr::platform
