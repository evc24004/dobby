#!/usr/bin/env python3
"""Validate and install local persona cape packs for Dobby testing."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import struct
import sys
import tempfile
import time
import uuid
import zipfile
import zlib


MAX_ARCHIVES = 64
MAX_FILES_PER_ARCHIVE = 64
MAX_FILE_BYTES = 8 * 1024 * 1024
MAX_ARCHIVE_BYTES = 32 * 1024 * 1024
MAX_ARCHIVE_FILE_BYTES = 64 * 1024 * 1024
MAX_JSON_BYTES = 64 * 1024
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class CapePackError(ValueError):
    pass


def require_uuid(value: object, label: str) -> str:
    if not isinstance(value, str):
        raise CapePackError(f"{label} must be a UUID string")
    try:
        parsed = uuid.UUID(value)
    except ValueError as error:
        raise CapePackError(f"{label} is not a valid UUID") from error
    return str(parsed)


def safe_member_name(name: str) -> PurePosixPath:
    path = PurePosixPath(name)
    if not name or path.is_absolute() or ".." in path.parts or "\\" in name:
        raise CapePackError(f"unsafe archive path: {name!r}")
    return path


def parse_json(
    data: bytes, label: str, maximum_bytes: int = MAX_JSON_BYTES
) -> object:
    if len(data) > maximum_bytes:
        raise CapePackError(f"{label} exceeds {maximum_bytes} bytes")
    try:
        return json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CapePackError(f"{label} is not valid UTF-8 JSON") from error


def decode_png_rgba(data: bytes, label: str) -> bytes:
    if (
        len(data) < 33
        or data[:8] != PNG_SIGNATURE
        or data[8:12] != b"\x00\x00\x00\r"
        or data[12:16] != b"IHDR"
    ):
        raise CapePackError(f"{label} is not a PNG with an IHDR chunk")
    width, height, bit_depth, color_type, compression, filtering, interlace = (
        struct.unpack(">IIBBBBB", data[16:29])
    )
    if (width, height) != (64, 32):
        raise CapePackError(
            f"{label} is {width}x{height}; cape textures must be 64x32"
        )
    if (bit_depth, color_type, compression, filtering, interlace) != (8, 6, 0, 0, 0):
        raise CapePackError(
            f"{label} must be non-interlaced 8-bit RGBA PNG data"
        )

    idat = bytearray()
    position = 8
    saw_ihdr = False
    saw_iend = False
    while position + 12 <= len(data):
        length = struct.unpack(">I", data[position : position + 4])[0]
        chunk_end = position + 12 + length
        if chunk_end > len(data):
            raise CapePackError(f"{label} contains a truncated PNG chunk")
        chunk_type = data[position + 4 : position + 8]
        chunk_data = data[position + 8 : position + 8 + length]
        expected_crc = struct.unpack(">I", data[position + 8 + length : chunk_end])[0]
        if zlib.crc32(chunk_type + chunk_data) & 0xFFFFFFFF != expected_crc:
            raise CapePackError(f"{label} contains a PNG CRC mismatch")
        if chunk_type == b"IHDR":
            if saw_ihdr or position != 8:
                raise CapePackError(f"{label} contains an invalid IHDR position")
            saw_ihdr = True
        elif chunk_type == b"IDAT":
            idat.extend(chunk_data)
        elif chunk_type == b"IEND":
            saw_iend = True
            break
        position = chunk_end
    if not saw_ihdr or not saw_iend or not idat:
        raise CapePackError(f"{label} is missing required PNG chunks")

    try:
        expected_filtered_bytes = height * (width * 4 + 1)
        decoder = zlib.decompressobj()
        filtered = decoder.decompress(bytes(idat), expected_filtered_bytes + 1)
    except zlib.error as error:
        raise CapePackError(f"{label} contains invalid compressed pixels") from error
    stride = width * 4
    if (
        len(filtered) != expected_filtered_bytes
        or not decoder.eof
        or decoder.unconsumed_tail
        or decoder.unused_data
    ):
        raise CapePackError(f"{label} has an unexpected decoded byte count")

    decoded = bytearray(height * stride)
    previous = bytearray(stride)
    for row in range(height):
        source = filtered[row * (stride + 1) : (row + 1) * (stride + 1)]
        filter_type = source[0]
        current = bytearray(source[1:])
        for index in range(stride):
            left = current[index - 4] if index >= 4 else 0
            above = previous[index]
            upper_left = previous[index - 4] if index >= 4 else 0
            if filter_type == 1:
                current[index] = (current[index] + left) & 0xFF
            elif filter_type == 2:
                current[index] = (current[index] + above) & 0xFF
            elif filter_type == 3:
                current[index] = (current[index] + ((left + above) // 2)) & 0xFF
            elif filter_type == 4:
                predictor = left + above - upper_left
                left_distance = abs(predictor - left)
                above_distance = abs(predictor - above)
                upper_left_distance = abs(predictor - upper_left)
                paeth = (
                    left
                    if left_distance <= above_distance
                    and left_distance <= upper_left_distance
                    else above
                    if above_distance <= upper_left_distance
                    else upper_left
                )
                current[index] = (current[index] + paeth) & 0xFF
            elif filter_type != 0:
                raise CapePackError(f"{label} uses an unsupported PNG row filter")
        decoded[row * stride : (row + 1) * stride] = current
        previous = current
    return bytes(decoded)


def load_archive(archive: Path) -> dict[str, object]:
    if archive.stat().st_size > MAX_ARCHIVE_FILE_BYTES:
        raise CapePackError("archive file is too large")
    try:
        with zipfile.ZipFile(archive) as cape_zip:
            infos = cape_zip.infolist()
            if len(infos) > MAX_FILES_PER_ARCHIVE:
                raise CapePackError("archive contains too many entries")
            total_bytes = 0
            files: dict[str, bytes] = {}
            for info in infos:
                path = safe_member_name(info.filename)
                if info.is_dir():
                    continue
                if (info.external_attr >> 16) & 0o170000 == 0o120000:
                    raise CapePackError(f"symbolic link is not allowed: {info.filename}")
                if info.file_size > MAX_FILE_BYTES:
                    raise CapePackError(f"archive member is too large: {info.filename}")
                total_bytes += info.file_size
                if total_bytes > MAX_ARCHIVE_BYTES:
                    raise CapePackError("expanded archive is too large")
                normalized = str(path)
                if normalized in files:
                    raise CapePackError(f"duplicate archive path: {normalized}")
                files[normalized] = cape_zip.read(info)
    except zipfile.BadZipFile as error:
        raise CapePackError("file is not a valid ZIP archive") from error

    required = {"manifest.json", "contents.json", "signatures.json"}
    missing = required - files.keys()
    if missing:
        raise CapePackError(f"missing required files: {', '.join(sorted(missing))}")

    manifest = parse_json(files["manifest.json"], "manifest.json")
    if not isinstance(manifest, dict) or manifest.get("format_version") != 1:
        raise CapePackError("manifest.json must use format_version 1")
    header = manifest.get("header")
    modules = manifest.get("modules")
    if not isinstance(header, dict) or not isinstance(modules, list):
        raise CapePackError("manifest.json is missing header or modules")
    pack_id = require_uuid(header.get("uuid"), "manifest header UUID")
    persona_modules = [
        module
        for module in modules
        if isinstance(module, dict) and module.get("type") == "persona_piece"
    ]
    if len(persona_modules) != 1:
        raise CapePackError("manifest must contain exactly one persona_piece module")
    require_uuid(persona_modules[0].get("uuid"), "persona module UUID")

    meta_names = sorted(name for name in files if name.endswith(".meta.json"))
    if len(meta_names) != 1:
        raise CapePackError("archive must contain exactly one persona meta file")
    meta = parse_json(files[meta_names[0]], meta_names[0])
    if not isinstance(meta, dict) or meta.get("piece_type") != "persona_capes":
        raise CapePackError("persona meta must declare piece_type persona_capes")
    piece_id = require_uuid(meta.get("piece_id"), "persona piece UUID")
    textures = meta.get("texture_sources")
    if not isinstance(textures, list) or len(textures) != 1:
        raise CapePackError("persona cape must declare exactly one texture source")
    texture_name = textures[0].get("texture") if isinstance(textures[0], dict) else None
    if not isinstance(texture_name, str) or texture_name not in files:
        raise CapePackError("persona cape texture is missing from the archive")
    rgba = decode_png_rgba(files[texture_name], texture_name)

    title = archive.stem.replace(" (persona)", "")
    language = files.get("texts/en_US.lang", b"")
    try:
        for line in language.decode("utf-8").splitlines():
            if ".title=" in line:
                title = line.split("=", 1)[1].strip() or title
                break
    except UnicodeDecodeError as error:
        raise CapePackError("texts/en_US.lang is not valid UTF-8") from error

    return {
        "archive": archive,
        "pack_id": pack_id,
        "piece_id": piece_id,
        "title": title,
        "rgba": rgba,
        "sha256": sha256_file(archive),
    }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as archive_file:
        while chunk := archive_file.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def discover(source: Path) -> list[dict[str, object]]:
    if not source.is_dir():
        raise CapePackError(f"cape source directory does not exist: {source}")
    archives = sorted(source.glob("* (persona).zip"))
    if not archives:
        raise CapePackError(f"no '* (persona).zip' archives found in {source}")
    if len(archives) > MAX_ARCHIVES:
        raise CapePackError(f"more than {MAX_ARCHIVES} persona archives found")
    packs = [load_archive(archive) for archive in archives]
    pack_ids = [str(pack["pack_id"]) for pack in packs]
    piece_ids = [str(pack["piece_id"]) for pack in packs]
    if len(set(pack_ids)) != len(pack_ids):
        raise CapePackError("duplicate persona pack UUID detected")
    if len(set(piece_ids)) != len(piece_ids):
        raise CapePackError("duplicate persona piece UUID detected")
    return packs


def write_json_atomic(path: Path, value: object) -> None:
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    os.replace(temporary, path)


def install_runtime_capes(
    packs: list[dict[str, object]], launcher_root: Path, backup_root: Path
) -> Path:
    destination = launcher_root / "dobby-capes"
    stage = Path(tempfile.mkdtemp(prefix=".dobby-capes-", dir=launcher_root))
    backup: Path | None = None
    try:
        index_lines = ["version=2"]
        for pack in packs:
            piece_id = str(pack["piece_id"])
            pack_id = str(pack["pack_id"])
            title = str(pack["title"])
            if any(character in title for character in "\t\r\n"):
                raise CapePackError("cape titles cannot contain tabs or newlines")
            if not title or len(title.encode("utf-8")) > 80:
                raise CapePackError("cape titles must contain 1 to 80 UTF-8 bytes")
            pixels = pack["rgba"]
            if not isinstance(pixels, bytes) or len(pixels) != 64 * 32 * 4:
                raise CapePackError("decoded cape texture has an invalid byte count")
            (stage / f"{piece_id}.rgba").write_bytes(pixels)
            index_lines.append(f"{piece_id}\t{pack_id}\t{title}")
        (stage / "index.tsv").write_text("\n".join(index_lines) + "\n", encoding="utf-8")

        if destination.exists():
            backup_root.mkdir(parents=True, exist_ok=True)
            backup = backup_root / destination.name
            os.replace(destination, backup)
        try:
            os.replace(stage, destination)
        except Exception:
            if backup is not None and backup.exists() and not destination.exists():
                os.replace(backup, destination)
            raise
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    return destination


def default_launcher_root() -> Path:
    home = os.environ.get("HOME")
    if not home:
        raise CapePackError("HOME is unavailable; pass --launcher-root explicitly")
    return Path(home) / "Library/Application Support/mcpelauncher"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=Path("capes"))
    parser.add_argument("--launcher-root", type=Path, default=None)
    parser.add_argument("--check", action="store_true", help="validate without installing")
    args = parser.parse_args()

    try:
        packs = discover(args.source)
        for pack in packs:
            print(
                f"Validated {pack['title']} "
                f"(pack {pack['pack_id']}, piece {pack['piece_id']})"
            )
        if args.check:
            print(f"Validated {len(packs)} local persona cape packs.")
            return 0

        launcher_root = args.launcher_root or default_launcher_root()
        launcher_root.mkdir(parents=True, exist_ok=True)
        timestamp = time.strftime("%Y%m%d-%H%M%S")
        backup_root = (
            launcher_root
            / "disabled-mods/dobby-cape-deploy-backups"
            / f"{timestamp}-{os.getpid()}"
        )
        destination = install_runtime_capes(packs, launcher_root, backup_root)
        receipt = {
            "version": 2,
            "installed_at": timestamp,
            "destination": "dobby-capes",
            "packs": [
                {
                    "title": pack["title"],
                    "pack_id": pack["pack_id"],
                    "piece_id": pack["piece_id"],
                    "sha256": pack["sha256"],
                }
                for pack in packs
            ],
        }
        receipt_path = launcher_root / "dobby-capes-installed.json"
        write_json_atomic(receipt_path, receipt)
        if not (destination / "index.tsv").is_file():
            raise CapePackError("installed cape index verification failed")
        for pack in packs:
            texture = destination / f"{pack['piece_id']}.rgba"
            if not texture.is_file() or texture.stat().st_size != 64 * 32 * 4:
                raise CapePackError(
                    f"installed cape texture verification failed: {texture.name}"
                )
        print(f"Installed {len(packs)} local cape textures for Dobby's native hook.")
        if backup_root.exists():
            print(f"Previous Dobby cape data backed up under {backup_root}")
        return 0
    except (CapePackError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
