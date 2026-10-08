// Memory manager: guest virtual address -> host pointer translation.
#include "switchrecomp/runtime/memory_manager.h"
#include "switchrecomp/platform/memory.h"

namespace sr::runtime {

bool MemoryManager::MapGuestMemory(u64 guest_addr, u64 size, MemProtect prot) {
    if (size == 0) return false;

    void* host_ptr = sr::platform::AllocPages(static_cast<usize>(size), prot);
    if (!host_ptr) return false;

    HostMapping mapping{host_ptr, size, prot};
    mappings_[guest_addr] = mapping;
    return true;
}

bool MemoryManager::ProtectGuestMemory(u64 guest_addr, MemProtect prot) {
    auto it = mappings_.find(guest_addr);
    if (it == mappings_.end()) return false;

    if (!sr::platform::ProtectPages(it->second.host_ptr,
                                    static_cast<usize>(it->second.size), prot)) {
        return false;
    }
    it->second.prot = prot;
    return true;
}

void MemoryManager::UnmapGuestMemory(u64 guest_addr, u64 size) {
    auto it = mappings_.find(guest_addr);
    if (it != mappings_.end()) {
        sr::platform::FreePages(it->second.host_ptr, static_cast<usize>(it->second.size));
        mappings_.erase(it);
    }
    (void)size;
}

auto MemoryManager::FindMappingFor(u64 guest_addr) const
    -> std::map<u64, HostMapping>::const_iterator {
    // upper_bound gives the first mapping with key > guest_addr; the candidate
    // is the one before it.
    auto it = mappings_.upper_bound(guest_addr);
    if (it == mappings_.begin()) return mappings_.end();
    --it;

    const u64 map_start = it->first;
    const u64 map_end = map_start + it->second.size;
    if (guest_addr < map_start || guest_addr >= map_end) return mappings_.end();
    return it;
}

bool MemoryManager::QueryMapping(u64 guest_addr, MappingInfo& out) const {
    auto it = FindMappingFor(guest_addr);
    if (it == mappings_.end()) return false;

    out.guest_addr = it->first;
    out.size = it->second.size;
    out.prot = it->second.prot;
    return true;
}

TranslateResult MemoryManager::Translate(u64 guest_addr, u64 size, bool write,
                                         void** host) const {
    auto it = FindMappingFor(guest_addr);
    if (it == mappings_.end()) return TranslateResult::Unmapped;

    const u64 map_start = it->first;
    const HostMapping& m = it->second;

    // The whole requested range must stay within the mapping.
    if (size > 0 && (guest_addr + size) > (map_start + m.size)) {
        return TranslateResult::Unmapped;
    }

    // Permission check (bit flags: Read=1, Write=2, Exec=4).
    const bool readable = static_cast<u32>(m.prot) & static_cast<u32>(MemProtect::Read);
    const bool writable = static_cast<u32>(m.prot) & static_cast<u32>(MemProtect::Write);

    if (write && !writable) return TranslateResult::PermissionDenied;
    if (!write && !readable) return TranslateResult::PermissionDenied;

    if (host != nullptr) {
        const u64 offset = guest_addr - map_start;
        *host = static_cast<char*>(m.host_ptr) + offset;
    }
    return TranslateResult::Success;
}

void* MemoryManager::GuestToHost(u64 guest_addr, u64 size, bool write) {
    void* host = nullptr;
    if (Translate(guest_addr, size, write, &host) != TranslateResult::Success) {
        return nullptr;
    }
    return host;
}

} // namespace sr::runtime
