#!/usr/bin/env python3
"""Diff one function between two stripped AArch64 images.

Usage:
    python3 tools/diff_functions.py <old.so> <old-address> <new.so> <new-address> \
        [--objdump llvm-objdump-19] [--context 3] [--raw]

Each address may be any instruction inside the function; the .eh_frame range
selects the whole body. Disassembly comes from llvm-objdump; addresses, PC-
relative targets and symbolized comments are stripped (unless --raw) so the
unified diff shows only real changes: frame sizes, field offsets, argument
registers and inserted calls. Use it to confirm that a re-found hook target
keeps the ABI and object layout the detour depends on. The output contains
disassembly of the game image: print it locally, never commit it.
"""

from __future__ import annotations

import argparse
import difflib
import re
import shutil
import subprocess
from pathlib import Path

from elf_functions import FunctionTable
from elf_rtti import Elf64

ADDRESS = re.compile(r"^\s*[0-9a-f]+:\s*")
COMMENT = re.compile(r"\s*(<[^>]*>|//.*)$")
BRANCH_TARGET = re.compile(r"\b0x[0-9a-f]+\b")


def disassemble(objdump: str, library: Path, start: int, end: int, raw: bool) -> list[str]:
    output = subprocess.run(
        [objdump, "-d", "--no-show-raw-insn", f"--start-address=0x{start:x}",
         f"--stop-address=0x{end:x}", str(library)],
        check=True, capture_output=True, text=True,
    ).stdout
    lines = []
    for line in output.splitlines():
        if not ADDRESS.match(line):
            continue
        body = ADDRESS.sub("", line).strip()
        body = COMMENT.sub("", body)
        if not raw and re.match(r"^(b|bl|b\.\w+|cbn?z|tbn?z|adrp|adr)\b", body):
            body = BRANCH_TARGET.sub("<pc>", body)
        lines.append(body)
    return lines


def function_range(library: Path, address: int) -> tuple[int, int]:
    function = FunctionTable(Elf64(library.read_bytes())).containing(address)
    if function is None:
        raise SystemExit(f"0x{address:x}: no FDE in {library}")
    return function.start, function.end


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("old", type=Path)
    parser.add_argument("old_address", type=lambda value: int(value, 0))
    parser.add_argument("new", type=Path)
    parser.add_argument("new_address", type=lambda value: int(value, 0))
    parser.add_argument("--objdump", default=shutil.which("llvm-objdump-19") or "llvm-objdump")
    parser.add_argument("--context", type=int, default=3)
    parser.add_argument("--raw", action="store_true")
    args = parser.parse_args()

    old_start, old_end = function_range(args.old, args.old_address)
    new_start, new_end = function_range(args.new, args.new_address)
    old_lines = disassemble(args.objdump, args.old, old_start, old_end, args.raw)
    new_lines = disassemble(args.objdump, args.new, new_start, new_end, args.raw)
    ratio = difflib.SequenceMatcher(None, old_lines, new_lines, autojunk=False).ratio()
    print(f"old 0x{old_start:x}+0x{old_end - old_start:x} new 0x{new_start:x}+0x{new_end - new_start:x} similarity={ratio:.3f}")
    for line in difflib.unified_diff(
        old_lines, new_lines, "old", "new", n=args.context, lineterm=""
    ):
        print(line)


if __name__ == "__main__":
    main()
