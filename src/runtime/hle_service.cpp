// HLE service registry implementation.
#include "hle_service.h"

namespace sr::runtime {

bool HleServiceRegistry::Register(std::string_view name, HleHandler handler) {
    if (handler == nullptr) return false;
    // emplace fails (inserted == false) when the key already exists, so an
    // existing handler is never overwritten.
    auto result = services_.emplace(std::string(name), handler);
    return result.second;
}

bool HleServiceRegistry::Unregister(std::string_view name) {
    auto it = services_.find(name);
    if (it == services_.end()) return false;
    services_.erase(it);
    return true;
}

bool HleServiceRegistry::Contains(std::string_view name) const {
    return services_.find(name) != services_.end();
}

bool HleServiceRegistry::Invoke(std::string_view name, GuestContext* ctx) const {
    auto it = services_.find(name);
    if (it == services_.end()) return false;
    it->second(ctx);
    return true;
}

std::vector<std::string> HleServiceRegistry::ListServices() const {
    std::vector<std::string> names;
    names.reserve(services_.size());
    for (const auto& kv : services_) {
        names.push_back(kv.first);
    }
    // std::map iterates in sorted key order already.
    return names;
}

HleServiceRegistry& GlobalHleServices() {
    static HleServiceRegistry registry;
    return registry;
}

// ---- C-linkage ABI for generated code ----

extern "C" bool sr_hle_register(const char* name, HleHandler handler) {
    if (name == nullptr) return false;
    return GlobalHleServices().Register(name, handler);
}

extern "C" bool sr_hle_invoke(const char* name, GuestContext* ctx) {
    if (name == nullptr) return false;
    return GlobalHleServices().Invoke(name, ctx);
}

} // namespace sr::runtime
