// Shared loader implementation: page-aligned mapping, file copy, BSS zeroing.
#include "loader_common.h"

#include <cstring>
#include <utility>
#include <vector>

namespace sr::runtime {

u16 ReadLE16(const u8* p) {
    return static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8);
}

u32 ReadLE32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

u64 ReadLE64(const u8* p) {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<u64>(p[i]) << (i * 8);
    }
    return v;
}

u64 AlignDown(u64 x, u64 align) {
    return x - (x % align);
}

u64 AlignUp(u64 x, u64 align) {
    return AlignDown(x + align - 1u, align);
}

bool FileRangeValid(u64 file_size, u64 offset, u64 size) {
    if (offset > file_size) return false;
    // Guard against offset + size overflow.
    if (size > file_size - offset) return false;
    return true;
}

bool LoadSegments(MemoryManager& mm, common::span<const u8> file_data,
                  const RawSegment* segments, usize count, std::string* err) {
    const u64 page = static_cast<u64>(platform::PageSize());
    const u64 file_len = file_data.size();

    // Aligned mapping bases successfully created, for cleanup on failure.
    std::vector<u64> created;
    bool ok = true;

    for (usize i = 0; i < count; ++i) {
        const RawSegment& seg = segments[i];

        if (seg.memory_size == 0) continue;
        if (seg.memory_size < seg.file_size) {
            if (err) *err = "segment memory_size < file_size";
            ok = false;
            break;
        }
        if (!FileRangeValid(file_len, seg.file_offset, seg.file_size)) {
            if (err) *err = "segment file range out of bounds";
            ok = false;
            break;
        }
        // guest_address + memory_size must not overflow.
        if (seg.guest_address + seg.memory_size < seg.guest_address) {
            if (err) *err = "segment guest range overflow";
            ok = false;
            break;
        }

        const u64 aligned_start = AlignDown(seg.guest_address, page);
        const u64 seg_end = seg.guest_address + seg.memory_size;
        const u64 aligned_end = AlignUp(seg_end, page);
        const u64 map_size = aligned_end - aligned_start;

        // Map writable so file bytes can be loaded; drop permissions later.
        if (!mm.MapGuestMemory(aligned_start, map_size,
                               platform::MemProtect::ReadWrite)) {
            if (err) *err = "failed to map guest memory";
            ok = false;
            break;
        }
        created.push_back(aligned_start);

        // Host pointer for the (possibly unaligned) segment start.
        void* host = mm.GuestToHost(seg.guest_address, seg.memory_size, true);
        if (host == nullptr) {
            if (err) *err = "failed to translate mapped segment";
            ok = false;
            break;
        }

        auto* base = static_cast<u8*>(host);
        if (seg.file_size > 0) {
            std::memcpy(base, file_data.data() + seg.file_offset,
                        static_cast<size_t>(seg.file_size));
        }
        // Explicitly zero the BSS tail [file_size, memory_size).
        const u64 bss = seg.memory_size - seg.file_size;
        if (bss > 0) {
            std::memset(base + seg.file_size, 0, static_cast<size_t>(bss));
        }

        // Drop to the final protection. Tracked metadata is updated too.
        if (!mm.ProtectGuestMemory(aligned_start, seg.protect)) {
            if (err) *err = "failed to apply final segment protection";
            ok = false;
            break;
        }
    }

    if (!ok) {
        for (u64 base_addr : created) {
            MappingInfo info;
            if (mm.QueryMapping(base_addr, info)) {
                mm.UnmapGuestMemory(base_addr, info.size);
            }
        }
        return false;
    }

    return true;
}

} // namespace sr::runtime
