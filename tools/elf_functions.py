#!/usr/bin/env python3
"""Function boundaries and normalized AArch64 bodies for a stripped ELF64.

Usage:
    python3 tools/elf_functions.py <libminecraftpe.so> <address> [...]

Prints the .eh_frame FDE range that contains each image-relative address.
The module is also imported by map_release_offsets.py and verify_targets.py.

Boundaries come from .eh_frame_hdr (sorted FDE initial locations) and each
FDE's pc_range, so no symbols are needed. Normalization masks PC-relative
immediates and the low-12-bit page offsets paired with ADRP, which change
whenever code or data moves between releases.
"""

from __future__ import annotations

import argparse
import bisect
import struct
from dataclasses import dataclass
from pathlib import Path

from elf_rtti import Elf64


@dataclass(frozen=True)
class Function:
    start: int
    size: int

    @property
    def end(self) -> int:
        return self.start + self.size


def _section(elf: Elf64, name: str):
    return next(item for item in elf.sections if item.name == name)


def _read_encoded(data: bytes, offset: int, encoding: int, base: int) -> tuple[int, int]:
    """Decode the DW_EH_PE formats Android lld emits in .eh_frame_hdr."""
    form = encoding & 0x0F
    if form == 0x0B:  # sdata4
        value = struct.unpack_from("<i", data, offset)[0]
        size = 4
    elif form == 0x03:  # udata4
        value = struct.unpack_from("<I", data, offset)[0]
        size = 4
    elif form == 0x0C:  # sdata8
        value = struct.unpack_from("<q", data, offset)[0]
        size = 8
    else:
        raise ValueError(f"unsupported pointer encoding 0x{encoding:02x}")
    application = encoding & 0x70
    if application in (0x10, 0x30):  # pcrel or datarel (both use base here)
        value += base
    elif application != 0:
        raise ValueError(f"unsupported pointer application 0x{encoding:02x}")
    return value, size


class FunctionTable:
    """Sorted FDE function ranges of one image."""

    def __init__(self, elf: Elf64):
        self.elf = elf
        header = _section(elf, ".eh_frame_hdr")
        data = elf.data
        offset = header.offset
        version, frame_encoding, count_encoding, table_encoding = data[offset : offset + 4]
        if version != 1:
            raise ValueError("unexpected .eh_frame_hdr version")
        cursor = offset + 4
        _, size = _read_encoded(data, cursor, frame_encoding, header.address + 4)
        cursor += size
        count, size = _read_encoded(data, cursor, count_encoding, 0)
        cursor += size
        if table_encoding != 0x3B:
            raise ValueError(f"unexpected table encoding 0x{table_encoding:02x}")
        raw = struct.unpack_from(f"<{count * 2}i", data, cursor)
        starts = []
        sizes = []
        for index in range(count):
            start = raw[index * 2] + header.address
            fde = raw[index * 2 + 1] + header.address
            fde_offset = elf.address_to_file_offset(fde)
            # length, CIE pointer, pcrel sdata4 pc_begin, udata4 pc_range.
            pc_begin = struct.unpack_from("<i", data, fde_offset + 8)[0] + fde + 8
            pc_range = struct.unpack_from("<I", data, fde_offset + 12)[0]
            if pc_begin != start:
                pc_range = 0
            starts.append(start)
            sizes.append(pc_range)
        self.starts = starts
        self.sizes = sizes

    def __len__(self) -> int:
        return len(self.starts)

    def index_containing(self, address: int) -> int | None:
        index = bisect.bisect_right(self.starts, address) - 1
        if index < 0:
            return None
        if self.starts[index] <= address < self.starts[index] + self.sizes[index]:
            return index
        return None

    def containing(self, address: int) -> Function | None:
        index = self.index_containing(address)
        if index is None:
            return None
        return Function(self.starts[index], self.sizes[index])

    def function(self, index: int) -> Function:
        return Function(self.starts[index], self.sizes[index])

    def index_of_start(self, address: int) -> int | None:
        index = bisect.bisect_left(self.starts, address)
        if index < len(self.starts) and self.starts[index] == address:
            return index
        return None


def words(elf: Elf64, address: int, size: int) -> tuple[int, ...]:
    offset = elf.address_to_file_offset(address)
    if offset is None or size <= 0:
        return ()
    count = size // 4
    return struct.unpack_from(f"<{count}I", elf.data, offset)


def normalize(sequence: tuple[int, ...]) -> tuple[int, ...]:
    """Mask relocation-sensitive immediates in an instruction sequence."""
    result = []
    page_registers: dict[int, int] = {}
    for position, value in enumerate(sequence):
        normalized = value
        if value & 0x7C000000 == 0x14000000:  # B / BL
            normalized = value & 0xFC000000
        elif value & 0x1F000000 == 0x10000000:  # ADR / ADRP
            normalized = value & 0x9F00001F
            if value & 0x80000000:
                page_registers[value & 0x1F] = position
        elif value & 0xFF000010 == 0x54000000:  # B.cond
            normalized = value & 0xFF00001F
        elif value & 0x7E000000 == 0x34000000:  # CBZ / CBNZ
            normalized = value & 0xFF00001F
        elif value & 0x7E000000 == 0x36000000:  # TBZ / TBNZ
            normalized = value & 0xFFF8001F
        elif value & 0x3B000000 == 0x18000000:  # literal load
            normalized = value & 0xFF00001F
        else:
            base = (value >> 5) & 0x1F
            recent = page_registers.get(base)
            if recent is not None and position - recent <= 16:
                # ADD (immediate) or LDR/STR (unsigned immediate) of an
                # ADRP page: the low twelve bits move with the data.
                if value & 0x7F800000 == 0x11000000 or value & 0x3B000000 == 0x39000000:
                    normalized = value & 0xFFC003FF
        result.append(normalized)
    return tuple(result)


def normalized_body(elf: Elf64, function: Function) -> tuple[int, ...]:
    return normalize(words(elf, function.start, function.size))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("library", type=Path)
    parser.add_argument("addresses", nargs="+", type=lambda value: int(value, 0))
    args = parser.parse_args()
    table = FunctionTable(Elf64(args.library.read_bytes()))
    print(f"functions={len(table)}")
    for address in args.addresses:
        function = table.containing(address)
        if function is None:
            print(f"0x{address:x}: no FDE")
        else:
            print(
                f"0x{address:x}: function 0x{function.start:x}+0x{function.size:x} "
                f"(offset +0x{address - function.start:x})"
            )


if __name__ == "__main__":
    main()
