#!/bin/bash
set -euo pipefail
semantic_root="$(cd "$(dirname "$0")/../.." && pwd)"
if [[ $# -lt 2 || $# -gt 3 || ( $# == 3 && "$3" != --build-only ) ]]; then
  echo 'Usage: run-4b-trial-mac.sh /path/to/UnityProject /path/to/Qwen3-4B-Q4_K_M.gguf [--build-only]' >&2
  exit 2
fi
exec bash "$semantic_root/unity-host/run-mac.sh" "$1" --llm-size 4b --llm-model "$2" \
  --port "${SEMANTIC_TRIAL_BRIDGE_PORT:-5192}" "${@:3}"
