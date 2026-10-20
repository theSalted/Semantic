#!/bin/bash
set -euo pipefail
semantic_root="$(cd "$(dirname "$0")/.." && pwd)"
exec "$semantic_root/build-llm/bin/llama-server" \
  -m "$semantic_root/models/Qwen3-14B-Q4_K_M.gguf" \
  --host 127.0.0.1 --port 5191 --alias semantic-qwen3-14b-q4km \
  -c 8192 -np 1 -ngl 99 -t 6 --reasoning off --jinja --no-webui
