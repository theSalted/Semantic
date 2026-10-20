#!/bin/bash
set -euo pipefail
semantic_root="$(cd "$(dirname "$0")/.." && pwd)"
unity_project="${1:?Usage: unity-host/run-mac.sh /absolute/path/to/UnityProject}"
bank="$unity_project/Assets/StreamingAssets/Semantic/pell-cask.tsv"
profile="$unity_project/Assets/StreamingAssets/Semantic/pell-cask.profile.json"
[[ -f "$bank" && -f "$profile" ]] || { echo "Missing Pell bank/profile in Unity project." >&2; exit 1; }
semantic_dotnet="$(command -v dotnet || true)"
semantic_dotnet="${semantic_dotnet:-/usr/local/share/dotnet/dotnet}"
cd "$semantic_root"
"$semantic_dotnet" build unity-host/Semantic.UnityHost.csproj -c Release --nologo
qwen_pid=""
bridge_pid=""
cleanup() {
  trap - EXIT INT TERM
  if [[ -n "$bridge_pid" ]]; then kill "$bridge_pid" 2>/dev/null || true; wait "$bridge_pid" 2>/dev/null || true; fi
  if [[ -n "$qwen_pid" ]]; then kill "$qwen_pid" 2>/dev/null || true; wait "$qwen_pid" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if curl -fsS --max-time 2 http://127.0.0.1:5192/health >/dev/null 2>&1; then
  echo "A bridge already uses port 5192. Reuse it if its bank matches, or stop its owner first." >&2; exit 1
fi
if ! curl -fsS --max-time 2 http://127.0.0.1:5191/health >/dev/null 2>&1; then
  "$semantic_root/scripts/run-local-llm.sh" &
  qwen_pid=$!
fi
ready=0
for ((i=0; i<120; i++)); do
  if curl -fsS --max-time 1 http://127.0.0.1:5191/health >/dev/null 2>&1; then ready=1; break; fi
  if [[ -n "$qwen_pid" ]] && ! kill -0 "$qwen_pid" 2>/dev/null; then echo "Qwen failed to start." >&2; exit 1; fi
  sleep 1
done
[[ "$ready" == 1 ]] || { echo "Qwen did not become ready." >&2; exit 1; }
echo "Unity hybrid: http://127.0.0.1:5192. Ctrl-C stops this bridge and Qwen only if this launcher started it."
"$semantic_dotnet" "$semantic_root/unity-host/bin/Release/net10.0/Semantic.UnityHost.dll" \
  --semantic-root "$semantic_root" --bank "$bank" --character-profile "$profile" --port 5192 &
bridge_pid=$!
wait "$bridge_pid"
