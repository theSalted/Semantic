# Hybrid mode

Semantic has two modes behind one C API. The mode is chosen by what `Semantic_CreateMatcher(model, bank)` receives as `model`:

| `model` | Mode | Behavior |
|---|---|---|
| embedding `.gguf` | retrieval | Stateless: returns the nearest authored row. |
| `.json` config with `"mode": "hybrid"` | hybrid | One conversation per matcher. Retrieves a latest-input shortlist, then a local chat model either selects an authored row verbatim or generates an untagged reply. |

`Semantic_Match` and `Semantic_MatchWithTag` keep their shape in both modes. In hybrid mode, a selected row returns its tag and similarity, and a generated reply returns an empty tag and score 0. A failed turn is not added to the conversation. To start a new conversation, create a new matcher. The chat model is shared, so a replacement matcher is ready in about a second.

## Config

```json
{
  "mode": "hybrid",
  "embedding_model": "embeddinggemma-2-Q8_0.gguf",
  "llm_model": "Qwen3-4B-Q4_K_M.gguf",
  "llm_url": "https://huggingface.co/Qwen/Qwen3-4B-GGUF/resolve/<commit>/Qwen3-4B-Q4_K_M.gguf",
  "profile": "character.profile.json",
  "candidates": 12,
  "session_memory": true,
  "gpu_layers": 99,
  "fast_path": { "enabled": true, "min_similarity": 0.99, "min_margin": 0.06, "eligible_prompts": ["..."] }
}
```

- **Paths:** relative paths resolve against the config file.
- **Chat model:** if `llm_model` is not on disk, it is downloaded once from `llm_url` into the per-user cache: `~/Library/Caches/Semantic/models` on macOS, `%LOCALAPPDATA%\Semantic\models` on Windows. Interrupted downloads resume, and the file appears under its final name only when complete. Pin `llm_url` to a commit.
- **Chat format:** the chat model must use ChatML, as Qwen3 does. Decoding is greedy and grammar-constrained to `{"selected_id": ...}` or `{"response": ..., "used_fact_ids": [...]}`, then validated against the shortlist and the profile/player fact IDs.
- **Player memory (`session_memory`):** keeps a small set of explicit player facts per conversation. Supported recall questions get an exact, grammar-forced answer.
- **Fast path:** answers reviewed exact triggers without the chat model when both cosine gates pass.

## Prepare before the first turn

```c
Semantic_Prepare(config, bank);             // returns immediately; work runs on a worker thread
Semantic_PrepareStatus(config, bank, &progress, &stage);  // 1 running, 2 ready, -1 failed
```

Preparation downloads the chat model if it is missing, loads both models and embeds the bank. The next `Semantic_CreateMatcher` with the same paths takes the prepared matcher instantly; if preparation is still running, it waits for it. A prepared chat model stays loaded for the life of the process. Call `Semantic_Prepare` again after a failure to retry; a partial download resumes.

## Platforms

- **macOS (Apple silicon):** one static dylib. The chat model runs on Metal; the embedding model stays on the CPU, so retrieval results are unchanged.
- **Windows x64:** `Semantic.Native.dll` plus llama.cpp's official prebuilt release DLLs (tag `b11560`, SHA-256 verified): `llama.dll`, `ggml*.dll`, all CPU variants, Vulkan and CUDA with its runtime (CUDA adds ~1.15 GB; configure with `-DSEMANTIC_CUDA=OFF` to leave it out). Only `Semantic.Native.dll` is compiled. Backends load at runtime from the plugin folder: CUDA (NVIDIA) or Vulkan (any GPU) when available, otherwise the fastest CPU variant. If a GPU lacks the memory for the whole model, lower `gpu_layers`.

## Building and CI

`.github/workflows/native.yml` compiles both platforms when native sources change and runs only the model-free checks; nothing is downloaded. It publishes the plugins as artifacts. CUDA and Vulkan are compile-checked only. Model-backed checks (retrieval, prepare, download, hybrid smoke) run locally: `ctest` with the embedding model present, and `Semantic.HybridSmoke` with `SEMANTIC_HYBRID_CONFIG`/`SEMANTIC_HYBRID_BANK` for hybrid turns.

- `scripts/sync-unity-plugins.sh <UnityProject>` installs the latest green plugins for both platforms, with stable `.meta` files.
- `scripts/build-plugin.sh [UnityProject]` builds and tests locally on macOS.
- `scripts\build-plugin.ps1 [-UnityProject ...] [-NoCuda]` builds and tests locally on Windows; it needs only Visual Studio 2022.
