// Switch NRO image loader ("NRO0").
//
// Uses the simplified, self-consistent layout where a segment's guest offset
// equals its file offset (header occupies the leading bytes). Segment
// permissions are fixed by index: seg0 ReadExec (.text), seg1 Read
// (.rodata), seg2 ReadWrite (.data + BSS). Empty segments are skipped.
#include "switchrecomp/runtime/loader.h"

#include "loader_common.h"

#include <array>
#include <vector>

namespace sr::runtime {

namespace {

constexpr u32 NRO_MAGIC = 0x304F524Eu; // 'N','R','O','0' little-endian
constexpr u32 NRO_VERSION = 0;

// Header field offsets.
constexpr u64 NRO_MAGIC_OFF = 0x00;
constexpr u64 NRO_VERSION_OFF = 0x04;
constexpr u64 NRO_TOTAL_SIZE_OFF = 0x08;
constexpr u64 NRO_SEG0_OFF = 0x10; // seg headers: off,size each
constexpr u64 NRO_SEG1_OFF = 0x18;
constexpr u64 NRO_SEG2_OFF = 0x20;
constexpr u64 NRO_BSS_SIZE_OFF = 0x28;
constexpr usize NRO_MIN_HEADER = 0x30;

struct SegmentSpec {
    u64 header_off;
    platform::MemProtect protect;
};

} // namespace

common::Result<LoadedImage> LoadNro(MemoryManager& mm,
                                    common::span<const u8> data,
                                    u64 load_base) {
    if (data.size() < NRO_MIN_HEADER) {
        return common::Error{"NRO: truncated header"};
    }
    const u8* base = data.data();

    const u32 magic = ReadLE32(base + NRO_MAGIC_OFF);
    if (magic != NRO_MAGIC) {
        return common::Error{"NRO: bad magic"};
    }
    const u32 version = ReadLE32(base + NRO_VERSION_OFF);
    if (version != NRO_VERSION) {
        return common::Error{"NRO: unsupported version"};
    }
    const u32 total_size = ReadLE32(base + NRO_TOTAL_SIZE_OFF);
    if (total_size < NRO_MIN_HEADER || total_size > data.size()) {
        return common::Error{"NRO: invalid total_size"};
    }
    const u32 bss_size = ReadLE32(base + NRO_BSS_SIZE_OFF);

    constexpr std::array<SegmentSpec, 3> specs = {{
        {NRO_SEG0_OFF, platform::MemProtect::ReadExec},
        {NRO_SEG1_OFF, platform::MemProtect::Read},
        {NRO_SEG2_OFF, platform::MemProtect::ReadWrite},
    }};

    std::vector<RawSegment> raw;
    std::vector<LoadedSegment> loaded;
    u64 seg0_offset = 0;

    for (usize i = 0; i < specs.size(); ++i) {
        const u32 file_offset = ReadLE32(base + specs[i].header_off);
        const u32 file_size = ReadLE32(base + specs[i].header_off + 4);

        u64 memory_size = file_size;
        if (i == 2) {
            memory_size = static_cast<u64>(file_size) + bss_size;
        }
        if (memory_size == 0) continue;

        if (!FileRangeValid(data.size(), file_offset, file_size)) {
            return common::Error{"NRO: segment file range out of bounds"};
        }
        if (load_base + file_offset < load_base) {
            return common::Error{"NRO: guest address overflow"};
        }
        const u64 guest = load_base + file_offset;

        if (i == 0) seg0_offset = file_offset;

        raw.push_back(RawSegment{file_offset, guest, file_size, memory_size,
                                 specs[i].protect});
        loaded.push_back(LoadedSegment{guest, file_size, memory_size,
                                       specs[i].protect});
    }

    if (raw.empty()) {
        return common::Error{"NRO: no loadable segments"};
    }

    std::string err;
    if (!LoadSegments(mm, data, raw.data(), raw.size(), &err)) {
        return common::Error{"NRO: " + err};
    }

    LoadedImage image;
    image.entry_point = load_base + seg0_offset;
    image.segments = std::move(loaded);
    return image;
}

} // namespace sr::runtime
