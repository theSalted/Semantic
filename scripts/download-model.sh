#!/usr/bin/env bash
set -euo pipefail

# Official text-only Q8_0: https://huggingface.co/ggml-org/embeddinggemma-2-GGUF
# SHA-256 is the model's published LFS object hash at this pinned revision.
revision=bfcd298762cc34d0357ece5ebdd31791a3a374d8
checksum=2188ac1deca4b77dffefd603c2776a9d76d9d74ec01841392982ebb840b09135
filename=embeddinggemma-2-Q8_0.gguf
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
model_dir="${1:-"$repo_dir/models"}"
mkdir -p "$model_dir"
destination="$model_dir/$filename"

verify() { printf '%s  %s\n' "$checksum" "$1" | shasum -a 256 -c -; }
if [[ -e "$destination" ]]; then
    verify "$destination"
    exit 0
fi

download="$(mktemp "$model_dir/.embeddinggemma-2.XXXXXX")"
trap 'rm -f -- "$download"' EXIT
curl --fail --location --retry 3 \
    "https://huggingface.co/ggml-org/embeddinggemma-2-GGUF/resolve/$revision/$filename" \
    --output "$download"
verify "$download"
# Do not overwrite a file that appeared while the download was running.
ln "$download" "$destination"
printf 'Model ready: %s\n' "$destination"
