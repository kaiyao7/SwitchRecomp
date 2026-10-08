// runtime: guest memory management, call dispatch, SVC dispatch.
#include "switchrecomp/runtime/runtime.h"
#include "switchrecomp/runtime/memory_manager.h"

#include <cstdio>
#include <map>

namespace sr::runtime {

namespace {

// Global guest memory manager, initialized on first use.
MemoryManager& GetMemoryManager() {
    static MemoryManager manager;
    static bool initialized = false;
    if (!initialized) {
        // Default guest heap: 1 MB at guest address 0x100000, ReadWrite.
        manager.MapGuestMemory(0x100000, 0x100000, platform::MemProtect::ReadWrite);
        initialized = true;
    }
    return manager;
}

} // namespace

// Recompiled function registry: guest address -> host function pointer.
// Each recompiled guest function registers itself via sr_rt_register_function
// before the entry point is invoked. CallGuest performs the dispatch.
using GuestFn = void (*)(GuestContext*);

static std::map<u64, GuestFn>& GetFunctionTable() {
    static std::map<u64, GuestFn> table;
    return table;
}

void* GuestToHost(GuestContext* /*ctx*/, u64 guest_addr, usize size, bool write) {
    return GetMemoryManager().GuestToHost(guest_addr, static_cast<u64>(size), write);
}

void CallGuest(GuestContext* ctx, u64 target) {
    auto& table = GetFunctionTable();
    auto it = table.find(target);
    if (it != table.end()) {
        it->second(ctx);
    } else {
        std::printf("runtime: no function registered at 0x%llx\n",
                    (unsigned long long)target);
    }
}

// HandleSvc is implemented in svc_table.cpp.

// ---- C-linkage runtime ABI used by generated C code ----

extern "C" void sr_rt_register_function(u64 guest_addr, GuestFn fn) {
    GetFunctionTable()[guest_addr] = fn;
}

extern "C" void sr_rt_call(GuestContext* ctx, u64 target) {
    CallGuest(ctx, target);
}

extern "C" void* sr_rt_guest_to_host(GuestContext* ctx, u64 addr, u64 size,
                                     int write) {
    return GuestToHost(ctx, addr, size, write != 0);
}

extern "C" void sr_rt_handle_svc(GuestContext* ctx, u32 svc_id) {
    HandleSvc(ctx, svc_id);
}

} // namespace sr::runtime
