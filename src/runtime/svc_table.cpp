// SVC dispatch table.
//
// Built-in SVCs are exposed as named HLE services: on first use the three
// built-in handlers are registered with the process-global HleServiceRegistry,
// and HandleSvc translates an SVC id to its service name and dispatches
// through the registry. Unmapped SVC ids keep the old warning behavior.
#include "switchrecomp/runtime/runtime.h"
#include "hle_service.h"
#include "svc_thread.h"

#include <cstdio>
#include <string_view>

namespace sr::runtime {

namespace {

void svc_SetHeapSize(GuestContext* ctx) {
    ctx->x[0] = 0x100000; // return heap base address
}

void svc_Mutex(GuestContext* /*ctx*/) {
    // placeholder: no-op mutex
}

void svc_OutputDebugString(GuestContext* ctx) {
    // ctx->x[0] = guest address of string; print first 256 chars
    void* host = GuestToHost(ctx, ctx->x[0], 256, false);
    if (host) {
        std::printf("debug: %s\n", static_cast<char*>(host));
    }
}

void svc_Unknown(GuestContext* /*ctx*/) {
    std::printf("warning: unimplemented SVC\n");
}

// SVC id -> service name + built-in handler.
struct SvcEntry {
    u32 id;
    std::string_view name;
    HleHandler handler;
};

constexpr SvcEntry kSvcEntries[] = {
    {0x01, "SetHeapSize",       &svc_SetHeapSize},
    {0x16, "Mutex",             &svc_Mutex},
    {0x21, "OutputDebugString", &svc_OutputDebugString},
};

// Resolve an SVC id to its registered service name; empty if unmapped.
std::string_view SvcServiceName(u32 svc_id) {
    for (const SvcEntry& entry : kSvcEntries) {
        if (entry.id == svc_id) return entry.name;
    }
    // Thread / synchronization SVCs live in svc_thread.cpp.
    return ThreadSvcName(svc_id);
}

void InitSvcServices() {
    HleServiceRegistry& registry = GlobalHleServices();
    for (const SvcEntry& entry : kSvcEntries) {
        // Duplicate names are not overwritten by design; ignore the result
        // (built-ins may already have been registered).
        registry.Register(entry.name, entry.handler);
    }
    // Register the thread / synchronization handlers by name.
    RegisterThreadSvcs(registry);
}

} // namespace

void HandleSvc(GuestContext* ctx, u32 svc_id) {
    static bool initialized = false;
    if (!initialized) {
        InitSvcServices();
        initialized = true;
    }

    const std::string_view name = SvcServiceName(svc_id);
    if (!name.empty() && GlobalHleServices().Invoke(name, ctx)) {
        return;
    }
    // No mapping for this SVC id, or its service was unregistered.
    svc_Unknown(ctx);
}

} // namespace sr::runtime
