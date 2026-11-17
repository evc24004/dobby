#!/usr/bin/env python3
"""Rebuild the DisconnectFailReason name/codeword table for a release.

Usage:
    python3 tools/disconnect_identities.py <libminecraftpe.so> \
        <assets>/definitions/disconnection_errors/disconnection_error_messaging.json \
        [--check src/diagnostics/disconnect_decoder.cpp]

Names: libminecraftpe stores the enum's names as one contiguous run of
NUL-terminated strings starting at "Unknown\\0CantConnectNoInternet". The run
ends at the first string that is not an identifier. Codewords: an Errors
entry's OverrideStrings, else its theme's DefaultStrings, else the file's
FallbackStrings. Without --check the tool prints C++ initializer rows; with
--check it compares them to the table in disconnect_decoder.cpp and exits 1 on
any difference.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
ROW = re.compile(r'\{"([^"]+)", "([^"]+)"\}')


def enum_names(library: bytes) -> list[str]:
    start = library.find(b"\0Unknown\0CantConnectNoInternet\0")
    if start < 0 or library.find(b"\0Unknown\0CantConnectNoInternet\0", start + 1) >= 0:
        raise SystemExit("DisconnectFailReason name run not found exactly once")
    names = []
    cursor = start + 1
    while True:
        end = library.index(b"\0", cursor)
        text = library[cursor:end].decode("latin-1")
        # The run is followed by RTTI names such as NSt6__ndk1...; those
        # contain digits after a capital N prefix and are not enum names.
        if not IDENTIFIER.match(text) or re.match(r"^N?S?t?\d", text) or text.startswith("NSt"):
            break
        names.append(text)
        cursor = end + 1
    return names


def codewords(messaging: dict) -> tuple[dict[str, str], str]:
    fallback = messaging["FallbackStrings"]["Codeword"]
    table: dict[str, str] = {}
    for theme in messaging.get("Themes", []):
        default = theme.get("DefaultStrings", {}).get("Codeword", fallback)
        for error in theme.get("Errors", []):
            override = error.get("OverrideStrings", {}).get("Codeword", default)
            table[error["FailReason"]] = override
    return table, fallback


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("library", type=Path)
    parser.add_argument("messaging", type=Path)
    parser.add_argument("--check", type=Path)
    args = parser.parse_args()

    names = enum_names(args.library.read_bytes())
    table, fallback = codewords(json.loads(args.messaging.read_text()))
    rows = [(name, table.get(name, fallback)) for name in names]
    print(f"names={len(rows)} unmapped_to_fallback={sum(1 for n in names if n not in table)}")
    if args.check is None:
        for name, codeword in rows:
            print(f'    {{"{name}", "{codeword}"}},')
        return
    current = ROW.findall(args.check.read_text())
    differences = 0
    for index in range(max(len(rows), len(current))):
        expected = rows[index] if index < len(rows) else None
        actual = current[index] if index < len(current) else None
        if expected != actual:
            differences += 1
            print(f"[{index}] source={actual} release={expected}")
    print(f"differences={differences}")
    raise SystemExit(1 if differences else 0)


if __name__ == "__main__":
    main()
