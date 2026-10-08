// Loader unit tests: ELF-64/AArch64 and Switch NRO loading, permissions,
// code bytes, BSS zeroing, and malformed-input rejection (no crashes).
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "switchrecomp/runtime/loader.h"
#include "switchrecomp/runtime/memory_manager.h"

#ifndef SR_TEST_FIXTURES_DIR
#error "SR_TEST_FIXTURES_DIR must be defined via CMake"
#endif

using sr::common::u16;
using sr::common::u32;
using sr::common::u64;
using sr::common::u8;
using sr::platform::MemProtect;
using sr::runtime::LoadElf;
using sr::runtime::LoadNro;
using sr::runtime::LoadedImage;
using sr::runtime::MappingInfo;
using sr::runtime::MemoryManager;

namespace {

// add x0, x0, #1 ; ret
const u8 kCode[8] = {0x00, 0x04, 0x00, 0x91, 0xC0, 0x03, 0x5F, 0xD6};

std::vector<u8> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>());
}

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

struct SegmentDef {
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 filesz;
    u64 memsz;
};

// Build a minimal ELF-64 LE AArch64 image from segment definitions. The file
// is sized to hold every segment's file bytes, which are filled with 0xAA.
std::vector<u8> MakeElf(const std::vector<SegmentDef>& defs,
                        u16 machine = 183, u32 version = 1) {
    size_t file_size = 0x40 + defs.size() * 0x38;
    for (const auto& d : defs) {
        file_size = std::max<size_t>(file_size,
                                     static_cast<size_t>(d.offset + d.filesz));
    }
    std::vector<u8> img(file_size, 0);

    img[0] = 0x7f; img[1] = 'E'; img[2] = 'L'; img[3] = 'F';
    img[4] = 2; // ELFCLASS64
    img[5] = 1; // ELFDATA2LSB
    img[6] = 1; // EV_CURRENT
    img[7] = 0; // ELFOSABI_NONE

    PutLE16(img, 16, 2);                 // e_type ET_EXEC
    PutLE16(img, 18, machine);
    PutLE32(img, 20, version);
    PutLE64(img, 24, defs.empty() ? 0 : defs[0].vaddr); // e_entry
    PutLE64(img, 32, 0x40);              // e_phoff
    PutLE16(img, 52, 64);                // e_ehsize
    PutLE16(img, 54, 56);                // e_phentsize
    PutLE16(img, 56, static_cast<u16>(defs.size())); // e_phnum

    for (size_t i = 0; i < defs.size(); ++i) {
        size_t ph = 0x40 + i * 0x38;
        const SegmentDef& d = defs[i];
        PutLE32(img, ph + 0, 1);          // PT_LOAD
        PutLE32(img, ph + 4, d.flags);
        PutLE64(img, ph + 8, d.offset);
        PutLE64(img, ph + 16, d.vaddr);
        PutLE64(img, ph + 24, d.vaddr);   // p_paddr
        PutLE64(img, ph + 32, d.filesz);
        PutLE64(img, ph + 40, d.memsz);
        PutLE64(img, ph + 48, 0x1000);    // p_align
        for (u64 b = 0; b < d.filesz; ++b) {
            img[d.offset + b] = 0xAA;
        }
    }
    return img;
}

} // namespace

// ---- ELF fixture ----

TEST(LoaderElf, LoadsWithCorrectEntryAndCode) {
    const std::vector<u8> data =
        ReadFile(std::string(SR_TEST_FIXTURES_DIR) + "/images/add.elf");
    ASSERT_FALSE(data.empty());

    MemoryManager mm;
    auto result = LoadElf(mm, sr::common::span<const u8>(data.data(), data.size()), 0);
    ASSERT_TRUE(result) << result.error().message;

    const LoadedImage& img = result.value();
    EXPECT_EQ(img.entry_point, 0x1000u);
    ASSERT_EQ(img.segments.size(), 1u);
    EXPECT_EQ(img.segments[0].guest_address, 0x1000u);
    EXPECT_EQ(img.segments[0].file_size, 8u);
    EXPECT_EQ(img.segments[0].memory_size, 8u);

    MappingInfo info;
    ASSERT_TRUE(mm.QueryMapping(0x1000, info));
    EXPECT_EQ(info.prot, MemProtect::ReadExec);

    void* host = mm.GuestToHost(0x1000, 8, false);
    ASSERT_NE(host, nullptr);
    EXPECT_EQ(std::memcmp(host, kCode, sizeof(kCode)), 0);
}

TEST(LoaderElf, AppliesLoadBase) {
    const std::vector<u8> data =
        ReadFile(std::string(SR_TEST_FIXTURES_DIR) + "/images/add.elf");
    MemoryManager mm;
    constexpr u64 kBase = 0x80000000ULL;
    auto result = LoadElf(mm, sr::common::span<const u8>(data.data(), data.size()), kBase);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().entry_point, kBase + 0x1000u);
    EXPECT_EQ(result.value().segments[0].guest_address, kBase + 0x1000u);
}

TEST(LoaderElf, BssIsZeroInitialized) {
    // One RW segment: 8 file bytes (0xAA) then BSS up to 0x100.
    std::vector<u8> data = MakeElf({{6 /*R+W*/, 0x1000, 0x1000, 8, 0x100}});
    MemoryManager mm;
    auto result = LoadElf(mm, sr::common::span<const u8>(data.data(), data.size()), 0);
    ASSERT_TRUE(result) << result.error().message;

    MappingInfo info;
    ASSERT_TRUE(mm.QueryMapping(0x1000, info));
    EXPECT_EQ(info.prot, MemProtect::ReadWrite);

    const u8* file_bytes =
        static_cast<const u8*>(mm.GuestToHost(0x1000, 8, false));
    ASSERT_NE(file_bytes, nullptr);
    for (int i = 0; i < 8; ++i) EXPECT_EQ(file_bytes[i], 0xAA);

    // BSS region [0x1008, 0x1100) must read as zeros.
    const u8* bss =
        static_cast<const u8*>(mm.GuestToHost(0x1008, 0xF8, false));
    ASSERT_NE(bss, nullptr);
    for (size_t i = 0; i < 0xF8; ++i) EXPECT_EQ(bss[i], 0) << i;
}

TEST(LoaderElf, NonPageAlignedSegmentIsHandled) {
    std::vector<u8> data =
        MakeElf({{5 /*R+X*/, 0x1004, 0x1004, 8, 8}});
    MemoryManager mm;
    auto result = LoadElf(mm, sr::common::span<const u8>(data.data(), data.size()), 0);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().entry_point, 0x1004u);

    MappingInfo info;
    ASSERT_TRUE(mm.QueryMapping(0x1004, info));
    EXPECT_EQ(info.prot, MemProtect::ReadExec);

    const u8* p =
        static_cast<const u8*>(mm.GuestToHost(0x1004, 8, false));
    ASSERT_NE(p, nullptr);
    for (int i = 0; i < 8; ++i) EXPECT_EQ(p[i], 0xAA);
}

// ---- NRO fixture ----

TEST(LoaderNro, LoadsWithCorrectEntryAndCode) {
    const std::vector<u8> data =
        ReadFile(std::string(SR_TEST_FIXTURES_DIR) + "/images/add.nro");
    ASSERT_FALSE(data.empty());

    MemoryManager mm;
    auto result = LoadNro(mm, sr::common::span<const u8>(data.data(), data.size()), 0);
    ASSERT_TRUE(result) << result.error().message;

    const LoadedImage& img = result.value();
    EXPECT_EQ(img.entry_point, 0x1000u);
    ASSERT_EQ(img.segments.size(), 1u);
    EXPECT_EQ(img.segments[0].guest_address, 0x1000u);
    EXPECT_EQ(img.segments[0].file_size, 8u);

    MappingInfo info;
    ASSERT_TRUE(mm.QueryMapping(0x1000, info));
    EXPECT_EQ(info.prot, MemProtect::ReadExec);

    void* host = mm.GuestToHost(0x1000, 8, false);
    ASSERT_NE(host, nullptr);
    EXPECT_EQ(std::memcmp(host, kCode, sizeof(kCode)), 0);
}

TEST(LoaderNro, AppliesLoadBase) {
    const std::vector<u8> data =
        ReadFile(std::string(SR_TEST_FIXTURES_DIR) + "/images/add.nro");
    MemoryManager mm;
    constexpr u64 kBase = 0x200000ULL;
    auto result = LoadNro(mm, sr::common::span<const u8>(data.data(), data.size()), kBase);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().entry_point, kBase + 0x1000u);
}

// ---- Error / malformed inputs ----

TEST(LoaderElf, RejectsBadMagic) {
    std::vector<u8> data = MakeElf({{5, 0x1000, 0x1000, 8, 8}});
    data[0] = 0x00;
    MemoryManager mm;
    auto result = LoadElf(mm, sr::common::span<const u8>(data.data(), data.size()), 0);
    EXPECT_FALSE(result);
}

TEST(LoaderElf, RejectsTruncatedHeader) {
    std::vector<u8> data(10, 0);
    MemoryManager mm;
    auto result = LoadElf(mm, sr::common::span<const u8>(data.data(), data.size()), 0);
    EXPECT_FALSE(result);
}

TEST(LoaderElf, RejectsNonAArch64Machine) {
    std::vector<u8> data = MakeElf({{5, 0x1000, 0x1000, 8, 8}}, 62 /*x86_64*/);
    MemoryManager mm;
    auto result = LoadElf(mm, sr::common::span<const u8>(data.data(), data.size()), 0);
    EXPECT_FALSE(result);
}

TEST(LoaderElf, RejectsBadVersion) {
    std::vector<u8> data = MakeElf({{5, 0x1000, 0x1000, 8, 8}}, 183, 99);
    MemoryManager mm;
    auto result = LoadElf(mm, sr::common::span<const u8>(data.data(), data.size()), 0);
    EXPECT_FALSE(result);
}

TEST(LoaderElf, RejectsTruncatedSegmentData) {
    // Segment claims 8 bytes at 0x1000 but the file is cut short.
    std::vector<u8> data = MakeElf({{5, 0x1000, 0x1000, 8, 8}});
    data.resize(0x1004);
    MemoryManager mm;
    auto result = LoadElf(mm, sr::common::span<const u8>(data.data(), data.size()), 0);
    EXPECT_FALSE(result);
}

TEST(LoaderNro, RejectsBadMagic) {
    const std::vector<u8> good =
        ReadFile(std::string(SR_TEST_FIXTURES_DIR) + "/images/add.nro");
    std::vector<u8> data = good;
    PutLE32(data, 0x00, 0xDEADBEEF);
    MemoryManager mm;
    auto result = LoadNro(mm, sr::common::span<const u8>(data.data(), data.size()), 0);
    EXPECT_FALSE(result);
}

TEST(LoaderNro, RejectsTruncatedHeader) {
    std::vector<u8> data(0x10, 0);
    MemoryManager mm;
    auto result = LoadNro(mm, sr::common::span<const u8>(data.data(), data.size()), 0);
    EXPECT_FALSE(result);
}

TEST(LoaderNro, RejectsBadVersion) {
    const std::vector<u8> good =
        ReadFile(std::string(SR_TEST_FIXTURES_DIR) + "/images/add.nro");
    std::vector<u8> data = good;
    PutLE32(data, 0x04, 7);
    MemoryManager mm;
    auto result = LoadNro(mm, sr::common::span<const u8>(data.data(), data.size()), 0);
    EXPECT_FALSE(result);
}
