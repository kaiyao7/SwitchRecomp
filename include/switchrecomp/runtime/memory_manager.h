#pragma once

#include "switchrecomp/common/types.h"
#include "switchrecomp/platform/memory.h"
#include <map>

namespace sr::runtime {

using sr::common::u32;
using sr::common::u64;
using sr::common::usize;
using sr::platform::MemProtect;

struct HostMapping {
    void* host_ptr;
    u64 size;
    MemProtect prot;
};

// Public description of a guest mapping, returned by QueryMapping.
struct MappingInfo {
    u64 guest_addr;          // mapping start address in guest space
    u64 size;                // mapping size in bytes
    MemProtect prot;         // mapping permissions
};

// Detailed result of an address translation, distinguishing "not mapped"
// from "mapped but permission denied".
enum class TranslateResult {
    Success,
    Unmapped,            // no mapping covers the requested range
    PermissionDenied,    // mapping exists but access (read/write) is not allowed
};

class MemoryManager {
public:
    // Map a guest virtual address range to host memory.
    bool MapGuestMemory(u64 guest_addr, u64 size, MemProtect prot);
    // Change the protection of an existing mapping (host pages + tracked
    // metadata). Returns false when no mapping starts at guest_addr.
    bool ProtectGuestMemory(u64 guest_addr, MemProtect prot);
    // Unmap a guest range.
    void UnmapGuestMemory(u64 guest_addr, u64 size);
    // Translate guest address to host pointer. Returns nullptr if unmapped or permission denied.
    void* GuestToHost(u64 guest_addr, u64 size, bool write);

    // Find the mapping that contains `guest_addr` and describe it in `out`.
    // Returns false when no mapping covers the address (out is left unchanged).
    bool QueryMapping(u64 guest_addr, MappingInfo& out) const;

    // Translate with an explicit status code so callers can tell unmapped
    // apart from permission failures. On success, *host (when non-null) is
    // set to the translated pointer.
    TranslateResult Translate(u64 guest_addr, u64 size, bool write,
                              void** host) const;

private:
    std::map<u64, HostMapping> mappings_; // guest_addr -> mapping

    // Iterator to the mapping that contains guest_addr, or mappings_.end().
    std::map<u64, HostMapping>::const_iterator
    FindMappingFor(u64 guest_addr) const;
};

} // namespace sr::runtime
