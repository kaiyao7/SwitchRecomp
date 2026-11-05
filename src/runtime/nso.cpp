// Switch NSO ("NSO0") module loading and the binary module system.
//
// Decompresses the three LZ4 segments, builds the memory image, parses MOD0 /
// dynamic / symbols / relocations, then (for LoadModuleSystem) lays modules
// out in the fixed Horizon load order, resolves R_AARCH64_* relocations
// across module boundaries, and drops the segments to their final
// permissions.
#include "switchrecomp/runtime/nso.h"

#include "loader_common.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>

namespace sr::runtime {

namespace {

// ---- dynamic / relocation constants ----
constexpr u64 DT_NULL = 0;
constexpr u64 DT_NEEDED = 1;
constexpr u64 DT_PLTRELSZ = 2;
constexpr u64 DT_HASH = 4;
constexpr u64 DT_STRTAB = 5;
constexpr u64 DT_SYMTAB = 6;
constexpr u64 DT_RELA = 7;
constexpr u64 DT_RELASZ = 8;
constexpr u64 DT_JMPREL = 23;
constexpr u64 DT_GNU_HASH = 0x6FFFFEF5ULL;
constexpr u64 DT_RELR = 0x24;
constexpr u64 DT_RELRSZ = 0x23;

constexpr u32 NSO_MAGIC = 0x304F534Eu;  // 'N','S','O','0' little-endian

constexpr u64 MODULE_ALIGN = 0x200000;      // 2 MiB
constexpr u64 ADDRESS_LIMIT = 1ull << 39;   // IPC encodes 39-bit addresses

// Bounds-checked reader over the decompressed memory image. Every read that
// can go out of bounds sets `ok` false and leaves `err` set; callers check
// `ok` once after the sequence of reads they need.
struct ImageReader {
    const u8* p;
    u64 n;
    bool ok = true;
    std::string err;

    bool range(u64 off, u64 len) const {
        return off <= n && len <= n - off;
    }

    u8 byte(u64 off) {
        if (!range(off, 1)) { fail("read out of bounds"); return 0; }
        return p[off];
    }

    u32 rd32(u64 off) {
        if (!range(off, 4)) { fail("read32 out of bounds"); return 0; }
        return ReadLE32(p + off);
    }

    u64 rd64(u64 off) {
        if (!range(off, 8)) { fail("read64 out of bounds"); return 0; }
        return ReadLE64(p + off);
    }

    // NUL-terminated string at `off`. Empty on failure.
    std::string cstr(u64 off) {
        u64 end = off;
        while (end < n && p[end] != 0) ++end;
        if (end >= n) { fail("unterminated string"); return {}; }
        return std::string(reinterpret_cast<const char*>(p + off),
                           static_cast<size_t>(end - off));
    }

    void fail(const char* msg) {
        if (ok) err = msg;
        ok = false;
    }
};

} // namespace

bool Lz4BlockDecompress(const u8* src, u64 src_len, u8* dst, u64 dst_len,
                        std::string* err) {
    if (dst_len == 0) return src_len == 0;

    u64 ip = 0, op = 0;
    while (ip < src_len) {
        const u8 token = src[ip++];

        // Literal length (high nibble); 15 means more bytes follow.
        u64 lit_len = token >> 4;
        if (lit_len == 15) {
            while (true) {
                if (ip >= src_len) { if (err) *err = "truncated literal length"; return false; }
                const u8 b = src[ip++];
                lit_len += b;
                if (b != 255) break;
            }
        }
        if (lit_len > src_len - ip || lit_len > dst_len - op) {
            if (err) *err = "literal run out of bounds";
            return false;
        }
        std::memcpy(dst + op, src + ip, static_cast<size_t>(lit_len));
        ip += lit_len;
        op += lit_len;

        // The last sequence is literals only (no match).
        if (op == dst_len) break;
        if (ip + 2 > src_len) {
            if (err) *err = "missing match offset";
            return false;
        }

        const u16 offset = static_cast<u16>(src[ip] | (static_cast<u16>(src[ip + 1]) << 8));
        ip += 2;
        if (offset == 0 || static_cast<u64>(offset) > op) {
            if (err) *err = "invalid match offset";
            return false;
        }

        // Match length = 4 + low nibble; 15 means more bytes follow.
        u64 match_len = 4 + (token & 0x0F);
        if ((token & 0x0F) == 15) {
            while (true) {
                if (ip >= src_len) { if (err) *err = "truncated match length"; return false; }
                const u8 b = src[ip++];
                match_len += b;
                if (b != 255) break;
            }
        }
        if (match_len > dst_len - op) {
            if (err) *err = "match run out of bounds";
            return false;
        }
        // Byte-by-byte copy permits overlapping runs (RLE depends on this).
        for (u64 i = 0; i < match_len; ++i) {
            dst[op] = dst[op - offset];
            ++op;
        }
    }

    if (op != dst_len) {
        if (err) *err = "decompressed size mismatch";
        return false;
    }
    return true;
}

namespace {

// Build the contiguous memory image from the three decompressed segments and
// the BSS tail. The image extends to the farthest segment end (data + BSS in
// the real layout), sized up to 4 KiB, with the gaps zero-filled.
std::vector<u8> BuildMemoryImage(const NsoImage& img, u64& image_size) {
    u64 end = img.data_mem_off + img.data.size() + img.bss_size;
    end = std::max(end, img.text_mem_off + img.text.size());
    end = std::max(end, img.rodata_mem_off + img.rodata.size());
    image_size = (end + 0xFFF) & ~0xFFFull;
    std::vector<u8> image(static_cast<size_t>(image_size), 0);

    auto place = [&](const std::vector<u8>& seg, u64 mem_off) {
        if (!seg.empty()) {
            std::memcpy(image.data() + mem_off, seg.data(), seg.size());
        }
    };
    place(img.text, img.text_mem_off);
    place(img.rodata, img.rodata_mem_off);
    place(img.data, img.data_mem_off);
    return image;
}

// Walk the MOD0 dynamic section and record the {tag, value} pairs. Scalar
// tags keep their first occurrence (deduplicated); DT_NEEDED may appear
// several times and every value is collected in order into `needed`.
bool ParseDynamic(ImageReader& r, u64 dyn_off, std::map<u64, u64>& dyn,
                  std::vector<u64>& needed) {
    u64 off = dyn_off;
    while (true) {
        if (!r.range(off, 16)) { r.fail("dynamic section out of bounds"); return false; }
        const u64 tag = r.rd64(off);
        const u64 val = r.rd64(off + 8);
        if (!r.ok) return false;
        if (tag == DT_NULL) break;
        if (tag == DT_NEEDED) {
            needed.push_back(val);
        } else {
            dyn.emplace(tag, val);
        }
        off += 16;
    }
    return true;
}

// Symbol count: DT_HASH nchain, else the highest symbol reached by a
// DT_GNU_HASH chain (1.4.3 sdk/subsdk0 have no DT_HASH).
u64 SymbolCount(ImageReader& r, const std::map<u64, u64>& dyn) {
    auto hash = dyn.find(DT_HASH);
    if (hash != dyn.end()) {
        return r.rd32(hash->second + 4);
    }
    auto gnu = dyn.find(DT_GNU_HASH);
    if (gnu == dyn.end()) return 0;

    const u64 h = gnu->second;
    const u32 nbuckets = r.rd32(h);
    const u32 symoffset = r.rd32(h + 4);
    const u32 bloom_size = r.rd32(h + 8);
    const u64 buckets = h + 16 + static_cast<u64>(bloom_size) * 8;

    u32 last = 0;
    for (u32 i = 0; i < nbuckets; ++i) {
        last = std::max(last, r.rd32(buckets + static_cast<u64>(i) * 4));
    }
    if (last < symoffset) return symoffset;

    const u64 chains = buckets + static_cast<u64>(nbuckets) * 4;
    while (!(r.rd32(chains + static_cast<u64>(last - symoffset) * 4) & 1)) {
        ++last;
    }
    return static_cast<u64>(last) + 1;
}

} // namespace

common::Result<NsoImage> ParseNso(common::span<const u8> data,
                                  std::string_view name) {
    const u8* base = data.data();
    const u64 size = data.size();
    constexpr u64 HEADER = 0x100;

    if (size < HEADER) {
        return common::Error{"NSO: truncated header"};
    }
    if (ReadLE32(base) != NSO_MAGIC) {
        return common::Error{"NSO: bad magic"};
    }
    const u32 flags = ReadLE32(base + 0x0C);
    const u64 bss_size = ReadLE32(base + 0x3C);

    NsoImage img;
    img.name = std::string(name);
    // Build id: 32 bytes at 0x40.
    char hex[65];
    static const char* kHex = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        hex[i * 2] = kHex[base[0x40 + i] >> 4];
        hex[i * 2 + 1] = kHex[base[0x40 + i] & 0xF];
    }
    hex[64] = 0;
    img.build_id = hex;
    img.bss_size = bss_size;

    // Decompress the three segments.
    struct SegOut {
        std::vector<u8>* dst;
        u64* mem_off;
    };
    std::vector<u8> text, rodata, data_;
    SegOut segs[3] = {
        {&text, &img.text_mem_off},
        {&rodata, &img.rodata_mem_off},
        {&data_, &img.data_mem_off},
    };

    for (int i = 0; i < 3; ++i) {
        const u64 hdr = 0x10 + static_cast<u64>(i) * 0x10;
        const u32 file_off = ReadLE32(base + hdr);
        const u32 mem_off = ReadLE32(base + hdr + 4);
        const u32 dec_size = ReadLE32(base + hdr + 8);
        const u32 comp_size = ReadLE32(base + 0x60 + static_cast<u64>(i) * 4);

        *segs[i].mem_off = mem_off;
        if (dec_size == 0) continue;

        if (!FileRangeValid(size, file_off, comp_size)) {
            return common::Error{"NSO: segment file range out of bounds"};
        }
        const u8* src = base + file_off;
        std::vector<u8> out(dec_size);
        if (flags & (1u << i)) {
            std::string err;
            if (!Lz4BlockDecompress(src, comp_size, out.data(), dec_size, &err)) {
                return common::Error{"NSO: segment " + std::to_string(i) + " LZ4: " + err};
            }
        } else {
            if (comp_size != dec_size) {
                return common::Error{"NSO: uncompressed segment size mismatch"};
            }
            std::memcpy(out.data(), src, dec_size);
        }
        *segs[i].dst = std::move(out);
    }

    img.text = std::move(text);
    img.rodata = std::move(rodata);
    img.data = std::move(data_);

    // Build the memory image and parse MOD0 / dynamic / symbols / relocations.
    u64 image_size = 0;
    std::vector<u8> image = BuildMemoryImage(img, image_size);
    ImageReader r;
    r.p = image.data();
    r.n = image_size;

    const u64 mod0 = r.rd32(4);
    if (!r.ok) return common::Error{"NSO: image too small for MOD0 offset"};
    if (!r.range(mod0, 0x1C) || std::memcmp(image.data() + mod0, "MOD0", 4) != 0) {
        return common::Error{"NSO: bad MOD0 header"};
    }
    auto mod0_rel = [&](u32 idx) -> u64 {
        const s64 rel = static_cast<s32>(r.rd32(mod0 + 4 + static_cast<u64>(idx) * 4));
        return mod0 + static_cast<u64>(rel);
    };

    std::map<u64, u64> dyn;
    std::vector<u64> needed;
    if (!ParseDynamic(r, mod0_rel(0), dyn, needed)) {
        return common::Error{"NSO: " + r.err};
    }

    // DT_NEEDED module names (all occurrences, in order).
    const u64 strtab = dyn.count(DT_STRTAB) ? dyn[DT_STRTAB] : 0;
    for (const u64 val : needed) {
        img.needed.push_back(r.cstr(strtab + val));
        if (!r.ok) return common::Error{"NSO: bad DT_NEEDED"};
    }

    // Symbols.
    const u64 nsyms = SymbolCount(r, dyn);
    if (dyn.count(DT_SYMTAB)) {
        const u64 st = dyn[DT_SYMTAB];
        for (u64 i = 0; i < nsyms; ++i) {
            const u64 o = st + i * 24;
            if (!r.range(o, 24)) {
                return common::Error{"NSO: symbol table out of bounds"};
            }
            NsoSymbol sym;
            sym.name = r.cstr(strtab + r.rd32(o));
            sym.info = r.byte(o + 4);
            sym.shndx = static_cast<u16>(r.rd32(o + 6) & 0xFFFF);
            sym.value = r.rd64(o + 8);
            sym.size = r.rd64(o + 16);
            if (!r.ok) return common::Error{"NSO: bad symbol table"};
            img.symbols.push_back(std::move(sym));
        }
    }

    // Relocations: DT_RELA + DT_JMPREL (24-byte Elf64_Rela), then DT_RELR.
    for (const auto& [base_tag, size_tag] :
         std::vector<std::pair<u64, u64>>{{DT_RELA, DT_RELASZ}, {DT_JMPREL, DT_PLTRELSZ}}) {
        auto it = dyn.find(base_tag);
        if (it == dyn.end()) continue;
        const u64 b = it->second;
        const u64 count = dyn[size_tag] / 24;
        for (u64 i = 0; i < count; ++i) {
            const u64 o = b + i * 24;
            if (!r.range(o, 24)) {
                return common::Error{"NSO: relocation table out of bounds"};
            }
            NsoRelocation reloc;
            reloc.offset = r.rd64(o);
            const u64 info = r.rd64(o + 8);
            reloc.type = static_cast<u32>(info & 0xFFFFFFFFu);
            reloc.sym = static_cast<u32>(info >> 32);
            reloc.addend = static_cast<s64>(r.rd64(o + 16));
            if (!r.ok) return common::Error{"NSO: bad relocation table"};
            img.relocations.push_back(std::move(reloc));
        }
    }
    {
        auto it = dyn.find(DT_RELR);
        if (it != dyn.end()) {
            const u64 b = it->second;
            const u64 count = dyn[DT_RELRSZ] / 8;
            u64 where = 0;
            for (u64 i = 0; i < count; ++i) {
                const u64 e = r.rd64(b + i * 8);
                if (!r.ok) return common::Error{"NSO: bad RELR table"};
                if ((e & 1) == 0) {
                    NsoRelocation reloc;
                    reloc.offset = e;
                    reloc.type = R_AARCH64_RELATIVE;
                    reloc.sym = 0;
                    reloc.addend = static_cast<s64>(r.rd64(e));  // addend is the word in place
                    if (!r.ok) return common::Error{"NSO: RELR target out of bounds"};
                    img.relocations.push_back(std::move(reloc));
                    where = e + 8;
                } else {
                    for (int k = 0; k < 63; ++k) {
                        if ((e >> (k + 1)) & 1) {
                            const u64 a = where + static_cast<u64>(k) * 8;
                            NsoRelocation reloc;
                            reloc.offset = a;
                            reloc.type = R_AARCH64_RELATIVE;
                            reloc.sym = 0;
                            reloc.addend = static_cast<s64>(r.rd64(a));
                            if (!r.ok) return common::Error{"NSO: RELR target out of bounds"};
                            img.relocations.push_back(std::move(reloc));
                        }
                    }
                    where += 63 * 8;
                }
            }
        }
    }

    return img;
}

common::Result<LoadedImage> LoadNso(MemoryManager& mm,
                                    common::span<const u8> data,
                                    u64 load_base, std::string_view name) {
    auto parsed = ParseNso(data, name);
    if (!parsed) return common::Error{parsed.error()};

    NsoImage& img = parsed.value();
    u64 image_size = 0;
    std::vector<u8> image = BuildMemoryImage(img, image_size);

    RawSegment raw[3];
    int count = 0;
    auto add = [&](const std::vector<u8>& seg, u64 mem_off, u64 extra, platform::MemProtect prot) {
        if (seg.empty() && extra == 0) return;
        if (load_base + mem_off < load_base) return;  // overflow guarded below
        raw[count++] = RawSegment{mem_off, load_base + mem_off, seg.size(),
                                  seg.size() + extra, prot};
    };
    add(img.text, img.text_mem_off, 0, platform::MemProtect::ReadExec);
    add(img.rodata, img.rodata_mem_off, 0, platform::MemProtect::Read);
    add(img.data, img.data_mem_off, img.bss_size, platform::MemProtect::ReadWrite);

    std::string err;
    if (!LoadSegments(mm, sr::common::span<const u8>(image.data(), image.size()),
                      raw, static_cast<usize>(count), &err)) {
        return common::Error{"NSO: " + err};
    }

    LoadedImage out;
    out.entry_point = load_base + img.text_mem_off;
    for (int i = 0; i < count; ++i) {
        out.segments.push_back(LoadedSegment{raw[i].guest_address, raw[i].file_size,
                                             raw[i].memory_size, raw[i].protect});
    }
    return out;
}

common::Result<ModuleSystem> LoadModuleSystem(MemoryManager& mm,
                                              std::vector<ModuleInput> modules,
                                              u64 first_base) {
    if (modules.empty()) return common::Error{"module system: no modules"};

    // Parse every module up front and compute the layout (main, subsdk*, sdk).
    std::vector<NsoImage> images;
    images.reserve(modules.size());
    for (auto& m : modules) {
        auto parsed = ParseNso(sr::common::span<const u8>(m.bytes.data(), m.bytes.size()),
                               m.name);
        if (!parsed) return common::Error{parsed.error()};
        images.push_back(std::move(parsed.value()));
    }

    // Layout: bases in order, 2 MiB aligned, below 2^39.
    struct Placement {
        u64 base;
        u64 size;
    };
    std::vector<Placement> placement;
    placement.reserve(images.size());
    {
        u64 base = first_base;
        for (const NsoImage& img : images) {
            u64 image_size = 0;
            BuildMemoryImage(img, image_size);
            base = (base + MODULE_ALIGN - 1) & ~(MODULE_ALIGN - 1);
            if (base + image_size > ADDRESS_LIMIT) {
                return common::Error{"module system: layout exceeds 2^39"};
            }
            placement.push_back({base, image_size});
            base += image_size;
        }
    }

    // Map every segment ReadWrite first so relocations can patch text/rodata.
    std::vector<LoadedImage> loaded;
    loaded.resize(images.size());
    std::vector<u64> text_guest(images.size(), 0);
    std::vector<u64> text_size(images.size(), 0);
    std::vector<u64> rodata_guest(images.size(), 0);
    std::vector<u64> rodata_size(images.size(), 0);

    for (size_t m = 0; m < images.size(); ++m) {
        NsoImage& img = images[m];
        u64 image_size = 0;
        std::vector<u8> image = BuildMemoryImage(img, image_size);

        RawSegment raw[3];
        int count = 0;
        auto add = [&](const std::vector<u8>& seg, u64 mem_off, u64 extra) {
            if (seg.empty() && extra == 0) return;
            raw[count++] = RawSegment{mem_off, placement[m].base + mem_off,
                                      seg.size(), seg.size() + extra,
                                      platform::MemProtect::ReadWrite};
        };
        add(img.text, img.text_mem_off, 0);
        add(img.rodata, img.rodata_mem_off, 0);
        add(img.data, img.data_mem_off, img.bss_size);

        std::string err;
        if (!LoadSegments(mm, sr::common::span<const u8>(image.data(), image.size()),
                          raw, static_cast<usize>(count), &err)) {
            return common::Error{"module system: " + img.name + ": " + err};
        }

        text_guest[m] = placement[m].base + img.text_mem_off;
        text_size[m] = img.text.size();
        rodata_guest[m] = placement[m].base + img.rodata_mem_off;
        rodata_size[m] = img.rodata.size();

        loaded[m].entry_point = text_guest[m];
        for (int i = 0; i < count; ++i) {
            loaded[m].segments.push_back(LoadedSegment{raw[i].guest_address,
                                                       raw[i].file_size,
                                                       raw[i].memory_size,
                                                       raw[i].protect});
        }
    }

    // Global export map: symbol name -> absolute guest address.
    std::map<std::string, u64> exports;
    for (size_t m = 0; m < images.size(); ++m) {
        for (const NsoSymbol& sym : images[m].symbols) {
            if (sym.shndx != 0 && !sym.name.empty()) {
                exports[sym.name] = placement[m].base + sym.value;
            }
        }
    }

    // Resolve each module's relocations and write them into guest memory.
    auto write64 = [&](u64 guest, u64 value) -> bool {
        void* host = mm.GuestToHost(guest, 8, true);
        if (host == nullptr) return false;
        std::memcpy(host, &value, 8);
        return true;
    };

    for (size_t m = 0; m < images.size(); ++m) {
        const NsoImage& img = images[m];
        const u64 base = placement[m].base;
        for (const NsoRelocation& reloc : img.relocations) {
            const u64 target = base + reloc.offset;
            u64 value = 0;
            switch (reloc.type) {
                case R_AARCH64_RELATIVE:
                    value = base + static_cast<u64>(reloc.addend);
                    break;
                case R_AARCH64_ABS64:
                case R_AARCH64_GLOB_DAT:
                case R_AARCH64_JUMP_SLOT: {
                    const NsoSymbol* sym = nullptr;
                    if (reloc.sym < img.symbols.size()) {
                        sym = &img.symbols[reloc.sym];
                    }
                    u64 sym_addr = 0;
                    bool resolved = false;
                    if (sym != nullptr) {
                        if (sym->shndx != 0) {
                            sym_addr = base + sym->value;
                            resolved = true;
                        } else {
                            auto it = exports.find(sym->name);
                            if (it != exports.end()) {
                                sym_addr = it->second;
                                resolved = true;
                            }
                        }
                    }
                    if (!resolved) {
                        return common::Error{"module system: unresolved symbol in " + img.name};
                    }
                    value = sym_addr + (reloc.type == R_AARCH64_ABS64
                                            ? static_cast<u64>(reloc.addend) : 0);
                    break;
                }
                default:
                    // Unknown relocation types are left untouched in this
                    // reference implementation (they are recorded, not applied).
                    continue;
            }
            if (!write64(target, value)) {
                return common::Error{"module system: relocation write failed in " + img.name};
            }
        }
    }

    // Drop text to RX and rodata to R (data stays RW).
    for (size_t m = 0; m < images.size(); ++m) {
        const u64 page = static_cast<u64>(platform::PageSize());
        if (text_size[m] > 0) {
            const u64 aligned = text_guest[m] - (text_guest[m] % page);
            mm.ProtectGuestMemory(aligned, platform::MemProtect::ReadExec);
            // Record the final protection in the returned image.
            for (auto& seg : loaded[m].segments) {
                if (seg.guest_address == text_guest[m]) {
                    seg.protect = platform::MemProtect::ReadExec;
                }
            }
        }
        if (rodata_size[m] > 0) {
            const u64 aligned = rodata_guest[m] - (rodata_guest[m] % page);
            mm.ProtectGuestMemory(aligned, platform::MemProtect::Read);
            for (auto& seg : loaded[m].segments) {
                if (seg.guest_address == rodata_guest[m]) {
                    seg.protect = platform::MemProtect::Read;
                }
            }
        }
    }

    ModuleSystem out;
    out.first_base = first_base;
    for (size_t m = 0; m < images.size(); ++m) {
        out.modules.push_back(ModuleSystem::Placed{images[m].name, placement[m].base,
                                                   std::move(loaded[m])});
    }
    return out;
}

} // namespace sr::runtime
