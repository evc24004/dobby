#!/bin/sh
set -eu

bundle_id=io.mrarm.mcpelauncher.ui
profiles_path="$HOME/Library/Application Support/mcpelauncher/profiles/profiles.ini"
log_path="$HOME/Library/Application Support/mcpelauncher/dobby.log"
protocol_path="$HOME/Library/Application Support/mcpelauncher/protocol.json"
observed_protocol_path="$HOME/Library/Application Support/mcpelauncher/protocol-observed.json"
version_path="$HOME/Library/Application Support/mcpelauncher/version.json"
protocol_status_path="$HOME/Library/Application Support/mcpelauncher/protocol-dump-status.json"
client_pattern='^/Applications/Minecraft Bedrock Launcher.app/Contents/MacOS/(\./)?mcpelauncher-client-arm64-v8a '
before_lines=0
[ ! -f "$log_path" ] || before_lines=$(wc -l < "$log_path" | tr -d ' ')

profile=${DOBBY_LAUNCHER_PROFILE:-}

live_client_pid() {
    pgrep -f "$client_pattern" 2>/dev/null | while IFS= read -r pid; do
        state=$(ps -p "$pid" -o state= 2>/dev/null | tr -d ' ')
        case "$state" in
            ''|Z*) ;;
            *) printf '%s\n' "$pid"; return 0 ;;
        esac
    done
}

if [ -z "$profile" ] && [ -f "$profiles_path" ]; then
    profile=$(awk '
        $0 == "[General]" {general=1; next}
        /^\[/ {general=0}
        general && /^selected=/ {sub(/^selected=/, ""); print; exit}
    ' "$profiles_path")
fi
[ -n "$profile" ] || {
    echo "error: no launcher profile selected; set DOBBY_LAUNCHER_PROFILE" >&2
    exit 1
}

osascript -e "tell application id \"$bundle_id\" to quit" >/dev/null 2>&1 || true
pkill -TERM -f "$client_pattern" 2>/dev/null || true
attempt=0
while [ -n "$(live_client_pid)" ] && [ "$attempt" -lt 10 ]; do
    sleep 1
    attempt=$((attempt + 1))
done
if [ -n "$(live_client_pid)" ]; then
    echo "error: existing Minecraft client did not stop cleanly" >&2
    exit 1
fi

sleep 1
open -n -b "$bundle_id" --args --profile "$profile"
echo "Started launcher profile: $profile"

attempt=0
while [ "$attempt" -lt 45 ]; do
    if [ -f "$log_path" ]; then
        new_log=$(tail -n "+$((before_lines + 1))" "$log_path")
        if printf '%s\n' "$new_log" | grep -q 'READY: Dobby' &&
                printf '%s\n' "$new_log" | grep -q 'protocol startup dump complete:'; then
            client_pid=$(live_client_pid)
            [ -n "$client_pid" ] || {
                echo "error: Dobby reported READY but the Minecraft client already exited" >&2
                exit 1
            }
            stable=0
            while [ "$stable" -lt 10 ]; do
                current_client_pid=$(live_client_pid)
                [ "$current_client_pid" = "$client_pid" ] || {
                    echo "error: Minecraft crashed during the Dobby stability check" >&2
                    exit 1
                }
                sleep 1
                stable=$((stable + 1))
            done
            python3 - "$protocol_path" "$observed_protocol_path" "$version_path" "$protocol_status_path" <<'PY'
import hashlib
import json
import pathlib
import sys

protocol_path, observed_path, version_path, status_path = map(pathlib.Path, sys.argv[1:])
protocol = json.loads(protocol_path.read_text())
observed = json.loads(observed_path.read_text())
version = json.loads(version_path.read_text())
status = json.loads(status_path.read_text())
if "mcpe_packet" not in protocol.get("types", {}):
    raise SystemExit("error: protocol.json has no mcpe_packet type")
if "mcpe_packet" not in observed.get("types", {}):
    raise SystemExit("error: protocol-observed.json has no mcpe_packet type")
if version.get("version") != 2168 or version.get("minecraftVersion") != "1.26.40":
    raise SystemExit("error: version.json does not describe Bedrock 1.26.40 protocol 2168")
if status.get("factory_packets", 0) <= 0 or status.get("serialized_packets", 0) <= 0:
    raise SystemExit("error: protocol startup sweep did not serialize packets")
if not status.get("reference_verified"):
    raise SystemExit("error: embedded Prismarine reference failed startup verification")
if status.get("reference_packets") != 244 or status.get("complete_packets") != 244:
    raise SystemExit("error: startup protocol catalog is not complete")
packet_mappings = protocol["types"]["mcpe_packet"][1][0]["type"][1]["mappings"]
if len(packet_mappings) != 244:
    raise SystemExit("error: protocol.json does not contain all 244 packet IDs")
protocol_sha = hashlib.sha256(protocol_path.read_bytes()).hexdigest()
version_sha = hashlib.sha256(version_path.read_bytes()).hexdigest()
if protocol_sha != status.get("reference_protocol_sha256"):
    raise SystemExit("error: protocol.json differs from the pinned reference")
if version_sha != status.get("reference_version_sha256"):
    raise SystemExit("error: version.json differs from the pinned reference")
print(
    "Verified startup protocol dump: "
    f"factory={status['factory_packets']} "
    f"serialized={status['serialized_packets']} "
    f"complete={status['complete_packets']} "
    f"reference_only={len(status.get('reference_only_packets', []))} "
    f"name_divergences={len(status.get('runtime_name_divergences', []))}"
)
PY
            printf '%s\n' "$new_log" | grep -E \
                'library loaded: Dobby|installed ReadOnlyBinaryStream|installed PacketViolationWarningPacket|protocol startup dump complete|READY: Dobby|registered Mods > Dobby'
            echo "Minecraft client $client_pid remained stable for 10 seconds after READY."
            exit 0
        fi
    fi
    sleep 1
    attempt=$((attempt + 1))
done

echo "error: Minecraft started, but Dobby did not report READY within 45 seconds" >&2
exit 1
