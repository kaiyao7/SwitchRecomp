#pragma once

#include "switchrecomp/common/types.h"
#include "switchrecomp/common/span.h"
#include "switchrecomp/common/result.h"
#include "switchrecomp/platform/memory.h"
#include "switchrecomp/runtime/memory_manager.h"

#include <vector>

namespace sr::runtime {

using sr::common::u8;
using sr::common::u64;

// Description of a single loaded image segment.
struct LoadedSegment {
    u64 guest_address;                 // guest virtual address of segment start
    u64 file_size;                     // bytes initialized from the image file
    u64 memory_size;                   // total mapped bytes (>= file_size; BSS after)
    platform::MemProtect protect;      // mapping permissions
};

// Result of loading an executable image.
struct LoadedImage {
    u64 entry_point;                          // guest address of the entry point
    std::vector<LoadedSegment> segments;      // all loaded PT_LOAD / NRO segments
};

// Load a standard little-endian ELF-64 image (AArch64, ET_EXEC/ET_DYN).
// Guest addresses are translated by `load_base` (segment vaddr + load_base).
common::Result<LoadedImage> LoadElf(MemoryManager& mm, common::span<const u8> data, u64 load_base);

// Load a Switch NRO image (magic "NRO0").
// Guest addresses follow the file-offset layout translated by `load_base`.
common::Result<LoadedImage> LoadNro(MemoryManager& mm, common::span<const u8> data, u64 load_base);

} // namespace sr::runtime
