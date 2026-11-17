#!/usr/bin/env python3
"""Compare a class's vtables slot by slot between two release images.

Usage:
    python3 tools/compare_vtables.py <old.so> <new.so> ClassName [More ...] [--slots 40]

ClassName is the unmangled leaf name (PacketViolationWarningPacket) or an
Itanium RTTI name (N15RakNetConnector17RakNetNetworkPeerE). For every vtable
of that class the tool prints, per slot, the old and new function and whether
their normalized bodies are identical (`same`), equal in size only (`size`),
or different. An unchanged slot layout plus identical bodies is the evidence
that a vtable slot target and any object offsets it reads still hold.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from map_release_offsets import Image


def rtti_name(name: str) -> str:
    if name[0].isdigit() or name.startswith("N"):
        return name
    return f"{len(name)}{name}"


def slot_target(image: Image, entry: int) -> int | None:
    return image.elf.relative_by_target.get(entry)


def describe(image: Image, address: int | None) -> tuple[str, tuple[int, ...] | None]:
    if address is None:
        return "<none>", None
    function = image.functions.containing(address)
    if function is None or function.start != address:
        return f"0x{address:x}?", None
    index = image.functions.index_of_start(address)
    return f"0x{address:x}+0x{function.size:x}", image.body(index)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("old", type=Path)
    parser.add_argument("new", type=Path)
    parser.add_argument("classes", nargs="+")
    parser.add_argument("--slots", type=int, default=40)
    args = parser.parse_args()
    old = Image(args.old)
    new = Image(args.new)
    for name in args.classes:
        mangled = rtti_name(name)
        old_tables = old.vtables_of(mangled)
        new_tables = new.vtables_of(mangled)
        print(f"class {mangled}: old vtables={len(old_tables)} new vtables={len(new_tables)}")
        for number, (old_entry, new_entry) in enumerate(zip(old_tables, new_tables)):
            print(f"  vtable {number}: old address point 0x{old_entry + 8:x} new 0x{new_entry + 8:x}")
            for slot in range(args.slots):
                old_slot = old_entry + 8 + slot * 8
                new_slot = new_entry + 8 + slot * 8
                old_target = slot_target(old, old_slot)
                new_target = slot_target(new, new_slot)
                if old_target is None and new_target is None:
                    break
                old_text, old_body = describe(old, old_target)
                new_text, new_body = describe(new, new_target)
                if old_body is not None and old_body == new_body:
                    verdict = "same"
                elif old_body is not None and new_body is not None and len(old_body) == len(new_body):
                    verdict = "size"
                else:
                    verdict = "diff"
                print(
                    f"    [{slot:02d}] old 0x{old_slot:x}->{old_text}  "
                    f"new 0x{new_slot:x}->{new_text}  {verdict}"
                )


if __name__ == "__main__":
    main()
