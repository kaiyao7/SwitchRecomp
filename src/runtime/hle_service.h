#pragma once

// Internal HLE (high-level emulation) service registry.
//
// Services are named handlers that recompiled guest code (or the SVC table)
// can invoke by name. This header lives under src/runtime because the registry
// is an internal runtime facility; the public runtime header is unchanged.

#include "switchrecomp/runtime/runtime.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace sr::runtime {

using HleHandler = void (*)(GuestContext*);

class HleServiceRegistry {
public:
    // Register a service under `name` with handler `handler`.
    // Registering a name that already exists fails (returns false) and the
    // existing handler is NOT replaced. A null handler is rejected.
    bool Register(std::string_view name, HleHandler handler);

    // Remove a previously registered service.
    // Returns false if `name` was not registered.
    bool Unregister(std::string_view name);

    // Whether a service with the given name is currently registered.
    bool Contains(std::string_view name) const;

    // Invoke the named service with `ctx`. Returns false (without crashing)
    // when no such service is registered.
    bool Invoke(std::string_view name, GuestContext* ctx) const;

    // Sorted list of currently registered service names.
    std::vector<std::string> ListServices() const;

private:
    // std::less<> enables heterogeneous lookup with std::string_view.
    std::map<std::string, HleHandler, std::less<>> services_;
};

// Process-global registry. Recompiled code reaches it through the
// extern "C" wrappers declared below / implemented in hle_service.cpp.
HleServiceRegistry& GlobalHleServices();

// ---- C-linkage ABI for generated code ----
// Mirrors the sr_rt_* style used in runtime.cpp.

extern "C" bool sr_hle_register(const char* name, HleHandler handler);
extern "C" bool sr_hle_invoke(const char* name, GuestContext* ctx);

} // namespace sr::runtime
