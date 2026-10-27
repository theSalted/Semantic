#!/usr/bin/env bash
set -euo pipefail

# Installs the Semantic native plugins for every platform into a Unity project, from the
# latest successful CI run on main (or a given run), so either OS can develop for both.
# Usage: scripts/sync-unity-plugins.sh /path/to/UnityProject [--run <run-id>] [--local-macos <dylib>]
project="${1:?Usage: scripts/sync-unity-plugins.sh /path/to/UnityProject [--run <run-id>] [--local-macos <dylib>]}"
shift
run=""; local_macos=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --run) run="${2:?}"; shift 2 ;;
        --local-macos) local_macos="${2:?}"; shift 2 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
plugins="$project/Assets/Plugins"
[[ -d "$project/Assets" ]] || { echo "Not a Unity project: $project" >&2; exit 1; }
repo="theSalted/Semantic"
if [[ -z "$run" ]]; then
    run="$(gh run list --repo "$repo" --workflow native.yml --branch main --status success --limit 1 --json databaseId -q '.[0].databaseId')"
    [[ -n "$run" ]] || { echo "No successful native CI run on main yet." >&2; exit 1; }
fi
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
gh run download "$run" --repo "$repo" --dir "$work"

# Stable GUIDs keep .meta files identical on every machine and in version control.
guid() { printf '%s' "semantic-plugin:$1" | md5 | cut -c1-32; }
command -v md5 >/dev/null || guid() { printf '%s' "semantic-plugin:$1" | md5sum | cut -c1-32; }

meta() { # path os cpu-editor platform-block
    local file="$1" name; name="$(basename "$file")"
    [[ -e "$file.meta" ]] && return 0
    cat > "$file.meta" <<EOF
fileFormatVersion: 2
guid: $(guid "$2/$name")
PluginImporter:
  externalObjects: {}
  serializedVersion: 3
  iconMap: {}
  executionOrder: {}
  defineConstraints: []
  isPreloaded: 0
  isOverridable: 0
  isExplicitlyReferenced: 0
  validateReferences: 1
  platformData:
    Any:
      enabled: 0
      settings: {}
    Editor:
      enabled: 1
      settings:
        CPU: $3
        DefaultValueInitialized: true
        OS: $2
$4
  userData:
  assetBundleName:
  assetBundleVariant:
EOF
}

windows="$plugins/Windows/x86_64"
mkdir -p "$windows"
# Replace the whole Windows set: backends are discovered by file name at runtime.
for stale in "$windows"/*.dll; do
    if [[ -e "$stale" && "$(basename "$stale")" != Semantic.Native.dll ]]; then rm -f -- "$stale" "$stale.meta"; fi
done
cp "$work/semantic-plugin-windows-x64/"*.dll "$windows/"
for dll in "$windows"/*.dll; do
    meta "$dll" Windows x86_64 "    Win:
      enabled: 0
      settings:
        CPU: None
    Win64:
      enabled: 1
      settings:
        CPU: x86_64"
done

macos="$plugins/macOS"
mkdir -p "$macos"
source_dylib="${local_macos:-$work/semantic-plugin-macos-arm64/libSemantic.Native.dylib}"
# Atomic replace: a running Editor keeps its mapped copy until restart.
cp "$source_dylib" "$macos/.libSemantic.Native.dylib.new"
mv -f "$macos/.libSemantic.Native.dylib.new" "$macos/libSemantic.Native.dylib"
meta "$macos/libSemantic.Native.dylib" OSX ARM64 "    OSXUniversal:
      enabled: 1
      settings:
        CPU: ARM64"

echo "Installed Semantic plugins from run $run into $plugins. Restart Unity to load native changes."
