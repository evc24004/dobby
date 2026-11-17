#!/usr/bin/env python3
"""Locate Itanium C++ RTTI objects and vtables in a stripped ELF64 image."""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path


SHT_RELA = 4
R_AARCH64_RELATIVE = 1027


@dataclass(frozen=True)
class Section:
    name: str
    kind: int
    address: int
    offset: int
    size: int
    entry_size: int


@dataclass(frozen=True)
class Relocation:
    target: int
    info: int
    addend: int

    @property
    def kind(self) -> int:
        return self.info & 0xFFFFFFFF

    @property
    def symbol(self) -> int:
        return self.info >> 32


class Elf64:
    def __init__(self, data: bytes):
        if data[:6] != b"\x7fELF\x02\x01":
            raise ValueError("expected a little-endian ELF64 image")
        self.data = data
        self.sections = self._read_sections()
        self._relocations: list[Relocation] | None = None
        self._relative_by_target: dict[int, int] | None = None

    @property
    def relocations(self) -> list[Relocation]:
        # Parsed on first use: a full Bedrock image has about a million
        # entries, and boundary-only tools never need them.
        if self._relocations is None:
            self._relocations = self._read_relocations()
        return self._relocations

    @property
    def relative_by_target(self) -> dict[int, int]:
        if self._relative_by_target is None:
            self._relative_by_target = {
                relocation.target: relocation.addend
                for relocation in self.relocations
                if relocation.kind == R_AARCH64_RELATIVE
            }
        return self._relative_by_target

    def _read_sections(self) -> list[Section]:
        section_offset = struct.unpack_from("<Q", self.data, 0x28)[0]
        entry_size = struct.unpack_from("<H", self.data, 0x3A)[0]
        count = struct.unpack_from("<H", self.data, 0x3C)[0]
        names_index = struct.unpack_from("<H", self.data, 0x3E)[0]
        raw = []
        for index in range(count):
            offset = section_offset + index * entry_size
            raw.append(struct.unpack_from("<IIQQQQIIQQ", self.data, offset))
        names_offset = raw[names_index][4]
        names_size = raw[names_index][5]
        names = self.data[names_offset : names_offset + names_size]

        def section_name(index: int) -> str:
            end = names.find(b"\0", index)
            return names[index:end].decode("utf-8", "replace")

        return [
            Section(section_name(item[0]), item[1], item[3], item[4], item[5], item[9])
            for item in raw
        ]

    def _read_relocations(self) -> list[Relocation]:
        relocations = []
        for section in self.sections:
            if section.kind != SHT_RELA:
                continue
            entry_size = section.entry_size or 24
            for offset in range(section.offset, section.offset + section.size, entry_size):
                target, info, addend = struct.unpack_from("<QQq", self.data, offset)
                relocations.append(Relocation(target, info, addend))
        return relocations

    def file_offset_to_address(self, offset: int) -> int | None:
        for section in self.sections:
            if section.offset <= offset < section.offset + section.size:
                return section.address + offset - section.offset
        return None

    def address_to_file_offset(self, address: int) -> int | None:
        for section in self.sections:
            if section.address <= address < section.address + section.size:
                return section.offset + address - section.address
        return None

    def pointers_to(self, address: int) -> list[int]:
        return [
            relocation.target
            for relocation in self.relocations
            if relocation.kind == R_AARCH64_RELATIVE and relocation.addend == address
        ]

    def c_string(self, address: int) -> str:
        offset = self.address_to_file_offset(address)
        if offset is None:
            return ""
        end = self.data.find(b"\0", offset)
        return self.data[offset:end].decode("utf-8", "replace")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("library", type=Path)
    parser.add_argument("type_name", help="unmangled class name, for example PacketViolationWarningPacket")
    parser.add_argument("--slots", type=int, default=18)
    args = parser.parse_args()

    elf = Elf64(args.library.read_bytes())
    encoded_name = f"{len(args.type_name)}{args.type_name}".encode() + b"\0"
    search_at = 0
    name_addresses = []
    while True:
        found = elf.data.find(encoded_name, search_at)
        if found < 0:
            break
        address = elf.file_offset_to_address(found)
        if address is not None:
            name_addresses.append(address)
        search_at = found + 1

    if not name_addresses:
        raise SystemExit(f"RTTI name {encoded_name[:-1]!r} not found")

    print(f"type={args.type_name}")
    for name_address in name_addresses:
        print(f"rtti_name=0x{name_address:x}")
        name_references = elf.pointers_to(name_address)
        if not name_references:
            print("  no relative relocations reference the RTTI name")
            continue
        for name_reference in name_references:
            typeinfo = name_reference - 8
            print(f"  typeinfo=0x{typeinfo:x} (name field 0x{name_reference:x})")
            typeinfo_references = elf.pointers_to(typeinfo)
            if not typeinfo_references:
                print("    no relative relocations reference typeinfo")
                continue
            for typeinfo_reference in typeinfo_references:
                vtable = typeinfo_reference - 8
                print(f"    vtable=0x{vtable:x} address_point=0x{vtable + 16:x}")
                for slot in range(args.slots):
                    entry = vtable + 16 + slot * 8
                    target = elf.relative_by_target.get(entry)
                    if target is None:
                        print(f"      [{slot:02d}] entry=0x{entry:x} target=<non-relative>")
                    else:
                        print(f"      [{slot:02d}] entry=0x{entry:x} target=0x{target:x}")


if __name__ == "__main__":
    main()
