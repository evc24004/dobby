#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$script_dir/common.sh"
cd "$(project_root)"

artifact=build-android-arm64/libdobby.so
manifest=mod.json
[ -s "$artifact" ] || { echo "error: run the Android build first" >&2; exit 1; }

launcher_root="$HOME/Library/Application Support/mcpelauncher"
mods_root="$launcher_root/mods"
profiles="$launcher_root/profiles/profiles.ini"
registered=""
if [ -f "$profiles" ]; then
    registered=$(sed -n 's/^mods\\[0-9][0-9]*\\path=//p' "$profiles" | \
        awk '/\/(dobby|packet-debugger)\// {path=$0} END {print path}')
fi
install_dir=${registered:-"$mods_root/dobby/1.26.44.3/arm64-v8a/"}

case "$install_dir" in
    "$mods_root"/*) ;;
    *) echo "error: refusing launcher path outside the mod directory: $install_dir" >&2; exit 1 ;;
esac

other_libraries=$(find "$install_dir" -maxdepth 1 -type f -name '*.so' ! -name libdobby.so -print 2>/dev/null || true)
if [ -n "$other_libraries" ]; then
    echo "error: another mod library occupies the selected Dobby slot:" >&2
    printf '%s\n' "$other_libraries" >&2
    exit 1
fi

mkdir -p "$install_dir"
if [ -f "$install_dir/libdobby.so" ] && ! cmp -s "$artifact" "$install_dir/libdobby.so"; then
    backup_dir="$launcher_root/disabled-mods/dobby-deploy-backups/$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$backup_dir"
    cp "$install_dir/libdobby.so" "$backup_dir/libdobby.so"
    [ ! -f "$install_dir/mod.json" ] || cp "$install_dir/mod.json" "$backup_dir/mod.json"
    echo "Previous build backed up to: $backup_dir"
fi

artifact_temp="$install_dir/.libdobby.so.$$"
manifest_temp="$install_dir/.mod.json.$$"
trap 'rm -f "$artifact_temp" "$manifest_temp"' EXIT HUP INT TERM
cp "$artifact" "$artifact_temp"
cp "$manifest" "$manifest_temp"
mv "$artifact_temp" "$install_dir/libdobby.so"
mv "$manifest_temp" "$install_dir/mod.json"
trap - EXIT HUP INT TERM

build_hash=$(shasum -a 256 "$artifact" | awk '{print $1}')
installed_hash=$(shasum -a 256 "$install_dir/libdobby.so" | awk '{print $1}')
[ "$build_hash" = "$installed_hash" ] || { echo "error: installed artifact hash mismatch" >&2; exit 1; }

printf '%s\n' "$install_dir" > build-android-arm64/install-path.txt
echo "Installed Dobby $build_hash to: $install_dir"
