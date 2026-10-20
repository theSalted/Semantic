#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

if command -v dotnet >/dev/null 2>&1; then
  semantic_dotnet="$(command -v dotnet)"
else
  semantic_dotnet="/usr/local/share/dotnet/dotnet"
fi

if [[ ! -f ../build/libSemantic.Native.dylib ]]; then
  echo "Build the native matcher first: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel 4" >&2
  exit 1
fi
if [[ ! -f ../models/embeddinggemma-2-Q8_0.gguf ]]; then
  echo "Download the model first with scripts/download-model.sh from the project root." >&2
  exit 1
fi

export DOTNET_CLI_TELEMETRY_OPTOUT=1
exec "$semantic_dotnet" run --project Semantic.Playground.csproj --configuration Release --no-launch-profile -- "$@"
