#!/bin/bash
set -euo pipefail
semantic_root="$(cd "$(dirname "$0")/.." && pwd)"
source "$semantic_root/scripts/local-llm-settings.sh"
llm_size=4b; llm_path=""; print_config=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --llm-size|--llm-model) [[ $# -ge 2 ]] || { echo "Missing value for $1" >&2; exit 2; }; if [[ "$1" == --llm-size ]]; then llm_size="$2"; else llm_path="$2"; fi; shift 2 ;;
    --print-config) print_config=1; shift ;;
    *) echo 'Usage: scripts/run-local-llm.sh [--llm-size 4b|14b] [--llm-model /path/to/model.gguf] [--print-config]' >&2; exit 2 ;;
  esac
done
semantic_configure_llm "$semantic_root" "$llm_size" "$llm_path"
if [[ "$print_config" == 1 ]]; then semantic_print_llm_config; exit 0; fi
if /usr/sbin/lsof -nP -iTCP:"$semantic_llm_port" -sTCP:LISTEN >/dev/null 2>&1; then
  echo "Model port $semantic_llm_port is occupied; refusing a competing server." >&2; exit 1
fi
semantic_verify_llm_model
exec "$semantic_root/build-llm/bin/llama-server" \
  -m "$semantic_llm_path" --host 127.0.0.1 --port "$semantic_llm_port" --alias "$semantic_llm_alias" \
  -c 8192 -np 1 -ngl 99 -t 6 --reasoning off --jinja --no-webui
