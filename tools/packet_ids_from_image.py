#!/usr/bin/env python3
"""List the packet IDs and names the client image itself implements.

Usage:
    python3 tools/packet_ids_from_image.py <libminecraftpe.so> \
        [--docs MinecraftPacketIds.json] [--header src/network/packet_names.hpp]

Every concrete Packet vtable has getId() at slot 2 and getName() at slot 3.
On Android arm64 they compile to two fixed shapes:

    getId:   mov w0, #id ; ret
    getName: adrp x0, page ; add x0, x0, #off ; mov w1, #length ; ret

(getName returns a std::string_view in x0/x1.) The tool scans .data.rel.ro
for adjacent relocated slots with those shapes and prints `id name`.
--docs compares against Mojang's bedrock-protocol-docs MinecraftPacketIds.json
(names there omit the `Packet` suffix); --header compares against Dobby's
catalog. Differences are printed; the image is the ground truth for the
client build, the docs are the citation for names.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
from pathlib import Path

from elf_rtti import Elf64

RET = 0xD65F03C0


def code_words(elf: Elf64, address: int, count: int) -> tuple[int, ...] | None:
    offset = elf.address_to_file_offset(address)
    if offset is None or offset + count * 4 > len(elf.data):
        return None
    return struct.unpack_from(f"<{count}I", elf.data, offset)


def get_id(elf: Elf64, address: int) -> int | None:
    words = code_words(elf, address, 2)
    if words is None or words[1] != RET or words[0] & 0xFFE0001F != 0x52800000:
        return None
    return (words[0] >> 5) & 0xFFFF


def get_name(elf: Elf64, address: int) -> str | None:
    words = code_words(elf, address, 4)
    if words is None or words[3] != RET:
        return None
    adrp, add, mov = words[:3]
    if adrp & 0x9F00001F != 0x90000000:
        return None
    if add & 0xFFC003FF != 0x91000000:
        return None
    if mov & 0xFFE0001F != 0x52800001:
        return None
    immediate = ((adrp >> 29) & 0x3) | (((adrp >> 5) & 0x7FFFF) << 2)
    if immediate & (1 << 20):
        immediate -= 1 << 21
    page = (address & ~0xFFF) + (immediate << 12)
    target = page + ((add >> 10) & 0xFFF)
    length = (mov >> 5) & 0xFFFF
    offset = elf.address_to_file_offset(target)
    if offset is None:
        return None
    raw = elf.data[offset : offset + length]
    try:
        name = raw.decode("ascii")
    except UnicodeDecodeError:
        return None
    return name if re.fullmatch(r"[A-Za-z0-9_]+", name) else None


def image_packets(elf: Elf64) -> dict[int, set[str]]:
    relative = elf.relative_by_target
    found: dict[int, set[str]] = {}
    for slot, target in relative.items():
        packet_id = get_id(elf, target)
        if packet_id is None:
            continue
        name_target = relative.get(slot + 8)
        if name_target is None:
            continue
        name = get_name(elf, name_target)
        if name is None:
            continue
        found.setdefault(packet_id, set()).add(name)
    return found


def strip_suffix(name: str) -> str:
    return name[:-6] if name.endswith("Packet") else name


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("library", type=Path)
    parser.add_argument("--docs", type=Path)
    parser.add_argument("--header", type=Path)
    args = parser.parse_args()

    packets = image_packets(Elf64(args.library.read_bytes()))
    for packet_id in sorted(packets):
        print(f"{packet_id} {' | '.join(sorted(packets[packet_id]))}")
    print(f"image_ids={len(packets)} highest={max(packets)}")

    if args.docs:
        document = json.loads(args.docs.read_text())
        docs = dict(zip(document["x-enum-binary-value"], document["enum"]))
        print(f"docs protocol={document.get('x-protocol-version')} ids={len(docs)}")
        for packet_id in sorted(set(docs) | set(packets)):
            image_names = {strip_suffix(n) for n in packets.get(packet_id, set())}
            doc_name = docs.get(packet_id)
            if doc_name is None:
                print(f"docs-missing {packet_id} image={sorted(image_names)}")
            elif strip_suffix(doc_name) not in image_names:
                print(f"docs-differs {packet_id} docs={doc_name} image={sorted(image_names)}")
    if args.header:
        header = {
            int(i): n
            for i, n in re.findall(r'PacketNameEntry\{(\d+), "([^"]+)"\}', args.header.read_text())
        }
        for packet_id in sorted(set(header) | set(packets)):
            image_names = {strip_suffix(n) for n in packets.get(packet_id, set())}
            name = header.get(packet_id)
            if name is None:
                print(f"header-missing {packet_id} image={sorted(image_names)}")
            elif image_names and strip_suffix(name) not in image_names:
                print(f"header-differs {packet_id} header={name} image={sorted(image_names)}")


if __name__ == "__main__":
    main()
