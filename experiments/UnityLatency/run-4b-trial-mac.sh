#!/bin/bash
set -euo pipefail
trial_tools="$(cd "$(dirname "$0")" && pwd)"
semantic_root="$(cd "$trial_tools/../.." && pwd)"
if [[ $# -lt 2 || $# -gt 3 || ( $# == 3 && "$3" != --build-only ) ]]; then
  echo "Usage: bash experiments/UnityLatency/run-4b-trial-mac.sh /path/to/UnityProject /path/to/Qwen3-4B-Q4_K_M.gguf [--build-only]" >&2
  exit 2
fi
unity_project="$(cd "$1" && pwd)"
model_path="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
bridge_port="${SEMANTIC_TRIAL_BRIDGE_PORT:-5192}"
[[ "$bridge_port" =~ ^[0-9]+$ ]] && (( bridge_port >= 1024 && bridge_port <= 65535 && bridge_port != 5194 )) || { echo 'Invalid bridge port.' >&2; exit 2; }
bank="$unity_project/Assets/StreamingAssets/Semantic/pell-cask.tsv"
profile="$unity_project/Assets/StreamingAssets/Semantic/pell-cask.profile.json"
server="$semantic_root/build-llm/bin/llama-server"
[[ -f "$bank" && -f "$profile" && -x "$server" && -f "$semantic_root/build/libSemantic.Native.dylib" && -f "$semantic_root/models/embeddinggemma-2-Q8_0.gguf" ]] || { echo 'Missing existing bank/profile, native library, llama server or embedding model; see README.' >&2; exit 1; }
if [[ "${3:-}" != --build-only ]]; then
  for trial_port in "$bridge_port" 5194; do
    if /usr/sbin/lsof -nP -iTCP:"$trial_port" -sTCP:LISTEN >/dev/null 2>&1; then
      echo "Port $trial_port already in use. Leave its owner untouched; stop it deliberately before switching models." >&2; exit 1
    fi
  done
fi
semantic_dotnet="$(command -v dotnet || true)"
semantic_dotnet="${semantic_dotnet:-/usr/local/share/dotnet/dotnet}"
trial_dir="$(python3 "$trial_tools/prepare_trial.py" --root "$semantic_root" --output "$semantic_root/artifacts/unity-4b-trial" --model "$model_path")"
"$semantic_dotnet" build "$trial_dir/unity-host/Semantic.UnityHost.csproj" -c Release --nologo --artifacts-path "$trial_dir/build"
echo "Prepared isolated trial in $trial_dir"
[[ "${3:-}" != --build-only ]] || exit 0
qwen_pid=""; bridge_pid=""
cleanup() {
  trap - EXIT INT TERM
  [[ -z "$bridge_pid" ]] || { kill "$bridge_pid" 2>/dev/null || true; wait "$bridge_pid" 2>/dev/null || true; }
  [[ -z "$qwen_pid" ]] || { kill "$qwen_pid" 2>/dev/null || true; wait "$qwen_pid" 2>/dev/null || true; }
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
"$server" -m "$model_path" --host 127.0.0.1 --port 5194 --alias semantic-qwen3-4b-q4km \
  -c 8192 -np 1 -ngl 99 -t 6 --reasoning off --jinja --no-webui > "$trial_dir/qwen.log" 2>&1 &
qwen_pid=$!
ready=0
for ((i=0; i<60; i++)); do
  if curl -fsS --max-time 1 http://127.0.0.1:5194/health >/dev/null 2>&1; then ready=1; break; fi
  kill -0 "$qwen_pid" 2>/dev/null || { echo "Qwen failed; see $trial_dir/qwen.log" >&2; exit 1; }
  sleep 1
done
[[ "$ready" == 1 ]] || { echo 'Qwen readiness timed out.' >&2; exit 1; }
"$semantic_dotnet" "$trial_dir/build/bin/Semantic.UnityHost/release/Semantic.UnityHost.dll" \
  --semantic-root "$semantic_root" --llm-model "$model_path" --bank "$bank" --character-profile "$profile" \
  --port "$bridge_port" > "$trial_dir/bridge.log" 2>&1 &
bridge_pid=$!
ready=0
for ((i=0; i<60; i++)); do
  if curl -fsS --max-time 1 "http://127.0.0.1:$bridge_port/health" >/dev/null 2>&1; then ready=1; break; fi
  kill -0 "$bridge_pid" 2>/dev/null || { echo "Bridge failed; see $trial_dir/bridge.log" >&2; exit 1; }
  sleep 1
done
[[ "$ready" == 1 ]] || { echo 'Bridge readiness timed out.' >&2; exit 1; }
echo "TEMPORARY 4B TRIAL: known dialogue quality failures. 14B defaults remain unchanged."
echo "Ready: http://127.0.0.1:$bridge_port ; logs: $trial_dir ; Ctrl-C stops only these owned services."
echo 'Use the existing Playground scene. Exit Play Mode before switching back to the normal unity-host/run-mac.sh launcher.'
wait "$bridge_pid"
