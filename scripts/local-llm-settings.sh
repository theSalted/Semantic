#!/bin/bash
# Shared shell configuration; C# identities are cross-checked by ModelConfigChecks.
semantic_configure_llm() {
  local model_root="$1"
  semantic_llm_size="${2:-4b}"
  case "$semantic_llm_size" in
    4b)
      semantic_llm_file=Qwen3-4B-Q4_K_M.gguf
      semantic_llm_alias=semantic-qwen3-4b-q4km
      semantic_llm_hash=7485fe6f11af29433bc51cab58009521f205840f5b4ae3a32fa7f92e8534fdf5
      semantic_llm_port=5194 ;;
    14b)
      semantic_llm_file=Qwen3-14B-Q4_K_M.gguf
      semantic_llm_alias=semantic-qwen3-14b-q4km
      semantic_llm_hash=500a8806e85ee9c83f3ae08420295592451379b4f8cf2d0f41c15dffeb6b81f0
      semantic_llm_port=5191 ;;
    *) echo '--llm-size must be 4b (default) or 14b.' >&2; return 2 ;;
  esac
  semantic_llm_path="${3:-$model_root/models/$semantic_llm_file}"
  [[ "$(basename "$semantic_llm_path")" == "$semantic_llm_file" ]] || { echo '--llm-model filename must match --llm-size.' >&2; return 2; }
}
semantic_print_llm_config() {
  printf '%s\n' "size=$semantic_llm_size" "file=$semantic_llm_file" "model=$semantic_llm_path" "alias=$semantic_llm_alias" "port=$semantic_llm_port" "sha256=$semantic_llm_hash"
}
semantic_verify_llm_model() {
  [[ -f "$semantic_llm_path" ]] || { echo "Missing $semantic_llm_path. Supply the verified model; no automatic download or fallback." >&2; return 1; }
  local actual_hash
  actual_hash="$(shasum -a 256 "$semantic_llm_path")"
  [[ "${actual_hash%% *}" == "$semantic_llm_hash" ]] || { echo 'Qwen model SHA256 mismatch.' >&2; return 1; }
}
