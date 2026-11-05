#pragma once

#include "switchrecomp/common/span.h"
#include "switchrecomp/common/result.h"
#include "switchrecomp/platform/memory.h"
#include "switchrecomp/runtime/loader.h"
#include "switchrecomp/runtime/memory_manager.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sr::runtime {

using sr::common::u8;
using sr::common::u16;
using sr::common::u32;
using sr::common::u64;
using sr::common::s32;
using sr::common::s64;

// ---- Switch NSO ("NSO0") executable-image parsing. ----
//
// A Switch title ships as one or more NSO modules (main, sdk, subsdk0..9)
// that the Horizon loader (rtld) parses, relocates, and lays out in a fixed
// load order. This header exposes that binary module-system understanding to
// the runtime: ParseNso turns a raw NSO file into a parsed image plus its
// dynamic metadata (MOD0, DT_NEEDED, symbols, relocations), and LoadNso /
// LoadModuleSystem map it into guest memory.

// LZ4 raw-block decompression (no 4-byte size prefix), as used by NSO
// compressed segments. Returns false with `err` set on a malformed block.
bool Lz4BlockDecompress(const u8* src, u64 src_len, u8* dst, u64 dst_len,
                        std::string* err);

// A parsed symbol table entry (mirrors Elf64_Sym, 24 bytes on disk).
struct NsoSymbol {
    std::string name;
    u32 info;    // bind<<4 | type
    u16 shndx;   // 0 for imported/undefined symbols
    u64 value;   // section-relative value for defined symbols
    u64 size;
};

// A parsed relocation entry (mirrors Elf64_Rela, 24 bytes on disk).
struct NsoRelocation {
    u64 offset;  // write target (guest address before load base is applied)
    u32 type;    // R_AARCH64_* constant
    u32 sym;     // index into the symbol table
    s64 addend;
};

// Metadata parsed from a single NSO module.
struct NsoImage {
    std::string name;          // caller-assigned module name ("main", ...)
    std::string build_id;      // 64 hex chars from the NSO build-id field

    // Decompressed segments (raw bytes) plus their memory-image offsets.
    // Guest address of a segment = load_base + *_mem_off.
    std::vector<u8> text;
    std::vector<u8> rodata;
    std::vector<u8> data;
    u64 text_mem_off;
    u64 rodata_mem_off;
    u64 data_mem_off;
    u64 bss_size;              // zeroed tail after .data

    // Dynamic metadata parsed from MOD0.
    std::vector<std::string> needed;         // DT_NEEDED module names
    std::vector<NsoSymbol> symbols;          // .dynsym entries
    std::vector<NsoRelocation> relocations;  // DT_RELA + DT_JMPREL + DT_RELR

    // Convenience accessors.
    bool has_symbols() const { return !symbols.empty(); }
};

// Parse a raw NSO file (decompressing segments) into an NsoImage. Pure:
// performs no guest-memory allocation. Returns an error on bad magic,
// truncated header, bad segment ranges, or a malformed LZ4 block.
common::Result<NsoImage> ParseNso(common::span<const u8> data,
                                  std::string_view name);

// Load a single NSO module into guest memory at `load_base` (all three
// segments, BSS zeroed, permissions per segment). No relocation is applied;
// use LoadModuleSystem for cross-module resolution.
common::Result<LoadedImage> LoadNso(MemoryManager& mm,
                                    common::span<const u8> data,
                                    u64 load_base, std::string_view name);

// One module to load, in load order (main, subsdk0..9, sdk).
struct ModuleInput {
    std::string name;              // module name used for DT_NEEDED matching
    std::vector<u8> bytes;         // raw NSO file contents
};

// Result of loading a whole title's module set.
struct ModuleSystem {
    struct Placed {
        std::string name;
        u64 base;                       // resolved guest base address
        LoadedImage image;              // mapped segments
    };
    std::vector<Placed> modules;        // in load order
    u64 first_base;                     // base of the first module
};

// Lay out, relocate, and map a set of NSO modules the way the Horizon loader
// does: modules are placed in the given order (already main, subsdk*, sdk),
// each base 2 MiB aligned, everything below 2^39. DT_NEEDED names are matched
// against the loaded set and R_AARCH64_* relocations are resolved across
// module boundaries and written into the mapped images before permissions are
// dropped to their final (non-writable) values.
common::Result<ModuleSystem> LoadModuleSystem(MemoryManager& mm,
                                              std::vector<ModuleInput> modules,
                                              u64 first_base);

// ---- relocation type constants (R_AARCH64_*) ----
inline constexpr u32 R_AARCH64_ABS64 = 257;
inline constexpr u32 R_AARCH64_GLOB_DAT = 1025;
inline constexpr u32 R_AARCH64_JUMP_SLOT = 1026;
inline constexpr u32 R_AARCH64_RELATIVE = 1027;

} // namespace sr::runtime
