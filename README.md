# Dobby

Compact Minecraft Bedrock developer client for macOS
[mcpelauncher](https://github.com/minecraft-linux/mcpelauncher-manifest).

Features include a fail-closed startup protocol dumper, packet-violation and raw decode evidence, entity/player hitboxes, client-known chest and ore outlines, and passive network, chunk, and packet-traffic metrics.

## In-game diagnostic

`Mods > Dobby` contains the developer overlay. It shows native `PING`, client-observed `TPS~`, loaded chunks and outstanding requests, plus client `FPS` and resident memory use.

The bottom-right packet overlay shows compact incoming/outgoing packet and byte rates plus cumulative traffic formatted in `B`, `KB`, `MB`, `GB`, or `TB`. It appears only while a client world is rendering. `Packet traffic` toggles it independently from the top-right network metrics.

![Dobby packet traffic overlay](media/network.png)

![Dobby developer metrics overlay](media/debugger.png)

`Chest ESP` outlines chests found in Bedrock's decoded client chunk storage. It does not request or modify world data, so concealed chests appear only if the server actually sent them.

`Ore ESP` incrementally scans client-decoded subchunk palettes for vanilla ores, ancient debris, and mineral storage blocks. Loaded chunks refresh nearest-first when enabled, while nearby chunks are rechecked for live block changes. It never requests or modifies chunk data.

Menu toggles are saved locally and restored on the next launch.

## Startup protocol dump

On targets with a validated packet factory, Dobby sweeps the client packet
factory and passively traces each safe default packet into an isolated
`BinaryStream`; none of those bytes are sent. The 1.26.51.1 release keeps this
optional dump disabled until its factory and schema-writer vtables are mapped.

Default values cannot prove empty collection element types or untaken conditional branches, so Dobby also embeds the exact PrismarineJS Bedrock `1.26.40` baseline from commit `8a80816cbfb3fe2b609f2cde4e57796c8033af61`. Startup verifies its size and hash before atomically writing the pinned baseline files `protocol.json` and `version.json`; direct runtime evidence remains separate in `protocol-observed.json`. `protocol-dump-status.json` records reference-only IDs, runtime-name divergences, serialization failures, field traces, source commit, and hashes. A failed target or reference check produces no claimed verified baseline.

## In-game packet validation evidence

Dobby hooks the shared `Packet::read` return and
`PacketSecurityController::checkForViolation` entry used by inbound packet
deserialize, size, validation, and rate-limit failures. That captures the
original `std::error_code`, Bedrock's source filename/line/context frames,
nested causal errors, the last 32 inbound packets, an image-relative native
stack, client schema field reads, and bounded raw bytes before the result is
reduced to a generic disconnect.

The downstream `ClientNetworkHandler::handlePacketViolation` entry,
`PacketViolationWarningPacket`, and correlated `allowIncomingPacketId` plus
`onDisconnect` hooks remain as independent fallbacks. The direct handler also
captures its own `std::error_code` when no upstream result was observed. For
terminating errors, Dobby opens its diagnostic after Minecraft creates the generic
`ClientDisconnection-90` / `Block` screen so its popup stays visible.
Popup requests are queued onto the launcher's render/UI thread, so a packet
worker callback cannot silently lose the window.

The same `onDisconnect` hook now records every disconnect reason. Non-packet
reasons become `client_disconnect` events; `BadPacket` keeps its richer packet
report and gains the same callback evidence. Reports preserve the exact numeric
reason and stage, the 1.26.51.1 enum name and shipped UI codeword (for example,
`Disconnected (41) / Bat`), bounded server/body strings, callback flags, the
last 32 inbound packet IDs/sizes/ages, and an image-relative native stack. This
path is passive and does not suppress, rewrite, or retry the disconnect.

For generic `Bat` disconnects, Dobby also probes the exact RakNet dispatch
branch that produces reason 41. It distinguishes
`ID_DISCONNECTION_NOTIFICATION` (21) from `ID_CONNECTION_LOST` (22), preserves
bounded raw transport bytes, and correlates the last 32 outbound Minecraft
packet IDs. During resource-pack negotiation this shows whether the client sent
`ResourcePackClientResponse` before the transport closed. For packet 8, Dobby
serializes a passive copy into an isolated `BinaryStream` and records the exact
status (`refused`, `send_packs`, `have_all_packs`, or `completed`), the observed
status string, requested pack IDs, and bounded raw bytes. Those isolated bytes
are never sent. If the response has not been sent, the disconnect report also
correlates the launcher's bounded `DownloadTemp` state and file sizes so a
still-active critical world-pack download is visible in the popup and AI JSON.

The latest paste-ready report is `latest-dobby-violation.txt`; the complete
machine-readable snapshot for AI analysis is `latest-dobby-ai.json`, and the
append-only history is `dobby-events.jsonl`. These files live in the configured
Dobby output directory and are never committed.

The always-on capture path is allocation-free after its bounded schema buffers
warm up, timestamps packet boundaries instead of individual field reads, and
skips guarded error-object inspection for successful packets. Visual metric and
traffic overlays can remain disabled without disabling packet diagnostics.

![Packet rejection diagnostic window](media/image.png)

![Entity and player hitbox overlay](media/esp.png)

## Target

- Dobby `2.18.0`
- Minecraft Android `1.26.51.1`
- `arm64-v8a`
- network protocol `2193`
- Android version code `972605101`
- `libminecraftpe.so` build ID `712509dc14ccc233e91f267937dfb46ecdcc4b68`

The mod validates the target signature and refuses to patch incompatible builds.

On macOS, use the launcher’s Android/arm64 profile for this build. The
1.26.51.1 port initializes the validated packet diagnostics, schema tracing,
render, entity, network, outbound, packet-traffic, and loaded-chunk lifecycle
hooks. Protocol discovery, chest ESP, ore ESP, and the packet-rate/pending-chunk
paths remain fail-closed until their target-specific layouts and dispatch
targets are proven for this binary.

## Build

```sh
./build.sh
```

The default workflow runs release and sanitizer tests, builds ARM64, audits public
content, installs the verified artifact, commits and pushes changes, then starts
Minecraft and confirms Dobby is ready. Use `./build.sh --local` for a build-only
iteration or `./build.sh --help` for individual opt-outs.

Logs default to `~/Library/Application Support/mcpelauncher/`. Set
`DOBBY_OUTPUT_DIR` to override the output directory. Optional configuration:

- `DOBBY_AUTO_POPUP=0` disables automatic violation popups.
- `DOBBY_VERBOSE=1` enables verbose developer events.
- `DOBBY_HISTORY_LIMIT=100` sets the bounded in-memory history size.
- `DOBBY_RAW_CAPTURE_LIMIT=2048` sets the maximum captured packet-body bytes.

Raw captures and logs may contain server-provided data and remain excluded from Git.
