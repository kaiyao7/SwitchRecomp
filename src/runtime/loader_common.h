#pragma once

// Internal shared helpers for the ELF/NRO loaders. Lives under src/runtime
// because it is an implementation detail of the runtime loaders; the public
// interface is switchrecomp/runtime/loader.h.

#include "switchrecomp/common/types.h"
#include "switchrecomp/common/span.h"
#include "switchrecomp/platform/memory.h"
#include "switchrecomp/runtime/memory_manager.h"

#include <string>

namespace sr::runtime {

using sr::common::u8;
using sr::common::u16;
using sr::common::u32;
using sr::common::u64;
using sr::common::usize;

// A segment requested by a format-specific loader, already expressed in final
// guest coordinates (i.e. load_base has been applied to guest_address).
struct RawSegment {
    u64 file_offset;                 // offset into the image file
    u64 guest_address;               // final guest virtual address
    u64 file_size;                   // bytes initialized from the file
    u64 memory_size;                 // total bytes (>= file_size; tail is BSS)
    platform::MemProtect protect;    // final mapping permissions
};

// Little-endian field readers (safe for unaligned addresses).
u16 ReadLE16(const u8* p);
u32 ReadLE32(const u8* p);
u64 ReadLE64(const u8* p);

// Round x down/up to `align` (must be a power of two).
u64 AlignDown(u64 x, u64 align);
u64 AlignUp(u64 x, u64 align);

// True when [offset, offset+size) lies within a file of `file_size` bytes.
// Empty ranges (size == 0) are valid; overflow is rejected.
bool FileRangeValid(u64 file_size, u64 offset, u64 size);

// Map every segment (page aligned, temporarily ReadWrite), copy its file
// bytes, explicitly zero the BSS tail, then drop to the final protection.
// On failure any mappings already created are removed and false is returned
// with a message in *err (when non-null).
bool LoadSegments(MemoryManager& mm, common::span<const u8> file_data,
                  const RawSegment* segments, usize count, std::string* err);

} // namespace sr::runtime
