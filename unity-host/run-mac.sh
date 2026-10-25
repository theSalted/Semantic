#!/bin/bash
set -euo pipefail
semantic_root="$(cd "$(dirname "$0")/.." && pwd)"
source "$semantic_root/scripts/local-llm-settings.sh"
unity_project="${1:?Usage: unity-host/run-mac.sh /absolute/path/to/UnityProject [--llm-size 4b|14b] [--llm-model /path/to/model.gguf] [--port 5192] [--build-only|--print-config]}"
shift
llm_size=4b; llm_path=""; bridge_port=5192; build_only=0; print_config=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --llm-size|--llm-model|--port)
      [[ $# -ge 2 ]] || { echo "Missing value for $1" >&2; exit 2; }
      case "$1" in --llm-size) llm_size="$2" ;; --llm-model) llm_path="$2" ;; --port) bridge_port="$2" ;; esac
      shift 2 ;;
    --build-only) build_only=1; shift ;;
    --print-config) print_config=1; shift ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done
semantic_configure_llm "$semantic_root" "$llm_size" "$llm_path"
[[ "$bridge_port" =~ ^[1-9][0-9]{3,4}$ ]] && (( bridge_port <= 65535 && bridge_port != semantic_llm_port )) || { echo 'Invalid bridge port.' >&2; exit 2; }
if [[ "$print_config" == 1 ]]; then semantic_print_llm_config; printf '%s\n' "bridgePort=$bridge_port"; exit 0; fi
unity_project="$(cd "$unity_project" && pwd)"
bank="$unity_project/Assets/StreamingAssets/Semantic/pell-cask.tsv"
profile="$unity_project/Assets/StreamingAssets/Semantic/pell-cask.profile.json"
[[ -f "$bank" && -f "$profile" ]] || { echo 'Missing Pell bank/profile in Unity project.' >&2; exit 1; }
if [[ "$build_only" != 1 ]] && /usr/sbin/lsof -nP -iTCP:"$bridge_port" -sTCP:LISTEN >/dev/null 2>&1; then
  echo "A service already uses bridge port $bridge_port. Leave its conversation intact; stop its owner deliberately before switching." >&2; exit 1
fi
[[ "$build_only" == 1 ]] || semantic_verify_llm_model
semantic_dotnet="$(command -v dotnet || true)"
semantic_dotnet="${semantic_dotnet:-/usr/local/share/dotnet/dotnet}"
cd "$semantic_root"
"$semantic_dotnet" build unity-host/Semantic.UnityHost.csproj -c Release --nologo
[[ "$build_only" != 1 ]] || exit 0
qwen_pid=""; bridge_pid=""
cleanup() {
  trap - EXIT INT TERM
  [[ -z "$bridge_pid" ]] || { kill "$bridge_pid" 2>/dev/null || true; wait "$bridge_pid" 2>/dev/null || true; }
  [[ -z "$qwen_pid" ]] || { kill "$qwen_pid" 2>/dev/null || true; wait "$qwen_pid" 2>/dev/null || true; }
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if ! /usr/sbin/lsof -nP -iTCP:"$semantic_llm_port" -sTCP:LISTEN >/dev/null 2>&1; then
  bash "$semantic_root/scripts/run-local-llm.sh" --llm-size "$semantic_llm_size" --llm-model "$semantic_llm_path" &
  qwen_pid=$!
fi
ready=0
for ((i=0; i<120; i++)); do
  if curl -fsS --max-time 1 "http://127.0.0.1:$semantic_llm_port/health" >/dev/null 2>&1; then ready=1; break; fi
  if [[ -n "$qwen_pid" ]] && ! kill -0 "$qwen_pid" 2>/dev/null; then echo 'Qwen failed to start.' >&2; exit 1; fi
  sleep 1
done
[[ "$ready" == 1 ]] || { echo 'Qwen did not become ready.' >&2; exit 1; }
# A pre-existing process must identify as the selected model; never relabel it.
curl -fsS --max-time 3 "http://127.0.0.1:$semantic_llm_port/v1/models" | python3 -c \
  'import json,sys; sys.exit(0 if any(x["id"] == sys.argv[1] for x in json.load(sys.stdin)["data"]) else "Qwen model alias mismatch")' "$semantic_llm_alias"
echo "Unity hybrid uses $semantic_llm_file at http://127.0.0.1:$bridge_port. Ctrl-C stops only services owned by this launcher."
"$semantic_dotnet" "$semantic_root/unity-host/bin/Release/net10.0/Semantic.UnityHost.dll" \
  --semantic-root "$semantic_root" --bank "$bank" --character-profile "$profile" --port "$bridge_port" \
  --llm-size "$semantic_llm_size" --llm-model "$semantic_llm_path" &
bridge_pid=$!
wait "$bridge_pid"
