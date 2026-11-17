#!/usr/bin/env python3
"""Find a masked byte pattern in the executable .text of a stripped ELF64.

Usage:
    python3 tools/find_pattern.py <libminecraftpe.so> "9f 36 02 71 ?? ?? ?? 54" \
        [--normalized] [--limit 20]

`??` matches any byte. With --normalized the pattern is given as
little-endian instruction words from the old image (eight hex digits each,
for example "7102369f 540002a8") and both sides are compared after
elf_functions.normalize(), so moved branch and ADRP targets still match.
Each hit prints its address and the containing .eh_frame function.
"""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path

from elf_functions import FunctionTable, normalize
from elf_rtti import Elf64


def byte_regex(pattern: str) -> re.Pattern[bytes]:
    parts = []
    for token in pattern.split():
        parts.append(b"." if token == "??" else re.escape(bytes([int(token, 16)])))
    return re.compile(b"".join(parts), re.DOTALL)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("library", type=Path)
    parser.add_argument("pattern")
    parser.add_argument("--normalized", action="store_true")
    parser.add_argument("--limit", type=int, default=20)
    args = parser.parse_args()

    elf = Elf64(args.library.read_bytes())
    text = next(s for s in elf.sections if s.name == ".text")
    raw = elf.data[text.offset : text.offset + text.size]
    hits: list[int] = []
    if args.normalized:
        needle = normalize(tuple(int(token, 16) for token in args.pattern.split()))
        # Anchor on the first word's opcode bits that normalization keeps.
        first = needle[0]
        count = len(raw) // 4
        values = struct.unpack_from(f"<{count}I", raw)
        for index in range(count - len(needle) + 1):
            if values[index] & first != first:
                continue
            window = normalize(values[index : index + len(needle)])
            if window == needle:
                hits.append(text.address + index * 4)
                if len(hits) >= args.limit:
                    break
    else:
        for match in byte_regex(args.pattern).finditer(raw):
            if match.start() % 4 == 0:
                hits.append(text.address + match.start())
                if len(hits) >= args.limit:
                    break
    functions = FunctionTable(elf)
    for address in hits:
        function = functions.containing(address)
        where = (
            f"function 0x{function.start:x}+0x{address - function.start:x}"
            if function
            else "no FDE"
        )
        print(f"0x{address:x} {where}")
    print(f"hits={len(hits)}{' (limit reached)' if len(hits) >= args.limit else ''}")


if __name__ == "__main__":
    main()
