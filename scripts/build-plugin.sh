#!/usr/bin/env bash
set -euo pipefail

# Local macOS (Apple silicon) plugin build with the same settings as CI.
# Usage: scripts/build-plugin.sh [/path/to/UnityProject]
# With a Unity project, installs the dylib and pulls the Windows plugin from CI.
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="$repo_dir/build-plugin"
cmake -S "$repo_dir" -B "$build" -DCMAKE_BUILD_TYPE=Release -DSEMANTIC_PORTABLE=ON -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build "$build" --config Release --parallel
ctest --test-dir "$build" -C Release --output-on-failure
if [[ $# -gt 0 ]]; then
    "$repo_dir/scripts/sync-unity-plugins.sh" "$1" --local-macos "$build/libSemantic.Native.dylib"
fi
