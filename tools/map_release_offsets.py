#!/usr/bin/env python3
"""Map Dobby targets from one Minecraft release image to the next.

Usage:
    python3 tools/map_release_offsets.py <old.so> <new.so> src/core/constants.hpp \
        [--only 0xADDR,...] [--window 0x400000]

Where map_hotfix_offsets.py assumes a small, nearly global address delta, a
minor release moves code by megabytes and reorders object files. This mapper
works per function instead:

1. Each code target is placed in its old .eh_frame function and that body is
   normalized (PC-relative and ADRP page-offset immediates masked).
2. A new function with an identical normalized body and size is an exact
   match. Several identical bodies are disambiguated by ordinal neighbours.
3. Otherwise the neighbours that do match exactly predict a new address, and
   functions near it are scored by instruction-sequence similarity.
4. Mid-function probes are re-found by their normalized bytes inside the new
   function; the offset from the function start is never assumed.
5. Vtable slots are mapped through the owning class: the slot's RTTI class
   name and index in the old image, then the same class and the new function
   pointer in the new image.

Every line states the method, so a reviewer can rerun or reject a mapping.
Nothing here is trusted at runtime: Dobby validates signatures and slots on
the device, and verify_targets.py checks the final constants statically.
"""

from __future__ import annotations

import argparse
import difflib
import re
import statistics
import struct
from collections import defaultdict
from pathlib import Path

from elf_functions import Function, FunctionTable, normalize, normalized_body, words
from elf_rtti import Elf64

HEX = re.compile(r"0x[0-9a-fA-F]+")
R_AARCH64_RELATIVE = 1027


class Image:
    def __init__(self, path: Path):
        self.elf = Elf64(path.read_bytes())
        self.functions = FunctionTable(self.elf)
        self.text = next(s for s in self.elf.sections if s.name == ".text")
        self.relro = next(s for s in self.elf.sections if s.name == ".data.rel.ro")
        self.pointers: dict[int, list[int]] = defaultdict(list)
        for target, addend in self.elf.relative_by_target.items():
            self.pointers[addend].append(target)
        self.relocated = {relocation.target for relocation in self.elf.relocations}
        self._by_size: dict[int, list[int]] | None = None
        self._bodies: dict[int, tuple[int, ...]] = {}

    def in_text(self, address: int) -> bool:
        return self.text.address <= address < self.text.address + self.text.size

    def in_relro(self, address: int) -> bool:
        return self.relro.address <= address < self.relro.address + self.relro.size

    def by_size(self) -> dict[int, list[int]]:
        if self._by_size is None:
            table: dict[int, list[int]] = defaultdict(list)
            for index, size in enumerate(self.functions.sizes):
                table[size].append(index)
            self._by_size = table
        return self._by_size

    def body(self, index: int) -> tuple[int, ...]:
        cached = self._bodies.get(index)
        if cached is None:
            cached = normalized_body(self.elf, self.functions.function(index))
            self._bodies[index] = cached
        return cached

    def rtti_name(self, typeinfo: int) -> str | None:
        name_pointer = self.elf.relative_by_target.get(typeinfo + 8)
        if name_pointer is None:
            return None
        name = self.elf.c_string(name_pointer)
        if not name or not (name[0].isdigit() or name[0] == "N"):
            return None
        return name

    def vtable_owner(self, slot: int) -> tuple[str, int, int] | None:
        """Return (mangled class, slot index, typeinfo entry address)."""
        cursor = slot
        for _ in range(4096):
            cursor -= 8
            typeinfo = self.elf.relative_by_target.get(cursor)
            if typeinfo is not None and self.in_relro(typeinfo):
                name = self.rtti_name(typeinfo)
                if name is not None:
                    return name, (slot - cursor - 8) // 8, cursor
            if cursor not in self.relocated:
                # offset-to-top word: the typeinfo entry must follow it.
                return None
        return None

    def vtables_of(self, name: str) -> list[int]:
        """Typeinfo-entry addresses of every vtable whose RTTI name matches."""
        encoded = name.encode() + b"\0"
        results = []
        cursor = 0
        while True:
            found = self.elf.data.find(encoded, cursor)
            if found < 0:
                break
            cursor = found + 1
            if found > 0 and self.elf.data[found - 1] != 0:
                continue
            address = self.elf.file_offset_to_address(found)
            for name_field in self.pointers.get(address, []):
                for entry in self.pointers.get(name_field - 8, []):
                    results.append(entry)
        return sorted(set(results))


def exact_candidates(old: Image, new: Image, index: int) -> list[int]:
    size = old.functions.sizes[index]
    body = old.body(index)
    return [candidate for candidate in new.by_size().get(size, []) if new.body(candidate) == body]


def neighbour_deltas(old: Image, new: Image, index: int, span: int = 48) -> list[int]:
    deltas = []
    for neighbour in range(max(0, index - span), min(len(old.functions), index + span + 1)):
        if neighbour == index or old.functions.sizes[neighbour] < 64:
            continue
        candidates = exact_candidates(old, new, neighbour)
        if len(candidates) == 1:
            deltas.append(new.functions.starts[candidates[0]] - old.functions.starts[neighbour])
    return deltas


def map_function(old: Image, new: Image, index: int, window: int) -> tuple[int | None, str]:
    candidates = exact_candidates(old, new, index)
    if len(candidates) == 1:
        return candidates[0], "exact-body"
    deltas = neighbour_deltas(old, new, index)
    if not deltas:
        return None, f"no anchored neighbours ({len(candidates)} exact bodies)"
    predicted_delta = int(statistics.median(deltas))
    predicted = old.functions.starts[index] + predicted_delta
    if candidates:
        nearest = sorted(
            candidates, key=lambda c: abs(new.functions.starts[c] - predicted)
        )
        first = abs(new.functions.starts[nearest[0]] - predicted)
        second = abs(new.functions.starts[nearest[1]] - predicted) if len(nearest) > 1 else None
        if second is None or first * 4 < second:
            return nearest[0], f"exact-body x{len(candidates)} nearest neighbour prediction"
        return None, f"ambiguous: {len(candidates)} exact bodies near prediction"
    old_body = old.body(index)
    old_size = len(old_body)
    scored = []
    starts = new.functions.starts
    import bisect

    begin = bisect.bisect_left(starts, predicted - window)
    end = bisect.bisect_right(starts, predicted + window)
    for candidate in range(begin, end):
        size = new.functions.sizes[candidate] // 4
        if size == 0 or abs(size - old_size) > max(4, old_size // 3):
            continue
        matcher = difflib.SequenceMatcher(None, old_body, new.body(candidate), autojunk=False)
        if matcher.real_quick_ratio() < 0.7 or matcher.quick_ratio() < 0.7:
            continue
        scored.append((matcher.ratio(), candidate))
    scored.sort(reverse=True)
    if not scored:
        return None, "no similar function near prediction"
    best_score, best = scored[0]
    runner_up = scored[1][0] if len(scored) > 1 else 0.0
    if best_score >= 0.8 and best_score - runner_up >= 0.05:
        return best, f"similar {best_score:.2f} (next {runner_up:.2f}, {len(deltas)} anchors)"
    return None, f"weak similarity {best_score:.2f} (next {runner_up:.2f})"


def map_probe(old: Image, new: Image, address: int, old_function: Function, new_function: Function) -> tuple[int | None, str]:
    relative = address - old_function.start
    if relative == 0:
        return new_function.start, "function entry"
    if old_function.size == new_function.size and normalized_body(old.elf, old_function) == normalized_body(new.elf, new_function):
        return new_function.start + relative, "same body offset"
    for length in (32, 24, 16, 12, 8):
        needle = normalize(words(old.elf, address, length))
        if len(needle) * 4 != length:
            continue
        hay = normalize(words(new.elf, new_function.start, new_function.size))
        hits = [
            position
            for position in range(0, len(hay) - len(needle) + 1)
            if hay[position : position + len(needle)] == needle
        ]
        if len(hits) == 1:
            return new_function.start + hits[0] * 4, f"probe bytes {length}B unique in function"
    return None, "probe bytes not unique in new function"


def map_slot(old: Image, new: Image, slot: int, function_map: dict[int, int]) -> tuple[int | None, str]:
    owner = old.vtable_owner(slot)
    if owner is None:
        return None, "old slot owner not found"
    name, index, _ = owner
    old_target = old.elf.relative_by_target.get(slot)
    new_target = function_map.get(old_target) if old_target is not None else None
    tables = new.vtables_of(name)
    results = []
    for entry in tables:
        candidate = entry + 8 + index * 8
        value = new.elf.relative_by_target.get(candidate)
        if new_target is None or value == new_target:
            results.append(candidate)
    if len(results) == 1:
        suffix = "" if new_target is not None else " (target unmapped)"
        return results[0], f"RTTI {name}[{index}]{suffix}"
    if new_target is not None:
        pointers = [p for p in new.pointers.get(new_target, []) if new.in_relro(p)]
        if len(pointers) == 1:
            return pointers[0], f"unique pointer to mapped target ({name}[{index}] moved)"
    return None, f"RTTI {name}[{index}]: {len(results)} candidates in {len(tables)} vtables"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("old", type=Path)
    parser.add_argument("new", type=Path)
    parser.add_argument("source", type=Path)
    parser.add_argument("--only", default="", help="comma-separated old addresses")
    parser.add_argument("--window", type=lambda value: int(value, 0), default=0x400000)
    args = parser.parse_args()

    old = Image(args.old)
    new = Image(args.new)
    tokens = sorted({int(t, 16) for t in HEX.findall(args.source.read_text())})
    if args.only:
        tokens = sorted({int(t, 0) for t in args.only.split(",")})
    code = [t for t in tokens if old.in_text(t)]
    data = [t for t in tokens if old.in_relro(t)]

    function_map: dict[int, int] = {}
    unresolved = 0
    for address in code:
        index = old.functions.index_containing(address)
        if index is None:
            print(f"code 0x{address:09x} -> unresolved (no old FDE)")
            unresolved += 1
            continue
        old_function = old.functions.function(index)
        new_index, method = map_function(old, new, index, args.window)
        if new_index is None:
            print(f"code 0x{address:09x} -> unresolved ({method})")
            unresolved += 1
            continue
        new_function = new.functions.function(new_index)
        function_map[old_function.start] = new_function.start
        mapped, probe_method = map_probe(old, new, address, old_function, new_function)
        if mapped is None:
            print(f"code 0x{address:09x} -> unresolved (function 0x{new_function.start:x}: {probe_method})")
            unresolved += 1
            continue
        print(f"code 0x{address:09x} -> 0x{mapped:09x} [{method}; {probe_method}]")

    for address in data:
        old_target = old.elf.relative_by_target.get(address)
        if old_target is not None and old.in_text(old_target):
            mapped, method = map_slot(old, new, address, function_map)
            label = "slot"
        else:
            owner = old.elf.relative_by_target.get(address - 8)
            name = old.rtti_name(owner) if owner is not None else None
            mapped, method = None, "not a vtable slot"
            label = "data"
            if name is not None:
                tables = new.vtables_of(name)
                if len(tables) == 1:
                    mapped, method = tables[0] + 8, f"vtable address point of {name}"
                else:
                    method = f"{len(tables)} vtables named {name}"
        if mapped is None:
            print(f"{label} 0x{address:09x} -> unresolved ({method})")
            unresolved += 1
        else:
            print(f"{label} 0x{address:09x} -> 0x{mapped:09x} [{method}]")
    print(f"mapped={len(code) + len(data) - unresolved} unresolved={unresolved} total={len(code) + len(data)}")
    raise SystemExit(1 if unresolved else 0)


if __name__ == "__main__":
    main()
