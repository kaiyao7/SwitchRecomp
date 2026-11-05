// NSO loader / module-system unit tests: NSO0 header parsing, LZ4 segment
// decompression, MOD0/dynamic/symbol/relocation parsing, single-module
// loading, and cross-module relocation resolution. All fixtures are synthetic
// and built in memory; no proprietary binaries are involved.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "switchrecomp/runtime/nso.h"

using sr::common::u8;
using sr::common::u16;
using sr::common::u32;
using sr::common::u64;
using sr::platform::MemProtect;
using sr::runtime::LoadModuleSystem;
using sr::runtime::LoadNso;
using sr::runtime::Lz4BlockDecompress;
using sr::runtime::MemoryManager;
using sr::runtime::ModuleInput;
using sr::runtime::ModuleSystem;
using sr::runtime::NsoImage;
using sr::runtime::NsoRelocation;
using sr::runtime::NsoSymbol;
using sr::runtime::ParseNso;

namespace {

void PutLE16(std::vector<u8>& v, size_t off, u16 x) {
    v[off + 0] = static_cast<u8>(x);
    v[off + 1] = static_cast<u8>(x >> 8);
}
void PutLE32(std::vector<u8>& v, size_t off, u32 x) {
    for (int i = 0; i < 4; ++i) v[off + i] = static_cast<u8>(x >> (i * 8));
}
void PutLE64(std::vector<u8>& v, size_t off, u64 x) {
    for (int i = 0; i < 8; ++i) v[off + i] = static_cast<u8>(x >> (i * 8));
}

// Spec for a synthetic NSO. Segments are stored uncompressed (flags == 0).
struct NsoSpec {
    std::vector<u8> text;
    std::vector<u8> rodata;
    std::vector<u8> data;
    u32 text_mem = 0;
    u32 rodata_mem = 0x1000;
    u32 data_mem = 0x2000;
    u32 bss_size = 0;
};

// Build a valid uncompressed NSO0 file from the spec. Segment bytes are placed
// in the file right after the 0x100-byte header, in text/rodata/data order.
std::vector<u8> MakeNso(const NsoSpec& spec) {
    constexpr size_t HEADER = 0x100;
    size_t text_off = HEADER;
    size_t rodata_off = text_off + spec.text.size();
    size_t data_off = rodata_off + spec.rodata.size();
    size_t file_size = data_off + spec.data.size();

    std::vector<u8> img(file_size, 0);
    PutLE32(img, 0x00, 0x304F534Eu);  // "NSO0"
    PutLE32(img, 0x0C, 0);            // flags: all uncompressed

    // Segment headers are 16 bytes apart: text at 0x10, ro at 0x20, data at
    // 0x30; ModuleNameOffset (0x1C) and ModuleNameSize (0x2C) sit in the gaps.
    PutLE32(img, 0x10, static_cast<u32>(text_off));
    PutLE32(img, 0x14, spec.text_mem);
    PutLE32(img, 0x18, static_cast<u32>(spec.text.size()));
    PutLE32(img, 0x20, static_cast<u32>(rodata_off));
    PutLE32(img, 0x24, spec.rodata_mem);
    PutLE32(img, 0x28, static_cast<u32>(spec.rodata.size()));
    PutLE32(img, 0x30, static_cast<u32>(data_off));
    PutLE32(img, 0x34, spec.data_mem);
    PutLE32(img, 0x38, static_cast<u32>(spec.data.size()));

    PutLE32(img, 0x3C, spec.bss_size);

    // uncompressed sizes == decompressed sizes
    PutLE32(img, 0x60, static_cast<u32>(spec.text.size()));
    PutLE32(img, 0x64, static_cast<u32>(spec.rodata.size()));
    PutLE32(img, 0x68, static_cast<u32>(spec.data.size()));

    if (!spec.text.empty())
        std::memcpy(img.data() + text_off, spec.text.data(), spec.text.size());
    if (!spec.rodata.empty())
        std::memcpy(img.data() + rodata_off, spec.rodata.data(), spec.rodata.size());
    if (!spec.data.empty())
        std::memcpy(img.data() + data_off, spec.data.data(), spec.data.size());
    return img;
}

// ---- a minimal text segment: MOD0 at offset 16, one DT_NULL dynamic entry ----
constexpr u32 kMod0Off = 16;
constexpr u32 kDynOff = 44;  // MOD0 + 28

std::vector<u8> MinimalText() {
    std::vector<u8> t(60, 0);
    PutLE32(t, 4, kMod0Off);
    std::memcpy(t.data() + kMod0Off, "MOD0", 4);
    PutLE32(t, kMod0Off + 4, 28);  // dynamic offset relative to MOD0
    // dynamic section at kDynOff: a single DT_NULL entry
    PutLE64(t, kDynOff, 0);
    PutLE64(t, kDynOff + 8, 0);
    return t;
}

} // namespace

// ---- LZ4 block decompression ----

TEST(NsoLz4, DecompressesLiteralRun) {
    // "rand16" compressed as an LZ4 raw block: token 0xF0 (15 literals + 1
    // extension byte), 16 literal bytes, no match.
    const u8 src[] = {0xF0, 0x01, 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23, 0x45,
                      0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFE, 0xDC, 0xBA, 0x98};
    const u8 expect[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23, 0x45, 0x67,
                         0x89, 0xAB, 0xCD, 0xEF, 0xFE, 0xDC, 0xBA, 0x98};
    u8 out[16];
    std::string err;
    ASSERT_TRUE(Lz4BlockDecompress(src, sizeof(src), out, sizeof(out), &err)) << err;
    EXPECT_EQ(std::memcmp(out, expect, sizeof(expect)), 0);
}

TEST(NsoLz4, DecompressesRleMatch) {
    // "A"*20 -> token 0x1A (1 literal, match len 15+? -> low nibble 0xF needs
    // an extension byte). Raw-block layout.
    const u8 src[] = {0x1A, 0x41, 0x01, 0x00, 0x50, 0x41, 0x41, 0x41, 0x41, 0x41};
    u8 out[20];
    std::string err;
    ASSERT_TRUE(Lz4BlockDecompress(src, sizeof(src), out, sizeof(out), &err)) << err;
    for (int i = 0; i < 20; ++i) EXPECT_EQ(out[i], 'A') << i;
}

TEST(NsoLz4, DecompressesRealVector) {
    // 60-byte text image (MOD0 + DT_NULL) compressed to 29 bytes.
    const u8 src[] = {0x50, 0x00, 0x00, 0x00, 0x00, 0x10, 0x05, 0x00, 0x03,
                      0x02, 0x00, 0x53, 0x4d, 0x4f, 0x44, 0x30, 0x1c, 0x0c,
                      0x00, 0x0f, 0x02, 0x00, 0x08, 0x50, 0x00, 0x00, 0x00,
                      0x00, 0x00};
    const std::vector<u8> expect = MinimalText();
    ASSERT_EQ(expect.size(), 60u);
    std::vector<u8> out(expect.size());
    std::string err;
    ASSERT_TRUE(Lz4BlockDecompress(src, sizeof(src), out.data(), out.size(), &err)) << err;
    EXPECT_EQ(out, expect);
}

TEST(NsoLz4, RejectsTruncation) {
    const u8 src[] = {0x1A, 0x41, 0x01};  // cut before match offset
    u8 out[20];
    std::string err;
    EXPECT_FALSE(Lz4BlockDecompress(src, sizeof(src), out, sizeof(out), &err));
    EXPECT_FALSE(err.empty());
}

// ---- NSO parsing ----

TEST(NsoParse, RejectsBadMagic) {
    auto img = MakeNso({MinimalText()});
    PutLE32(img, 0, 0xDEADBEEF);
    MemoryManager mm;
    auto r = ParseNso(sr::common::span<const u8>(img.data(), img.size()), "main");
    EXPECT_FALSE(r);
}

TEST(NsoParse, RejectsTruncatedHeader) {
    std::vector<u8> img(0x40, 0);
    auto r = ParseNso(sr::common::span<const u8>(img.data(), img.size()), "main");
    EXPECT_FALSE(r);
}

TEST(NsoParse, ParsesMinimalModule) {
    auto img = MakeNso({MinimalText()});
    auto r = ParseNso(sr::common::span<const u8>(img.data(), img.size()), "main");
    ASSERT_TRUE(r) << r.error().message;
    const NsoImage& parsed = r.value();
    EXPECT_EQ(parsed.name, "main");
    EXPECT_EQ(parsed.build_id.size(), 64u);
    EXPECT_EQ(parsed.text.size(), 60u);
    EXPECT_TRUE(parsed.rodata.empty());
    EXPECT_TRUE(parsed.data.empty());
    EXPECT_EQ(parsed.bss_size, 0u);
    EXPECT_TRUE(parsed.needed.empty());
    EXPECT_TRUE(parsed.symbols.empty());
    EXPECT_TRUE(parsed.relocations.empty());
}

TEST(NsoParse, DecompressesCompressedText) {
    // Same minimal text, but stored LZ4-compressed (flags bit 0 set, comp_size
    // 29, dec_size 60).
    const u8 comp[] = {0x50, 0x00, 0x00, 0x00, 0x00, 0x10, 0x05, 0x00, 0x03,
                       0x02, 0x00, 0x53, 0x4d, 0x4f, 0x44, 0x30, 0x1c, 0x0c,
                       0x00, 0x0f, 0x02, 0x00, 0x08, 0x50, 0x00, 0x00, 0x00,
                       0x00, 0x00};
    constexpr size_t HEADER = 0x100;
    std::vector<u8> img(HEADER + sizeof(comp), 0);
    PutLE32(img, 0x00, 0x304F534Eu);
    PutLE32(img, 0x0C, 1);  // text compressed
    PutLE32(img, 0x10, static_cast<u32>(HEADER));  // text file_off
    PutLE32(img, 0x14, 0);                         // text mem_off
    PutLE32(img, 0x18, 60);                        // text dec_size
    PutLE32(img, 0x60, sizeof(comp));              // text comp_size
    std::memcpy(img.data() + HEADER, comp, sizeof(comp));

    auto r = ParseNso(sr::common::span<const u8>(img.data(), img.size()), "main");
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r.value().text, MinimalText());
}

TEST(NsoParse, RejectsBadLz4) {
    constexpr size_t HEADER = 0x100;
    std::vector<u8> img(HEADER + 4, 0);
    PutLE32(img, 0x00, 0x304F534Eu);
    PutLE32(img, 0x0C, 1);
    PutLE32(img, 0x10, static_cast<u32>(HEADER));
    PutLE32(img, 0x14, 0);
    PutLE32(img, 0x18, 100);  // claims 100 bytes
    PutLE32(img, 0x60, 4);
    img[HEADER] = 0xFF; img[HEADER + 1] = 0xFF; img[HEADER + 2] = 0xFF; img[HEADER + 3] = 0xFF;

    auto r = ParseNso(sr::common::span<const u8>(img.data(), img.size()), "main");
    EXPECT_FALSE(r);
}

// ---- single-module loading ----

TEST(NsoLoad, MapsSegmentsAndEntry) {
    auto img = MakeNso({MinimalText()});
    MemoryManager mm;
    auto r = LoadNso(mm, sr::common::span<const u8>(img.data(), img.size()), 0, "main");
    ASSERT_TRUE(r) << r.error().message;

    EXPECT_EQ(r.value().entry_point, 0u);
    ASSERT_EQ(r.value().segments.size(), 1u);
    EXPECT_EQ(r.value().segments[0].guest_address, 0u);
    EXPECT_EQ(r.value().segments[0].protect, MemProtect::ReadExec);

    // MOD0 magic readable at guest offset 16.
    void* host = mm.GuestToHost(16, 4, false);
    ASSERT_NE(host, nullptr);
    EXPECT_EQ(std::memcmp(host, "MOD0", 4), 0);
}

TEST(NsoLoad, AppliesLoadBase) {
    auto img = MakeNso({MinimalText()});
    MemoryManager mm;
    constexpr u64 kBase = 0x7000000000ULL;
    auto r = LoadNso(mm, sr::common::span<const u8>(img.data(), img.size()), kBase, "main");
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r.value().entry_point, kBase);
}

TEST(NsoLoad, ZeroesBss) {
    NsoSpec spec;
    spec.text = MinimalText();
    spec.data = std::vector<u8>(4, 0xAB);
    spec.bss_size = 8;
    auto img = MakeNso(spec);
    MemoryManager mm;
    auto r = LoadNso(mm, sr::common::span<const u8>(img.data(), img.size()), 0, "main");
    ASSERT_TRUE(r) << r.error().message;

    // data at mem_off 0x2000, 4 bytes of 0xAB then 8 zeroed BSS bytes.
    const u8* d = static_cast<const u8*>(mm.GuestToHost(0x2000, 12, false));
    ASSERT_NE(d, nullptr);
    for (int i = 0; i < 4; ++i) EXPECT_EQ(d[i], 0xAB);
    for (int i = 4; i < 12; ++i) EXPECT_EQ(d[i], 0);
}

// ---- symbol table / relocation fixture ----

// Build a 0x400-byte text segment with a string table, a 2-entry symbol table,
// a SysV hash table, a dynamic section, and (when `with_reloc`) one DT_RELA
// relocation. The single named symbol is either defined (`defined` true:
// shndx=1, value 0x1000) or an undefined import (shndx=0). The relocation is
// R_AARCH64_ABS64 referencing that symbol, targeting a word at 0x1C0 whose
// offset is returned in `word_off_out`.
std::vector<u8> SymModuleText(const std::string& sym_name, bool defined,
                              bool with_reloc, u64& word_off_out) {
    std::vector<u8> t(0x400, 0);

    PutLE32(t, 4, kMod0Off);
    std::memcpy(t.data() + kMod0Off, "MOD0", 4);
    PutLE32(t, kMod0Off + 4, 28);  // dynamic section offset relative to MOD0

    const u64 dyn_off = kDynOff;   // 44
    const u64 strtab = 0xB0;
    const u64 symtab = 0x100;
    const u64 hash = 0x150;
    const u64 rela = 0x180;
    const u64 word_off = 0x1C0;

    // string table: "\0" at offset 0, then the symbol name (NUL-terminated).
    t[strtab] = 0;
    std::memcpy(t.data() + strtab + 1, sym_name.c_str(), sym_name.size() + 1);

    // symbol table: [0] null, [1] the named symbol.
    auto sym = [&](u32 idx, u32 name_off, u8 info, u16 shndx, u64 value) {
        size_t o = symtab + static_cast<size_t>(idx) * 24;
        PutLE32(t, o, name_off);
        t[o + 4] = info;
        PutLE16(t, o + 6, shndx);
        PutLE64(t, o + 8, value);
        PutLE64(t, o + 16, 0);
    };
    sym(0, 0, 0, 0, 0);
    sym(1, 1, 0x12, defined ? 1 : 0, defined ? 0x1000 : 0);

    // SysV hash: nbucket=1, nchain=2, buckets[1], chains[2].
    PutLE32(t, hash + 0, 1);
    PutLE32(t, hash + 4, 2);
    PutLE32(t, hash + 8, 0);
    PutLE32(t, hash + 12, 0);
    PutLE32(t, hash + 16, 0);

    // dynamic entries
    u64 o = dyn_off;
    auto dyn = [&](u64 tag, u64 val) { PutLE64(t, o, tag); PutLE64(t, o + 8, val); o += 16; };
    dyn(5, strtab);   // DT_STRTAB
    dyn(6, symtab);   // DT_SYMTAB
    dyn(4, hash);     // DT_HASH
    if (with_reloc) {
        dyn(7, rela);   // DT_RELA
        dyn(8, 24);     // DT_RELASZ (one relocation)
    }
    dyn(0, 0);        // DT_NULL

    if (with_reloc) {
        // R_AARCH64_ABS64 (257), symbol 1, addend 0.
        PutLE64(t, rela + 0, word_off);
        PutLE64(t, rela + 8, 257ull | (1ull << 32));
        PutLE64(t, rela + 16, 0);
        PutLE64(t, word_off, 0xDEADBEEFDEADBEEFull);  // placeholder, overwritten
    }

    word_off_out = word_off;
    return t;
}

TEST(NsoParse, ParsesSymbolsAndRelocations) {
    u64 word_off;
    auto text = SymModuleText("shared_fn", /*defined=*/true, /*with_reloc=*/true, word_off);
    auto img = MakeNso({text});
    auto r = ParseNso(sr::common::span<const u8>(img.data(), img.size()), "main");
    ASSERT_TRUE(r) << r.error().message;

    const NsoImage& p = r.value();
    ASSERT_EQ(p.symbols.size(), 2u);
    EXPECT_EQ(p.symbols[0].name, "");
    EXPECT_EQ(p.symbols[1].name, "shared_fn");
    EXPECT_EQ(p.symbols[1].shndx, 1u);
    EXPECT_EQ(p.symbols[1].value, 0x1000u);

    ASSERT_EQ(p.relocations.size(), 1u);
    EXPECT_EQ(p.relocations[0].offset, word_off);
    EXPECT_EQ(p.relocations[0].type, 257u);
    EXPECT_EQ(p.relocations[0].sym, 1u);
}

// ---- cross-module relocation resolution ----

TEST(NsoModuleSystem, ResolvesCrossModuleImport) {
    // main: defines "shared_fn" at value 0x1000, no relocation.
    u64 a_word;
    auto main_text = SymModuleText("shared_fn", /*defined=*/true, /*with_reloc=*/false, a_word);
    auto main_img = MakeNso({main_text});

    // subsdk0: imports "shared_fn" with an R_AARCH64_ABS64 relocation.
    u64 b_word;
    auto sub_text = SymModuleText("shared_fn", /*defined=*/false, /*with_reloc=*/true, b_word);
    auto sub_img = MakeNso({sub_text});

    MemoryManager mm;
    std::vector<ModuleInput> modules;
    modules.push_back(ModuleInput{"main", std::move(main_img)});
    modules.push_back(ModuleInput{"subsdk0", std::move(sub_img)});

    auto r = LoadModuleSystem(mm, std::move(modules), 0x7000000000ULL);
    ASSERT_TRUE(r) << r.error().message;

    const ModuleSystem& sys = r.value();
    ASSERT_EQ(sys.modules.size(), 2u);
    EXPECT_EQ(sys.modules[0].name, "main");
    EXPECT_EQ(sys.modules[1].name, "subsdk0");
    // First module keeps the requested base; the next is 2 MiB aligned above it.
    EXPECT_EQ(sys.modules[0].base, 0x7000000000ULL);
    EXPECT_GT(sys.modules[1].base, sys.modules[0].base);
    EXPECT_EQ(sys.modules[1].base % 0x200000ULL, 0u);

    // subsdk0's import relocation must hold main's export absolute address.
    const u64 expected = sys.modules[0].base + 0x1000;
    const u64 target_guest = sys.modules[1].base + b_word;
    const u64* word = static_cast<const u64*>(mm.GuestToHost(target_guest, 8, false));
    ASSERT_NE(word, nullptr);
    EXPECT_EQ(*word, expected);
}

TEST(NsoModuleSystem, RejectsUnresolvedSymbol) {
    // A single module importing "shared_fn" with no exporter anywhere.
    u64 word_off;
    auto sub_text = SymModuleText("shared_fn", /*defined=*/false, /*with_reloc=*/true, word_off);
    auto sub_img = MakeNso({sub_text});

    MemoryManager mm;
    std::vector<ModuleInput> modules;
    modules.push_back(ModuleInput{"subsdk0", std::move(sub_img)});

    auto r = LoadModuleSystem(mm, std::move(modules), 0x7000000000ULL);
    EXPECT_FALSE(r);
}

} // namespace
