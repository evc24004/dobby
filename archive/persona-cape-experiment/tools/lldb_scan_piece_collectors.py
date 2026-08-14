"""Read-only LLDB scan for live Bedrock persona piece collectors.

Usage from LLDB after attaching to the client:
  command script import tools/lldb_scan_piece_collectors.py
  dobby-scan-piece-collectors [auto|<minecraft-base>]
"""

from __future__ import annotations

import json
import shlex
import struct

import lldb


_IPIECE_COLLECTOR_VTABLE = 0x11FA7EB0
_EXHAUSTIVE_COLLECTOR_VTABLE = 0x11FA6E58
_PIECE_COLLECTOR_SHARED_BLOCK_VTABLE = 0x11FED3F0
_PIECE_OFFER_SHARED_BLOCK_VTABLE = 0x11F06380
_STORE_CATALOG_ITEM_VTABLE = 0x11FF8748
_STORE_CATALOG_ITEM_KEY_OFFSET = 0x20
_STORE_CATALOG_ITEM_KEY_GETTER = 0x0B234ECC
_STORE_CATALOG_ITEM_KEY_GETTER_SIGNATURE = bytes.fromhex(
    "00800091c0035fd6"
)
_PAN_CAPE_PRODUCT_ID = b"f72bd899-c3f6-4516-ad10-a70c36a98641"
_WRAPPER_SIZE = 40
_MAXIMUM_WRAPPERS = 4096
_MAXIMUM_REGION_SIZE = 256 * 1024 * 1024
_PERSONA_MANAGER_OWNED_PIECES = 0x0A63BE68
_PERSONA_REPOSITORY_OFFLINE_PIECES = 0x0A6EFD38
_PERSONA_PIECE_COPY_CONSTRUCTOR = 0x0A6339C8
_PERSONA_MANAGER_OWNED_SIGNATURE = bytes.fromhex(
    "ff0302d1fd7b02a9fc6f03a9fa6704a9"
)
_PERSONA_REPOSITORY_OFFLINE_SIGNATURE = bytes.fromhex(
    "ff4303d1fd7b07a9fc6f08a9fa6709a9"
)
_PERSONA_PIECE_COPY_SIGNATURE = bytes.fromhex(
    "ffc302d1fd7b05a9fc6f06a9fa6707a9"
)


def _read(process: lldb.SBProcess, address: int, size: int) -> bytes | None:
    error = lldb.SBError()
    data = process.ReadMemory(address, size, error)
    return data if error.Success() and len(data) == size else None


def _read_u64(process: lldb.SBProcess, address: int) -> int | None:
    data = _read(process, address, 8)
    return struct.unpack("<Q", data)[0] if data is not None else None


def _read_android_string(
    process: lldb.SBProcess, address: int, maximum_length: int = 128
) -> str | None:
    header = _read(process, address, 24)
    if header is None:
        return None
    first = header[0]
    if first & 1:
        length, data_address = struct.unpack_from("<QQ", header, 8)
    else:
        length = first >> 1
        data_address = address + 1
    if length == 0 or length > maximum_length or data_address == 0:
        return None
    data = _read(process, data_address, length)
    if data is None:
        return None
    try:
        value = data.decode("utf-8")
    except UnicodeDecodeError:
        return None
    if any(ord(character) < 0x20 for character in value):
        return None
    return value


def audit_piece_offer(debugger, command, result, _internal_dict) -> None:
    arguments = shlex.split(command)
    if len(arguments) != 1:
        result.SetError("expected <40-byte-wrapper-address>")
        return
    try:
        wrapper_address = int(arguments[0], 0)
    except ValueError as error:
        result.SetError(str(error))
        return
    process = debugger.GetSelectedTarget().GetProcess()
    source = _read_u64(process, wrapper_address)
    if source is None or source == 0:
        result.SetError("wrapper source pointer is unavailable")
        return

    direct_strings = []
    vector_fields = []
    for offset in range(0, 0x258, 8):
        value = _read_android_string(process, source + offset)
        if value is not None:
            direct_strings.append({"offset": offset, "value": value})
        for element_size in (8, 16, 24):
            layout = _vector_layout(process, source, offset, element_size)
            if layout is None or layout["count"] == 0 or layout["count"] > 64:
                continue
            entry = {
                "offset": offset,
                "element_size": element_size,
                **layout,
            }
            if element_size == 24:
                strings = []
                for index in range(layout["count"]):
                    text = _read_android_string(
                        process, layout["begin"] + index * element_size
                    )
                    if text is not None:
                        strings.append({"index": index, "value": text})
                if strings:
                    entry["strings"] = strings
            vector_fields.append(entry)
    result.AppendMessage(
        json.dumps(
            {
                "wrapper": wrapper_address,
                "source": source,
                "direct_strings": direct_strings,
                "vectors": vector_fields,
            },
            sort_keys=True,
        )
    )


def scan_piece_offers(debugger, command, result, _internal_dict) -> None:
    arguments = shlex.split(command)
    if len(arguments) != 1:
        result.SetError("expected <validated-minecraft-base>")
        return
    try:
        minecraft_base = int(arguments[0], 0)
    except ValueError as error:
        result.SetError(str(error))
        return
    process = debugger.GetSelectedTarget().GetProcess()
    if (
        _read(
            process,
            minecraft_base + _PERSONA_REPOSITORY_OFFLINE_PIECES,
            len(_PERSONA_REPOSITORY_OFFLINE_SIGNATURE),
        )
        != _PERSONA_REPOSITORY_OFFLINE_SIGNATURE
    ):
        result.SetError("explicit Minecraft base failed signature validation")
        return

    regions = _regions(process, writable=True)
    control_vtable = minecraft_base + _PIECE_OFFER_SHARED_BLOCK_VTABLE
    control_blocks = sorted(
        address
        for _name, address in _find_patterns(
            process,
            regions,
            {"piece_offer_control": struct.pack("<Q", control_vtable)},
            maximum_matches=512,
        )
    )
    offers = []
    for control in control_blocks:
        source = control + 0x18
        source_references = _find_patterns(
            process,
            regions,
            {"piece_offer_source": struct.pack("<Q", source)},
            maximum_matches=128,
        )
        strings = []
        vector = _vector_layout(process, source, 0x108, 24)
        if vector is not None and vector["count"] <= 64:
            for index in range(vector["count"]):
                value = _read_android_string(
                    process, vector["begin"] + index * 24
                )
                if value is not None:
                    strings.append(value)
        references = []
        for _reference_name, reference in source_references:
            references.append(
                {
                    "address": reference,
                    "preceding_40_aligned": reference % 40,
                    "following_qwords": [
                        _read_u64(process, reference + offset)
                        for offset in (0, 8, 16, 24, 32)
                    ],
                }
            )
        offers.append(
            {
                "control": control,
                "source": source,
                "identity_vector_0x108": vector,
                "identity_strings_0x108": strings,
                "references": references,
            }
        )
    # The live Pan-only picker wrapper was proven to encode these final three
    # qwords. Match the full 24-byte suffix, then recover the wrapper beginning
    # and the exact vector fields whose [begin,end) contain it.
    pan_wrapper_suffix = struct.pack("<QQQ", 0x66, 0x0000006600000000, 1)
    wrapper_matches = _find_patterns(
        process,
        regions,
        {"pan_wrapper_suffix": pan_wrapper_suffix},
        maximum_matches=128,
    )
    pan_wrappers = []
    for _match_name, suffix_address in wrapper_matches:
        wrapper = suffix_address - 16
        source = _read_u64(process, wrapper)
        control = _read_u64(process, wrapper + 8)
        if source is None or control is None or source == 0 or control == 0:
            continue
        begin_matches = _find_patterns(
            process,
            regions,
            {"pan_wrapper_begin": struct.pack("<Q", wrapper)},
            maximum_matches=128,
        )
        vector_owners = []
        for _begin_name, begin_field in begin_matches:
            layout_bytes = _read(process, begin_field, 24)
            if layout_bytes is None:
                continue
            begin, end, capacity = struct.unpack("<QQQ", layout_bytes)
            if (
                begin != wrapper
                or end < begin + _WRAPPER_SIZE
                or capacity < end
                or (end - begin) % _WRAPPER_SIZE
                or (capacity - begin) % _WRAPPER_SIZE
            ):
                continue
            vector_owners.append(
                {
                    "vector_field": begin_field,
                    "count": (end - begin) // _WRAPPER_SIZE,
                    "capacity": (capacity - begin) // _WRAPPER_SIZE,
                    "possible_owner_if_offset_0x38": begin_field - 0x38,
                    "owner_vtable_if_offset_0x38": _read_u64(
                        process, begin_field - 0x38
                    ),
                }
            )
        pan_wrappers.append(
            {
                "wrapper": wrapper,
                "source": source,
                "control": control,
                "control_vtable": _read_u64(process, control),
                "vector_owners": vector_owners,
            }
        )
    result.AppendMessage(
        json.dumps(
            {
                "minecraft_base": minecraft_base,
                "control_vtable": control_vtable,
                "regions_scanned": len(regions),
                "offers": offers,
                "pan_wrappers": pan_wrappers,
            },
            sort_keys=True,
        )
    )


def _wrapper_layout(process: lldb.SBProcess, collector: int) -> dict | None:
    data = _read(process, collector + 0x38, 24)
    if data is None:
        return None
    begin, end, capacity = struct.unpack("<QQQ", data)
    if begin == end == capacity == 0:
        return {"begin": 0, "count": 0, "capacity": 0}
    if begin == 0 or end < begin or capacity < end:
        return None
    used = end - begin
    available = capacity - begin
    if used % _WRAPPER_SIZE or available % _WRAPPER_SIZE:
        return None
    count = used // _WRAPPER_SIZE
    capacity_count = available // _WRAPPER_SIZE
    if count > _MAXIMUM_WRAPPERS or capacity_count > _MAXIMUM_WRAPPERS * 4:
        return None
    return {"begin": begin, "count": count, "capacity": capacity_count}


def _vector_layout(
    process: lldb.SBProcess,
    collector: int,
    offset: int,
    element_size: int,
) -> dict | None:
    data = _read(process, collector + offset, 24)
    if data is None:
        return None
    begin, end, capacity = struct.unpack("<QQQ", data)
    if begin == end == capacity == 0:
        return {"begin": 0, "count": 0, "capacity": 0}
    if begin == 0 or end < begin or capacity < end:
        return None
    used = end - begin
    available = capacity - begin
    if used % element_size or available % element_size:
        return None
    count = used // element_size
    capacity_count = available // element_size
    if count > _MAXIMUM_WRAPPERS or capacity_count > _MAXIMUM_WRAPPERS * 4:
        return None
    return {"begin": begin, "count": count, "capacity": capacity_count}


def _regions(
    process: lldb.SBProcess, *, writable: bool = False, executable: bool = False
) -> list[tuple[int, int]]:
    result = []
    regions = process.GetMemoryRegions()
    for index in range(regions.GetSize()):
        info = lldb.SBMemoryRegionInfo()
        if not regions.GetMemoryRegionAtIndex(index, info):
            continue
        begin = info.GetRegionBase()
        end = info.GetRegionEnd()
        if (
            info.IsReadable()
            and (not writable or info.IsWritable())
            and (not executable or info.IsExecutable())
            and end > begin
        ):
            result.append((begin, end))
    return result


def _find_patterns(
    process: lldb.SBProcess,
    regions: list[tuple[int, int]],
    patterns: dict[str, bytes],
    maximum_matches: int = 256,
) -> list[tuple[str, int]]:
    matches = []
    chunk_size = 1024 * 1024
    longest = max((len(value) for value in patterns.values()), default=1)
    for begin, end in regions:
        cursor = begin
        overlap = b""
        while cursor < end and len(matches) < maximum_matches:
            size = min(chunk_size, end - cursor)
            chunk = _read(process, cursor, size)
            if chunk is None:
                overlap = b""
                cursor += size
                continue
            searchable = overlap + chunk
            searchable_address = cursor - len(overlap)
            for name, pattern in patterns.items():
                offset = searchable.find(pattern)
                while offset >= 0:
                    address = searchable_address + offset
                    if address % 8 == 0:
                        matches.append((name, address))
                    offset = searchable.find(pattern, offset + 1)
            overlap = searchable[-(longest - 1) :] if longest > 1 else b""
            cursor += size
    return matches


def _find_minecraft_base(process: lldb.SBProcess) -> tuple[int, int] | None:
    # mcpelauncher keeps the Android ELF image as readable guest memory and
    # executes translated host code elsewhere, so the validated bytes are not
    # necessarily in a host-executable mapping.
    readable_regions = _regions(process)
    candidates = _find_patterns(
        process,
        readable_regions,
        # The owned/offline entries are patched by the running Dobby build.
        # Anchor on the validated, unpatched PersonaPiece copy constructor.
        {"persona_piece_copy": _PERSONA_PIECE_COPY_SIGNATURE},
        maximum_matches=32,
    )
    for _name, address in candidates:
        base = address - _PERSONA_PIECE_COPY_CONSTRUCTOR
        if base < 0 or base % 0x1000:
            continue
        if (
            _read(
                process,
                base + _STORE_CATALOG_ITEM_KEY_GETTER,
                len(_STORE_CATALOG_ITEM_KEY_GETTER_SIGNATURE),
            )
            != _STORE_CATALOG_ITEM_KEY_GETTER_SIGNATURE
        ):
            continue
        return base, len(readable_regions)
    return None


def find_minecraft_base(debugger, _command, result, _internal_dict) -> None:
    process = debugger.GetSelectedTarget().GetProcess()
    discovered = _find_minecraft_base_from_pan(process)
    if discovered is None:
        fallback = _find_minecraft_base(process)
        if fallback is None:
            result.SetError(
                "exact supported Minecraft image was not found from Pan or signatures"
            )
            return
        minecraft_base, region_count = fallback
        result.AppendMessage(
            json.dumps(
                {
                    "minecraft_base": minecraft_base,
                    "base_evidence": {
                        "method": "exact_unpatched_signatures",
                        "readable_regions_scanned": region_count,
                    },
                },
                sort_keys=True,
            )
        )
        return
    minecraft_base, evidence = discovered
    result.AppendMessage(
        json.dumps(
            {"minecraft_base": minecraft_base, "base_evidence": evidence},
            sort_keys=True,
        )
    )


def find_minecraft_base_in_range(
    debugger, command, result, _internal_dict
) -> None:
    arguments = shlex.split(command)
    if len(arguments) != 2:
        result.SetError("expected <guest-mapping-begin> <guest-mapping-end>")
        return
    try:
        begin, end = (int(argument, 0) for argument in arguments)
    except ValueError as error:
        result.SetError(str(error))
        return
    if begin <= 0 or end <= begin or end - begin > 512 * 1024 * 1024:
        result.SetError("guest mapping range is invalid or exceeds 512 MiB")
        return

    process = debugger.GetSelectedTarget().GetProcess()
    candidates = _find_patterns(
        process,
        [(begin, end)],
        {"persona_piece_copy": _PERSONA_PIECE_COPY_SIGNATURE},
        maximum_matches=64,
    )
    accepted = []
    for _name, address in candidates:
        minecraft_base = address - _PERSONA_PIECE_COPY_CONSTRUCTOR
        if minecraft_base <= 0 or minecraft_base % 0x1000:
            continue
        if (
            _read(
                process,
                minecraft_base + _STORE_CATALOG_ITEM_KEY_GETTER,
                len(_STORE_CATALOG_ITEM_KEY_GETTER_SIGNATURE),
            )
            != _STORE_CATALOG_ITEM_KEY_GETTER_SIGNATURE
        ):
            continue
        accepted.append(minecraft_base)
    if len(set(accepted)) != 1:
        result.SetError(
            "guest mapping did not produce one exact supported image base: "
            + json.dumps(sorted(set(accepted)))
        )
        return
    result.AppendMessage(
        json.dumps(
            {
                "minecraft_base": accepted[0],
                "mapping_begin": begin,
                "mapping_end": end,
                "copy_signature_candidates": len(candidates),
            },
            sort_keys=True,
        )
    )


def trace_pan_offer(debugger, _command, result, _internal_dict) -> None:
    process = debugger.GetSelectedTarget().GetProcess()
    discovered = _find_minecraft_base_from_pan(process)
    if discovered is None:
        result.SetError("exact supported Minecraft image was not found from Pan")
        return
    minecraft_base, evidence = discovered
    pan_store_item = evidence["pan_store_item"]
    regions = _regions(process, writable=True)
    references = _find_patterns(
        process,
        regions,
        {"pan_store_item": struct.pack("<Q", pan_store_item)},
        maximum_matches=512,
    )
    candidates = []
    expected_control_vtable = (
        minecraft_base + _PIECE_OFFER_SHARED_BLOCK_VTABLE
    )
    for _name, reference in references:
        # PieceOffer is allocated inline at control+0x18 in the exact
        # 0x270-byte shared_ptr block. The Pan StoreCatalogItem pointer lives
        # somewhere inside that PieceOffer; prove the allocation boundary by
        # walking backwards only across its bounded payload and requiring the
        # exact shared-control vtable.
        for source_offset in range(0, 0x258, 8):
            source = reference - source_offset
            control = source - 0x18
            if _read_u64(process, control) != expected_control_vtable:
                continue
            source_refs = _find_patterns(
                process,
                regions,
                {"piece_offer_source": struct.pack("<Q", source)},
                maximum_matches=128,
            )
            wrappers = []
            for _source_name, wrapper in source_refs:
                if _read_u64(process, wrapper + 8) != control:
                    continue
                wrappers.append(wrapper)
            owners = []
            for wrapper in wrappers:
                # A vector may start before Pan when None or other offers are
                # present. Test every bounded predecessor as a possible begin,
                # then require a native [begin,end,capacity] owner field that
                # contains the exact wrapper address.
                for predecessor_count in range(0, 64):
                    begin = wrapper - predecessor_count * _WRAPPER_SIZE
                    begin_refs = _find_patterns(
                        process,
                        regions,
                        {"wrapper_vector_begin": struct.pack("<Q", begin)},
                        maximum_matches=128,
                    )
                    for _begin_name, begin_field in begin_refs:
                        data = _read(process, begin_field, 24)
                        if data is None:
                            continue
                        layout_begin, end, capacity = struct.unpack("<QQQ", data)
                        if (
                            layout_begin != begin
                            or end < wrapper + _WRAPPER_SIZE
                            or capacity < end
                            or (end - begin) % _WRAPPER_SIZE
                            or (capacity - begin) % _WRAPPER_SIZE
                            or (end - begin) // _WRAPPER_SIZE > _MAXIMUM_WRAPPERS
                        ):
                            continue
                        owners.append(
                            {
                                "vector_field": begin_field,
                                "count": (end - begin) // _WRAPPER_SIZE,
                                "capacity": (capacity - begin) // _WRAPPER_SIZE,
                                "owner_if_offset_0x38": begin_field - 0x38,
                                "owner_vtable": _read_u64(
                                    process, begin_field - 0x38
                                ),
                            }
                        )
                    if owners:
                        break
            candidates.append(
                {
                    "pan_pointer_reference": reference,
                    "source": source,
                    "source_offset": source_offset,
                    "control": control,
                    "wrappers": wrappers,
                    "owners": owners,
                }
            )
    result.AppendMessage(
        json.dumps(
            {
                "minecraft_base": minecraft_base,
                "pan_store_item": pan_store_item,
                "pan_store_item_references": len(references),
                "pan_pointer_references": [
                    address for _name, address in references
                ],
                "candidates": candidates,
            },
            sort_keys=True,
        )
    )


def trace_pan_referrers(debugger, _command, result, _internal_dict) -> None:
    process = debugger.GetSelectedTarget().GetProcess()
    discovered = _find_minecraft_base_from_pan(process)
    if discovered is None:
        result.SetError("exact supported Minecraft image was not found from Pan")
        return
    minecraft_base, evidence = discovered
    pan_store_item = evidence["pan_store_item"]
    regions = [
        region for region in _regions(process, writable=True)
        if region[0] >= 0x600000000
    ]
    item_refs = sorted(
        address for _name, address in _find_patterns(
            process,
            regions,
            {"pan_store_item": struct.pack("<Q", pan_store_item)},
            maximum_matches=512,
        )
    )
    levels = []
    frontier = item_refs
    seen = set(frontier)
    for depth in range(3):
        frontier = frontier[:64]
        patterns = {
            f"referrer_{index}": struct.pack("<Q", pointee)
            for index, pointee in enumerate(frontier)
        }
        grouped = {pointee: [] for pointee in frontier}
        for name, address in _find_patterns(
            process, regions, patterns, maximum_matches=512
        ):
            pointee = frontier[int(name.rsplit("_", 1)[1])]
            grouped[pointee].append(address)
        next_frontier = []
        level = []
        for pointee, refs in grouped.items():
            refs.sort()
            level.append({"pointee": pointee, "referrers": refs})
            for address in refs:
                if address not in seen:
                    seen.add(address)
                    next_frontier.append(address)
        levels.append({"depth": depth + 1, "entries": level})
        frontier = next_frontier
        if not frontier:
            break
    result.AppendMessage(json.dumps({
        "minecraft_base": minecraft_base,
        "pan_store_item": pan_store_item,
        "levels": levels,
    }, sort_keys=True))


def dump_piece_collector_controls(debugger, _command, result, _internal_dict) -> None:
    process = debugger.GetSelectedTarget().GetProcess()
    discovered = _find_minecraft_base_from_pan(process)
    if discovered is None:
        result.SetError("exact supported Minecraft image was not found from Pan")
        return
    minecraft_base, _evidence = discovered
    regions = [
        region for region in _regions(process, writable=True)
        if region[0] >= 0x600000000
    ]
    patterns = {
        "collector_control": struct.pack(
            "<Q", minecraft_base + _PIECE_COLLECTOR_SHARED_BLOCK_VTABLE
        ),
        "piece_offer_control": struct.pack(
            "<Q", minecraft_base + _PIECE_OFFER_SHARED_BLOCK_VTABLE
        ),
    }
    found = []
    for name, address in _find_patterns(
        process, regions, patterns, maximum_matches=1024
    ):
        qwords = [
            _read_u64(process, address + offset)
            for offset in range(0, 0x100, 8)
        ]
        found.append({"type": name, "address": address, "qwords": qwords})
    result.AppendMessage(json.dumps({
        "minecraft_base": minecraft_base,
        "objects": found,
    }, sort_keys=True))


def trace_piece_collector_owners(debugger, _command, result, _internal_dict) -> None:
    """Trace the exact shared and raw references owning each live collector.

    The supported client allocates persona::PieceCollector inline at +0x18 in
    a libc++ __shared_ptr_emplace control block.  This command is deliberately
    read-only: it records references to both halves of that shared_ptr so the
    actual PersonaOfferComponent field can be identified before any hook is
    proposed.
    """
    process = debugger.GetSelectedTarget().GetProcess()
    discovered = _find_minecraft_base_from_pan(process)
    if discovered is None:
        result.SetError("exact supported Minecraft image was not found from Pan")
        return
    minecraft_base, _evidence = discovered
    regions = [
        region for region in _regions(process, writable=True)
        if region[0] >= 0x600000000
    ]
    control_vtable = minecraft_base + _PIECE_COLLECTOR_SHARED_BLOCK_VTABLE
    controls = sorted(
        address
        for _name, address in _find_patterns(
            process,
            regions,
            {"collector_control": struct.pack("<Q", control_vtable)},
            maximum_matches=128,
        )
    )
    collectors = []
    for control in controls:
        source = control + 0x18
        patterns = {
            "control": struct.pack("<Q", control),
            "source": struct.pack("<Q", source),
        }
        references = {"control": [], "source": []}
        for name, address in _find_patterns(
            process, regions, patterns, maximum_matches=1024
        ):
            if control <= address < control + 0x200:
                continue
            references[name].append(address)

        shared_pairs = []
        for source_reference in references["source"]:
            if _read_u64(process, source_reference + 8) == control:
                shared_pairs.append(source_reference)

        referrer_patterns = {}
        referrer_values = sorted(
            set(references["source"] + references["control"] + shared_pairs)
        )
        for index, value in enumerate(referrer_values[:128]):
            referrer_patterns[f"referrer_{index}"] = struct.pack("<Q", value)
        second_level = []
        if referrer_patterns:
            for name, address in _find_patterns(
                process, regions, referrer_patterns, maximum_matches=2048
            ):
                pointee = referrer_values[int(name.rsplit("_", 1)[1])]
                second_level.append({"pointee": pointee, "address": address})

        collectors.append({
            "control": control,
            "source": source,
            "source_references": sorted(references["source"]),
            "control_references": sorted(references["control"]),
            "shared_ptr_pairs": sorted(shared_pairs),
            "second_level": sorted(
                second_level, key=lambda entry: (entry["pointee"], entry["address"])
            ),
        })
    result.AppendMessage(json.dumps({
        "minecraft_base": minecraft_base,
        "collectors": collectors,
    }, sort_keys=True))


def _find_minecraft_base_from_pan(
    process: lldb.SBProcess,
) -> tuple[int, dict] | None:
    # Bedrock guest objects are allocated in the launcher's high-address
    # malloc arenas. Resource mappings below this boundary contain many copied
    # catalog strings and can exhaust the bounded match list before the live
    # StoreCatalogItem is reached.
    writable_regions = [
        region
        for region in _regions(process, writable=True)
        if region[0] >= 0x600000000
    ]
    pan_matches = _find_patterns(
        process,
        writable_regions,
        {"pan_product_id": _PAN_CAPE_PRODUCT_ID},
        maximum_matches=4096,
    )
    pan_addresses = sorted({address for _name, address in pan_matches})
    if not pan_addresses:
        return None

    pointer_patterns = {
        f"pan_pointer_{index}": struct.pack("<Q", address)
        for index, address in enumerate(pan_addresses)
    }
    pointer_matches = _find_patterns(
        process,
        writable_regions,
        pointer_patterns,
        maximum_matches=4096,
    )
    for name, pointer_address in pointer_matches:
        pan_address = pan_addresses[int(name.rsplit("_", 1)[1])]
        # Android libc++ long strings store {capacity, length, data}. The
        # pointer match is therefore at string+0x10. StoreCatalogItem::getKey
        # returns the string at item+0x20 for this exact build.
        string_object = pointer_address - 16
        length = _read_u64(process, string_object + 8)
        if length != len(_PAN_CAPE_PRODUCT_ID):
            continue
        item = string_object - _STORE_CATALOG_ITEM_KEY_OFFSET
        vtable = _read_u64(process, item)
        if vtable is None or vtable < _STORE_CATALOG_ITEM_VTABLE:
            continue
        base = vtable - _STORE_CATALOG_ITEM_VTABLE
        if (
            _read(
                process,
                base + _STORE_CATALOG_ITEM_KEY_GETTER,
                len(_STORE_CATALOG_ITEM_KEY_GETTER_SIGNATURE),
            )
            != _STORE_CATALOG_ITEM_KEY_GETTER_SIGNATURE
        ):
            continue
        if (
            _read(
                process,
                base + _PERSONA_PIECE_COPY_CONSTRUCTOR,
                len(_PERSONA_PIECE_COPY_SIGNATURE),
            )
            != _PERSONA_PIECE_COPY_SIGNATURE
        ):
            continue
        return base, {
            "pan_product_data": pan_address,
            "pan_key_string": string_object,
            "pan_store_item": item,
            "pan_store_vtable": vtable,
            "writable_regions_scanned": len(writable_regions),
        }
    return None


def _pan_source_vector_candidates(
    process: lldb.SBProcess,
    minecraft_base: int,
    writable_regions: list[tuple[int, int]],
    pan_store_item: int,
) -> list[dict]:
    """Find vectors of StoreCatalogItem shared_ptrs containing the exact Pan item.

    This is a fallback for picker screens whose short-lived collector object has
    already been released while its presentation data remains cached.
    """
    item_references = _find_patterns(
        process,
        writable_regions,
        {"pan_store_item_pointer": struct.pack("<Q", pan_store_item)},
        maximum_matches=256,
    )
    candidates = []
    expected_vtable = minecraft_base + _STORE_CATALOG_ITEM_VTABLE
    for _name, reference in item_references:
        # A source vector element is shared_ptr<IStoreCatalogItem>: object and
        # control-block pointers. Walk exact-vtable objects in both directions
        # to recover the allocation's used range without guessing an owner.
        begin = reference
        for _index in range(_MAXIMUM_WRAPPERS):
            previous = begin - 16
            previous_item = _read_u64(process, previous)
            if previous_item is None or _read_u64(process, previous_item) != expected_vtable:
                break
            begin = previous
        end = reference + 16
        for _index in range(_MAXIMUM_WRAPPERS):
            next_item = _read_u64(process, end)
            if next_item is None or _read_u64(process, next_item) != expected_vtable:
                break
            end += 16
        count = (end - begin) // 16
        if count == 0 or count > _MAXIMUM_WRAPPERS:
            continue
        begin_references = _find_patterns(
            process,
            writable_regions,
            {"source_vector_begin": struct.pack("<Q", begin)},
            maximum_matches=64,
        )
        owners = []
        for _owner_name, begin_reference in begin_references:
            layout_bytes = _read(process, begin_reference, 24)
            if layout_bytes is None:
                continue
            layout_begin, layout_end, layout_capacity = struct.unpack(
                "<QQQ", layout_bytes
            )
            if (
                layout_begin != begin
                or layout_end < reference + 16
                or layout_end > end
                or layout_capacity < layout_end
                or (layout_end - layout_begin) % 16
                or (layout_capacity - layout_begin) % 16
            ):
                continue
            owners.append(
                {
                    "vector_field": begin_reference,
                    "possible_collector_if_offset_0x50": begin_reference - 0x50,
                    "owner_vtable_if_offset_0x50": _read_u64(
                        process, begin_reference - 0x50
                    ),
                    "begin": layout_begin,
                    "end": layout_end,
                    "capacity": layout_capacity,
                    "count": (layout_end - layout_begin) // 16,
                }
            )
        candidates.append(
            {
                "pan_pointer_reference": reference,
                "exact_vtable_run_begin": begin,
                "exact_vtable_run_end": end,
                "exact_vtable_run_count": count,
                "owners": owners,
            }
        )
    return candidates


def scan_piece_collectors(debugger, command, result, _internal_dict) -> None:
    arguments = shlex.split(command)
    if len(arguments) > 1:
        result.SetError("expected [auto|<minecraft-base>]")
        return

    process = debugger.GetSelectedTarget().GetProcess()
    executable_region_count = None
    if not arguments or arguments[0] == "auto":
        discovered = _find_minecraft_base(process)
        if discovered is None:
            pan_discovered = _find_minecraft_base_from_pan(process)
            if pan_discovered is None:
                result.SetError("exact supported Minecraft image was not found")
                return
            minecraft_base, base_evidence = pan_discovered
        else:
            minecraft_base, executable_region_count = discovered
            base_evidence = {"method": "function_signature"}
    else:
        try:
            minecraft_base = int(arguments[0], 0)
        except ValueError as error:
            result.SetError(str(error))
            return
        if (
            _read(
                process,
                minecraft_base + _PERSONA_REPOSITORY_OFFLINE_PIECES,
                len(_PERSONA_REPOSITORY_OFFLINE_SIGNATURE),
            )
            != _PERSONA_REPOSITORY_OFFLINE_SIGNATURE
        ):
            result.SetError("explicit Minecraft base failed signature validation")
            return
        base_evidence = {"method": "explicit_validated_function_signature"}

    regions = _regions(process, writable=True)
    patterns = {
        "IPieceCollector": struct.pack(
            "<Q", minecraft_base + _IPIECE_COLLECTOR_VTABLE
        ),
        "ExhaustivePieceCollector": struct.pack(
            "<Q", minecraft_base + _EXHAUSTIVE_COLLECTOR_VTABLE
        ),
        "PieceCollectorSharedBlock": struct.pack(
            "<Q", minecraft_base + _PIECE_COLLECTOR_SHARED_BLOCK_VTABLE
        ),
    }
    matches = []
    for collector_type, address in _find_patterns(process, regions, patterns):
        collector_address = address
        embedded_vtable = None
        if collector_type == "PieceCollectorSharedBlock":
            for object_offset in range(16, 65, 8):
                candidate = _read_u64(process, address + object_offset)
                if candidate in {
                    minecraft_base + _IPIECE_COLLECTOR_VTABLE,
                    minecraft_base + _EXHAUSTIVE_COLLECTOR_VTABLE,
                }:
                    collector_address = address + object_offset
                    embedded_vtable = candidate
                    break
        wrapper_layout = _vector_layout(
            process, collector_address, 0x38, _WRAPPER_SIZE
        )
        store_item_layout = _vector_layout(
            process, collector_address, 0x50, 16
        )
        if wrapper_layout is None and store_item_layout is None:
            continue
        matches.append(
            {
                "type": collector_type,
                "address": address,
                "collector_address": collector_address,
                "embedded_vtable": embedded_vtable,
                "piece_offer_wrappers": wrapper_layout,
                "store_catalog_items": store_item_layout,
                "state": _read_u64(process, collector_address + 0xC0),
            }
        )

    pan_source_vector_candidates = []
    pan_store_item = base_evidence.get("pan_store_item")
    if not matches and isinstance(pan_store_item, int):
        pan_source_vector_candidates = _pan_source_vector_candidates(
            process,
            minecraft_base,
            regions,
            pan_store_item,
        )

    result.AppendMessage(
        json.dumps(
            {
                "minecraft_base": minecraft_base,
                "base_evidence": base_evidence,
                "executable_regions_scanned": executable_region_count,
                "regions_scanned": len(regions),
                "collectors": matches,
                "pan_source_vector_candidates": pan_source_vector_candidates,
            },
            sort_keys=True,
        )
    )


def __lldb_init_module(debugger, _internal_dict) -> None:
    debugger.HandleCommand(
        "command script add -f lldb_scan_piece_collectors.find_minecraft_base "
        "dobby-find-minecraft-base"
    )
    debugger.HandleCommand(
        "command script add -f "
        "lldb_scan_piece_collectors.find_minecraft_base_in_range "
        "dobby-find-minecraft-base-in-range"
    )
    debugger.HandleCommand(
        "command script add -f lldb_scan_piece_collectors.trace_pan_offer "
        "dobby-trace-pan-offer"
    )
    debugger.HandleCommand(
        "command script add -f lldb_scan_piece_collectors.trace_pan_referrers "
        "dobby-trace-pan-referrers"
    )
    debugger.HandleCommand(
        "command script add -f "
        "lldb_scan_piece_collectors.dump_piece_collector_controls "
        "dobby-dump-piece-collector-controls"
    )
    debugger.HandleCommand(
        "command script add -f "
        "lldb_scan_piece_collectors.trace_piece_collector_owners "
        "dobby-trace-piece-collector-owners"
    )
    debugger.HandleCommand(
        "command script add -f lldb_scan_piece_collectors.scan_piece_collectors "
        "dobby-scan-piece-collectors"
    )
    debugger.HandleCommand(
        "command script add -f lldb_scan_piece_collectors.audit_piece_offer "
        "dobby-audit-piece-offer"
    )
    debugger.HandleCommand(
        "command script add -f lldb_scan_piece_collectors.scan_piece_offers "
        "dobby-scan-piece-offers"
    )
