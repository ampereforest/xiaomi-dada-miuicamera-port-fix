#!/usr/bin/env python3
"""Safely append a DT_NEEDED library to an AArch64 ELF64 shared object using a new PT_LOAD."""
import struct
import sys
from pathlib import Path

PT_LOAD = 1
PT_DYNAMIC = 2
PT_NOTE = 4
PT_GNU_PROPERTY = 0x6474e553

PF_R = 4

DT_NULL = 0
DT_NEEDED = 1
DT_STRTAB = 5
DT_STRSZ = 10


def align_up(v: int, a: int) -> int:
    return (v + a - 1) & ~(a - 1)


def add_needed(src: Path, dst: Path, libname: str) -> None:
    data = bytearray(src.read_bytes())

    # ELF64 header
    assert data[:4] == b"\x7fELF" and data[4] == 2 and data[5] == 1
    e_phoff, e_shoff = struct.unpack_from("<QQ", data, 32)
    e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHHHH", data, 54)
    assert e_phentsize == 56 and e_shentsize == 64

    # Parse sections
    sh_str_off = e_shoff + e_shstrndx * e_shentsize
    shstr_file_off, shstr_size = struct.unpack_from("<QQ", data, sh_str_off + 24)
    shstr = bytes(data[shstr_file_off:shstr_file_off + shstr_size])

    dynstr_idx = dynamic_idx = None
    dynstr_off = dynstr_sz = dynamic_off = dynamic_sz = 0
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name, _, _, _, sh_offset, sh_size = struct.unpack_from("<IIQQQQ", data, off)
        name = shstr[sh_name:shstr.find(b"\x00", sh_name)].decode(errors="ignore")
        if name == ".dynstr":
            dynstr_idx, dynstr_off, dynstr_sz = i, sh_offset, sh_size
        elif name == ".dynamic":
            dynamic_idx, dynamic_off, dynamic_sz = i, sh_offset, sh_size
    assert dynstr_idx is not None and dynamic_idx is not None

    # Parse program headers: find max PT_LOAD end and duplicate PT_NOTE (matching PT_GNU_PROPERTY)
    max_vaddr_end = 0
    dyn_phdr_idx = None
    gnu_prop_range = None
    note_phdr_idxs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, _, p_offset, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from("<IIQQQQQQ", data, off)
        if p_type == PT_LOAD:
            max_vaddr_end = max(max_vaddr_end, p_vaddr + p_memsz)
        elif p_type == PT_DYNAMIC:
            dyn_phdr_idx = i
        elif p_type == PT_GNU_PROPERTY:
            gnu_prop_range = (p_offset, p_filesz)
        elif p_type == PT_NOTE:
            note_phdr_idxs.append((i, p_offset, p_filesz))

    assert dyn_phdr_idx is not None
    # Pick the PT_NOTE that mirrors PT_GNU_PROPERTY (keep .note.android.ident + build-id intact)
    target_phdr_idx = None
    for idx, poff, psz in note_phdr_idxs:
        if gnu_prop_range == (poff, psz):
            target_phdr_idx = idx
            break
    assert target_phdr_idx is not None, f"Could not find redundant PT_NOTE in {src}"

    # Detach trailing section headers if they sit at EOF so our PT_LOAD is cleanly mapped
    sh_table_bytes = bytes(data[e_shoff:e_shoff + e_shnum * e_shentsize])
    if e_shoff + e_shnum * e_shentsize == len(data):
        del data[e_shoff:]

    new_off = align_up(max(len(data), max_vaddr_end), 0x1000)
    new_vaddr = new_off
    if len(data) < new_off:
        data.extend(b"\x00" * (new_off - len(data)))

    # Build expanded .dynstr
    old_dynstr = bytes(data[dynstr_off:dynstr_off + dynstr_sz])
    new_str_rel = len(old_dynstr)
    new_dynstr = old_dynstr + libname.encode("ascii") + b"\x00"
    new_dynstr_vaddr = new_vaddr
    new_dynstr_off = new_off
    data.extend(new_dynstr)
    while len(data) % 16 != 0:
        data.append(0)

    # Build expanded .dynamic with DT_NEEDED prepended before existing entries (or before DT_NULL)
    old_dyn = bytes(data[dynamic_off:dynamic_off + dynamic_sz])
    entries = []
    for pos in range(0, len(old_dyn), 16):
        tag, val = struct.unpack_from("<qQ", old_dyn, pos)
        if tag == DT_NULL:
            continue
        if tag == DT_STRTAB:
            val = new_dynstr_vaddr
        elif tag == DT_STRSZ:
            val = len(new_dynstr)
        entries.append((tag, val))

    # Insert new DT_NEEDED at the beginning of NEEDED list (or right before DT_NULL; prepending ensures DT_NEEDED runs first!)
    entries.insert(0, (DT_NEEDED, new_str_rel))
    entries.append((DT_NULL, 0))
    new_dyn_bytes = b"".join(struct.pack("<qQ", t, v) for t, v in entries)

    new_dyn_off = len(data)
    new_dyn_vaddr = new_vaddr + (new_dyn_off - new_off)
    data.extend(new_dyn_bytes)

    seg_filesz = len(data) - new_off
    seg_memsz = seg_filesz

    # Update target_phdr_idx -> PT_LOAD (PF_R)
    # Note: PT_LOAD headers should appear in ascending p_vaddr order for Bionic linker;
    # let's keep existing PHDRs in place and swap target_phdr_idx into position right after last PT_LOAD!
    phdrs = [bytearray(data[e_phoff + i * e_phentsize:e_phoff + (i + 1) * e_phentsize]) for i in range(e_phnum)]

    # Update PT_DYNAMIC
    struct.pack_into("<IIQQQQQQ", phdrs[dyn_phdr_idx], 0,
                     PT_DYNAMIC, 4, new_dyn_off, new_dyn_vaddr, new_dyn_vaddr,
                     len(new_dyn_bytes), len(new_dyn_bytes), 8)

    # Build new PT_LOAD phdr
    new_load = bytearray(56)
    struct.pack_into("<IIQQQQQQ", new_load, 0,
                     PT_LOAD, PF_R, new_off, new_vaddr, new_vaddr,
                     seg_filesz, seg_memsz, 0x1000)

    # Remove target_phdr_idx and insert new_load immediately after last PT_LOAD
    phdrs.pop(target_phdr_idx)
    last_load_idx = max(i for i, p in enumerate(phdrs) if struct.unpack_from("<I", p, 0)[0] == PT_LOAD)
    phdrs.insert(last_load_idx + 1, new_load)

    for i, p in enumerate(phdrs):
        data[e_phoff + i * e_phentsize:e_phoff + (i + 1) * e_phentsize] = p

    # Re-append section headers at EOF with updated .dynstr and .dynamic entries
    new_shoff = align_up(len(data), 8)
    if len(data) < new_shoff:
        data.extend(b"\x00" * (new_shoff - len(data)))

    sh_mut = bytearray(sh_table_bytes)
    # Update .dynstr sh_addr, sh_offset, sh_size
    struct.pack_into("<QQQ", sh_mut, dynstr_idx * e_shentsize + 16,
                     new_dynstr_vaddr, new_dynstr_off, len(new_dynstr))
    # Update .dynamic sh_addr, sh_offset, sh_size
    struct.pack_into("<QQQ", sh_mut, dynamic_idx * e_shentsize + 16,
                     new_dyn_vaddr, new_dyn_off, len(new_dyn_bytes))

    data.extend(sh_mut)
    struct.pack_into("<Q", data, 40, new_shoff)

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(data)
    print(f"Patched {src.name} -> {dst} (+DT_NEEDED {libname}, new PT_LOAD @ 0x{new_vaddr:x}, size=0x{seg_filesz:x})")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("Usage: patch_elf_needed.py <src.so> <dst.so> <libname.so>")
        sys.exit(1)
    add_needed(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3])
