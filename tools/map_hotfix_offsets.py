#!/usr/bin/env python3
"""Map target addresses between two closely related stripped AArch64 ELFs.

The mapper deliberately uses instruction neighborhoods rather than a global
delta. Bedrock hotfixes commonly add or remove a handful of functions, which
causes several independent address bands inside one .text section.
"""

from __future__ import annotations

import argparse
import re
import struct
from dataclasses import dataclass
from pathlib import Path

from elf_rtti import Elf64


HEX = re.compile(r"0x[0-9a-fA-F]+")


@dataclass(frozen=True)
class Mapping:
    old: int
    new: int
    anchor_delta: int
    anchor_size: int


def section(elf: Elf64, name: str):
    return next(item for item in elf.sections if item.name == name)


def image_bytes(elf: Elf64, address: int, size: int) -> bytes:
    offset = elf.address_to_file_offset(address)
    if offset is None:
        return b""
    return elf.data[offset : offset + size]


def normalized_instruction(value: int) -> int:
    # B / BL.
    if value & 0x7C000000 == 0x14000000:
        return value & 0xFC000000
    # ADR / ADRP.
    if value & 0x1F000000 == 0x10000000:
        return value & 0x9F00001F
    # B.cond.
    if value & 0xFF000010 == 0x54000000:
        return value & 0xFF00001F
    # CBZ / CBNZ.
    if value & 0x7E000000 == 0x34000000:
        return value & 0xFF00001F
    # TBZ / TBNZ.
    if value & 0x7E000000 == 0x36000000:
        return value & 0xFFF8001F
    # PC-relative literal load.
    if value & 0x3B000000 == 0x18000000:
        return value & 0xFF00001F
    return value


def normalized_words(data: bytes) -> tuple[int, ...]:
    return tuple(
        normalized_instruction(struct.unpack_from("<I", data, offset)[0])
        for offset in range(0, len(data) - 3, 4)
    )


def exact_anchor_mapping(
    old_elf: Elf64,
    new_elf: Elf64,
    address: int,
    radius: int,
) -> Mapping | None:
    for anchor_size in (64, 48, 32, 24, 16):
        for relative in range(-32, 257, 4):
            anchor = image_bytes(old_elf, address + relative, anchor_size)
            if len(anchor) != anchor_size:
                continue
            predicted = address - 0x100
            start_address = max(section(new_elf, ".text").address, predicted - radius)
            end_address = min(
                section(new_elf, ".text").address + section(new_elf, ".text").size,
                predicted + radius,
            )
            start = new_elf.address_to_file_offset(start_address)
            end = new_elf.address_to_file_offset(end_address)
            if start is None or end is None:
                continue
            matches = []
            cursor = start
            while True:
                found = new_elf.data.find(anchor, cursor, end)
                if found < 0:
                    break
                found_address = new_elf.file_offset_to_address(found)
                if found_address is not None and found_address % 4 == 0:
                    matches.append(found_address - relative)
                cursor = found + 1
            matches = sorted(set(matches))
            if len(matches) == 1:
                return Mapping(address, matches[0], relative, anchor_size)
    return None


def normalized_mapping(
    old_elf: Elf64,
    new_elf: Elf64,
    address: int,
    radius: int,
) -> tuple[Mapping | None, float]:
    relative = -16
    word_count = 32
    old_words = normalized_words(image_bytes(old_elf, address + relative, word_count * 4))
    if len(old_words) != word_count:
        return None, 0.0
    text = section(new_elf, ".text")
    predicted = address - 0x100
    begin = max(text.address, predicted - radius) & ~3
    end = min(text.address + text.size - word_count * 4, predicted + radius) & ~3
    best_score = -1
    best: list[int] = []
    for candidate in range(begin, end + 1, 4):
        words = normalized_words(image_bytes(new_elf, candidate + relative, word_count * 4))
        score = sum(left == right for left, right in zip(old_words, words))
        if score > best_score:
            best_score = score
            best = [candidate]
        elif score == best_score:
            best.append(candidate)
    confidence = best_score / word_count
    if len(best) != 1 or confidence < 0.75:
        return None, confidence
    return Mapping(address, best[0], relative, word_count * 4), confidence


def target_addresses(source: Path, old_elf: Elf64) -> list[int]:
    text = source.read_text()
    code = section(old_elf, ".text")
    return sorted(
        {
            int(token, 16)
            for token in HEX.findall(text)
            if code.address <= int(token, 16) < code.address + code.size
        }
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("old", type=Path, help="previous libminecraftpe.so")
    parser.add_argument("new", type=Path, help="new libminecraftpe.so")
    parser.add_argument(
        "source", type=Path, help="constants header containing previous-build targets"
    )
    parser.add_argument("--radius", type=lambda value: int(value, 0), default=0x20000)
    args = parser.parse_args()

    old_elf = Elf64(args.old.read_bytes())
    new_elf = Elf64(args.new.read_bytes())
    addresses = target_addresses(args.source, old_elf)
    unresolved = 0
    for address in addresses:
        mapping = exact_anchor_mapping(old_elf, new_elf, address, args.radius)
        method = "exact"
        confidence = 1.0
        if mapping is None:
            mapping, confidence = normalized_mapping(
                old_elf, new_elf, address, args.radius
            )
            method = "normalized"
        if mapping is None:
            unresolved += 1
            print(f"0x{address:09x} -> unresolved ({confidence:.0%})")
            continue
        print(
            f"0x{address:09x} -> 0x{mapping.new:09x} "
            f"delta={mapping.new - address:+#x} {method} "
            f"anchor={mapping.anchor_delta:+#x}/{mapping.anchor_size}"
        )
    print(f"mapped={len(addresses) - unresolved} unresolved={unresolved} total={len(addresses)}")
    if unresolved:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
