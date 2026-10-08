#!/usr/bin/env python3
"""Generate minimal executable image fixtures for loader tests.

Produces:
  tests/fixtures/images/add.elf  - ELF-64 little-endian AArch64, one
                                   PT_LOAD (R+X) containing
                                   `add x0, x0, #1 ; ret`.
  tests/fixtures/images/add.nro  - Switch NRO ("NRO0"), seg0 R+X holding the
                                   same 8-byte program.
  tests/fixtures/images/threads.elf - ELF-64 little-endian AArch64, one
                                   PT_LOAD (R+X) holding a 4-instruction
                                   program that drives the thread HLE SVCs:
                                       SVC #0x0A  ; CreateThread
                                       SVC #0x0B  ; WaitSynchronization
                                       SVC #0x0C  ; GetResult
                                       ret
                                   The translator decodes SVC into
                                   `sr_rt_handle_svc(ctx, imm16)` and the
                                   runtime's HLE registry dispatches it.

Both add images place the code at file offset / guest vaddr 0x1000 so the
single text segment is page aligned; the loader maps one page and copies the
bytes.

The script is idempotent: running it again overwrites the fixtures with
identical bytes.
"""

import os
import struct
import sys

# add x0, x0, #1 -> 0x91000400 ; ret -> 0xD65F03C0 (little-endian).
CODE = bytes([0x00, 0x04, 0x00, 0x91, 0xC0, 0x03, 0x5F, 0xD6])

# SVC #imm16 = 0xD4000001 | (imm16 << 5). RET = 0xD65F03C0. Little-endian.
THREADS_CODE = bytes([
    0x41, 0x01, 0x00, 0xD4,  # SVC #0x0A  (CreateThread)
    0x61, 0x01, 0x00, 0xD4,  # SVC #0x0B  (WaitSynchronization)
    0x81, 0x01, 0x00, 0xD4,  # SVC #0x0C  (GetResult)
    0xC0, 0x03, 0x5F, 0xD6,  # ret
])

CODE_OFFSET = 0x1000
ENTRY = 0x1000
PAGE = 0x1000


def build_elf(code=CODE):
    # Elf64_Ehdr (64 bytes).
    e_ident = bytearray(16)
    e_ident[0:4] = b"\x7fELF"
    e_ident[4] = 2       # EI_CLASS = ELFCLASS64
    e_ident[5] = 1       # EI_DATA = ELFDATA2LSB
    e_ident[6] = 1       # EI_VERSION = EV_CURRENT
    e_ident[7] = 0       # EI_OSABI = ELFOSABI_NONE

    ehdr = struct.pack(
        "<16sHHIQQQIHHHHHH",
        bytes(e_ident),
        2,             # e_type  = ET_EXEC
        183,           # e_machine = EM_AARCH64
        1,             # e_version
        ENTRY,         # e_entry
        0x40,          # e_phoff
        0,             # e_shoff
        0,             # e_flags
        64,            # e_ehsize
        56,            # e_phentsize
        1,             # e_phnum
        0,             # e_shentsize
        0,             # e_shnum
        0,             # e_shstrndx
    )
    assert len(ehdr) == 64

    # Elf64_Phdr (56 bytes).
    phdr = struct.pack(
        "<IIQQQQQQ",
        1,             # p_type  = PT_LOAD
        5,             # p_flags = PF_R | PF_X
        CODE_OFFSET,   # p_offset
        ENTRY,         # p_vaddr
        ENTRY,         # p_paddr
        len(code),     # p_filesz
        len(code),     # p_memsz
        PAGE,          # p_align
    )
    assert len(phdr) == 56

    image = ehdr + phdr
    image += b"\x00" * (CODE_OFFSET - len(image))
    image += code
    return image


def build_nro():
    total_size = CODE_OFFSET + len(CODE)

    # 0x100-byte header; only the first 0x30 are described fields.
    header = bytearray(0x100)
    struct.pack_into("<I", header, 0x00, 0x304F524E)  # magic "NRO0"
    struct.pack_into("<I", header, 0x04, 0)          # version
    struct.pack_into("<I", header, 0x08, total_size) # total file size
    struct.pack_into("<I", header, 0x0C, 0)          # flags

    # Segment descriptors {file_offset, size}.
    struct.pack_into("<II", header, 0x10, CODE_OFFSET, len(CODE))  # seg0 text
    struct.pack_into("<II", header, 0x18, 0x108, 0)                # seg1 rodata
    struct.pack_into("<II", header, 0x20, 0x108, 0)                # seg2 data

    struct.pack_into("<I", header, 0x28, 0)  # bss_size
    struct.pack_into("<I", header, 0x2C, 0)  # reserved

    image = bytes(header)
    image += b"\x00" * (CODE_OFFSET - len(image))
    image += CODE
    assert len(image) == total_size
    return image


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)
    out_dir = os.path.join(project_root, "tests", "fixtures", "images")
    os.makedirs(out_dir, exist_ok=True)

    fixtures = {
        "add.elf": build_elf(),
        "add.nro": build_nro(),
        "threads.elf": build_elf(THREADS_CODE),
    }
    for name, data in fixtures.items():
        path = os.path.join(out_dir, name)
        with open(path, "wb") as f:
            f.write(data)
        print(f"wrote {path} ({len(data)} bytes)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
