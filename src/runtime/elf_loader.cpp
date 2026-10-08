// ELF-64 (little-endian, AArch64) image loader.
//
// Handles ET_EXEC and ET_DYN images, loads PT_LOAD segments, maps their
// permissions, copies file bytes and zero-initializes BSS. All other program
// header types are ignored. Guest addresses are translated by load_base.
#include "switchrecomp/runtime/loader.h"

#include "loader_common.h"

#include <vector>

namespace sr::runtime {

namespace {

// ELF identification indices and constants.
constexpr int EI_CLASS = 4;
constexpr int EI_DATA = 5;
constexpr int EI_VERSION = 6;
constexpr u8 ELFCLASS64 = 2;
constexpr u8 ELFDATA2LSB = 1;
constexpr u8 EV_CURRENT = 1;

constexpr u16 ET_EXEC = 2;
constexpr u16 ET_DYN = 3;
constexpr u16 EM_AARCH64 = 183;

constexpr u32 PT_LOAD = 1;
constexpr u32 PF_X = 1;
constexpr u32 PF_W = 2;
constexpr u32 PF_R = 4;

// ELF-64 header size and field offsets.
constexpr usize EHDR_SIZE = 64;
constexpr u64 E_ENTRY = 24;
constexpr u64 E_PHOFF = 32;
constexpr u64 E_PHENTSIZE = 54;
constexpr u64 E_PHNUM = 56;

// ELF-64 program header size and field offsets.
constexpr usize PHDR_SIZE = 56;
constexpr u64 P_TYPE = 0;
constexpr u64 P_FLAGS = 4;
constexpr u64 P_OFFSET = 8;
constexpr u64 P_VADDR = 16;
constexpr u64 P_FILESZ = 32;
constexpr u64 P_MEMSZ = 40;

// Convert ELF permission bits to a platform MemProtect.
platform::MemProtect FlagsToProtect(u32 flags) {
    u32 value = 0;
    if (flags & PF_R) value |= static_cast<u32>(platform::MemProtect::Read);
    if (flags & PF_W) value |= static_cast<u32>(platform::MemProtect::Write);
    if (flags & PF_X) value |= static_cast<u32>(platform::MemProtect::Exec);
    return static_cast<platform::MemProtect>(value);
}

} // namespace

common::Result<LoadedImage> LoadElf(MemoryManager& mm,
                                    common::span<const u8> data,
                                    u64 load_base) {
    if (data.size() < EHDR_SIZE) {
        return common::Error{"ELF: truncated header"};
    }
    const u8* base = data.data();

    // Magic: 7f 'E' 'L' 'F'.
    static const u8 kMagic[4] = {0x7f, 'E', 'L', 'F'};
    for (int i = 0; i < 4; ++i) {
        if (base[i] != kMagic[i]) {
            return common::Error{"ELF: bad magic"};
        }
    }
    if (base[EI_CLASS] != ELFCLASS64) {
        return common::Error{"ELF: not a 64-bit object"};
    }
    if (base[EI_DATA] != ELFDATA2LSB) {
        return common::Error{"ELF: not little-endian"};
    }
    if (base[EI_VERSION] != EV_CURRENT) {
        return common::Error{"ELF: bad ident version"};
    }

    const u16 e_type = ReadLE16(base + 16);
    const u16 e_machine = ReadLE16(base + 18);
    const u32 e_version = ReadLE32(base + 20);

    if (e_type != ET_EXEC && e_type != ET_DYN) {
        return common::Error{"ELF: unsupported e_type"};
    }
    if (e_machine != EM_AARCH64) {
        return common::Error{"ELF: machine is not AArch64"};
    }
    if (e_version != EV_CURRENT) {
        return common::Error{"ELF: bad ELF version"};
    }

    const u64 e_entry = ReadLE64(base + E_ENTRY);
    const u64 e_phoff = ReadLE64(base + E_PHOFF);
    const u16 e_phentsize = ReadLE16(base + E_PHENTSIZE);
    const u16 e_phnum = ReadLE16(base + E_PHNUM);

    if (e_phentsize < PHDR_SIZE) {
        return common::Error{"ELF: phentsize too small"};
    }
    // Validate the whole program header table is present (reject overflow).
    const u64 ph_table_bytes = static_cast<u64>(e_phnum) * e_phentsize;
    if (e_phoff > data.size() || ph_table_bytes > data.size() - e_phoff) {
        return common::Error{"ELF: program header table out of bounds"};
    }

    std::vector<RawSegment> raw;
    std::vector<LoadedSegment> loaded;

    for (u16 i = 0; i < e_phnum; ++i) {
        const u8* ph = base + e_phoff + static_cast<u64>(i) * e_phentsize;
        const u32 p_type = ReadLE32(ph + P_TYPE);
        if (p_type != PT_LOAD) continue;

        const u32 p_flags = ReadLE32(ph + P_FLAGS);
        const u64 p_offset = ReadLE64(ph + P_OFFSET);
        const u64 p_vaddr = ReadLE64(ph + P_VADDR);
        const u64 p_filesz = ReadLE64(ph + P_FILESZ);
        const u64 p_memsz = ReadLE64(ph + P_MEMSZ);

        if (p_memsz < p_filesz) {
            return common::Error{"ELF: memsz < filesz"};
        }

        // Translate guest address, rejecting overflow.
        if (load_base + p_vaddr < load_base) {
            return common::Error{"ELF: guest address overflow"};
        }
        const u64 guest = load_base + p_vaddr;

        const platform::MemProtect protect = FlagsToProtect(p_flags);
        raw.push_back(RawSegment{p_offset, guest, p_filesz, p_memsz, protect});
        loaded.push_back(LoadedSegment{guest, p_filesz, p_memsz, protect});
    }

    std::string err;
    if (!LoadSegments(mm, data, raw.data(), raw.size(), &err)) {
        return common::Error{"ELF: " + err};
    }

    if (load_base + e_entry < load_base) {
        return common::Error{"ELF: entry overflow"};
    }

    LoadedImage image;
    image.entry_point = load_base + e_entry;
    image.segments = std::move(loaded);
    return image;
}

} // namespace sr::runtime
