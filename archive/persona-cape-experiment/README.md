# Archived persona cape experiment

Status: failed experiment, archived 2026-08-14.

This code attempted to add local persona cape entries to Bedrock's cape picker
and carry the selected pixels through `PlayerSkinPacket`. Repeated live tests on
Minecraft `1.26.40.5` did not populate the picker reliably, so the approach is
not considered functional or supported.

The files are retained as reverse-engineering evidence only. They are excluded
from Dobby's CMake targets, install workflow, preferences, UI, and tests. The
active client remains passive and does not mutate gameplay packets.

Contents:

- `src/hooks/`: repository injection, ownership bypass, and skin-packet hooks.
- `src/platform/`: local cape index and texture loader used by the hooks.
- `scripts/`: the former local cape pack installer.
- `tools/`: the LLDB scanner used while investigating picker internals.

This archive is intentionally not a standalone build. It depended on target
offsets and runtime state that were removed from the active client during the
cleanup. Consult repository history if the experiment must be reconstructed.
