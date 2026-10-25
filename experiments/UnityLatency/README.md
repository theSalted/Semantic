# Unity full-completion latency probes

The measured **original Qwen3-4B Q4_K_M is now the normal default**, by explicit user choice after the in-game trial. It still has known dialogue quality failures. See [RESULTS.md](RESULTS.md) for timing boundaries and limitations and [the Unity host guide](../../unity-host/README.md) for normal startup, explicit 14B fallback and model provenance.

`run-4b-trial-mac.sh` remains as a compatibility wrapper around the normal launcher: it explicitly chooses 4B and accepts the existing external-model-path arguments. It no longer patches or copies runtime source. The old isolated preparation/patch and source hashes remain in Git commit `ba0fb87b92181d9d5ff4d17dc6415c152238f8a1` for reproducing that historical build. Its prompt, retrieval, memory and emote behavior are unchanged by promoting the model to the default.

Prerequisites remain the separate Unity project/current bank, .NET 10, Python 3, existing native library, llama.cpp server and embedding model. Supply the official [Qwen3-4B-Q4_K_M.gguf](https://huggingface.co/Qwen/Qwen3-4B-GGUF/blob/main/Qwen3-4B-Q4_K_M.gguf), 2,497,280,256 bytes, SHA256 `7485fe6f11af29433bc51cab58009521f205840f5b4ae3a32fa7f92e8534fdf5`. No model is automatically downloaded. This is not Instruct-2507.

Exit Play Mode before deliberately changing services. Ctrl-C stops services owned by the launcher. An occupied bridge port is refused; other launchers' model processes are not stopped. A running 4B game needs no restart for the new default to take effect on subsequent launches.

## Reproduce the focused workload

Use a dedicated bridge on 5193 instead of the game's 5192:

```sh
SEMANTIC_TRIAL_BRIDGE_PORT=5193 bash experiments/UnityLatency/run-4b-trial-mac.sh \
  /absolute/path/to/UnityProject /absolute/path/to/Qwen3-4B-Q4_K_M.gguf
# In a separate terminal, after readiness:
python3 experiments/UnityLatency/measure.py --label qwen4b --rounds 5 --port 5193 \
  --bank /absolute/path/to/UnityProject/Assets/StreamingAssets/Semantic/pell-cask.tsv \
  --output artifacts/unity-latency/qwen4b
python3 experiments/UnityLatency/quality.py --label qwen4b-quality --port 5193 \
  --bank /absolute/path/to/UnityProject/Assets/StreamingAssets/Semantic/pell-cask.tsv \
  --output artifacts/unity-latency/qwen4b-quality
```

The output directory must be fresh. Both probes reject any bank except the measured 33-row SHA256 `30971f18424c19ba1b5fcc2f30936c3eaf15ecee143ddbe67a4ea643e519aceb`: expected IDs are specific to this bank. A changed bank requires reviewing the expectations. Player facts in these probes are synthetic.

For a 14B comparison, stop the 4B launcher, use `unity-host/run-mac.sh /absolute/path/to/UnityProject --llm-size 14b` on 5192 and pass `--port 5192` with a fresh output directory. Do not run both models or other inference workloads concurrently. Start each comparison on a newly loaded model if comparing the separately reported first request. The probes do not load/restart servers themselves.

`measure.py` sends five 20-turn episodes with real rolling history, plus one separately reported first request and five retrieval controls. Actual LLM calls, schema-constrained memory calls, ordinary hybrid calls, generated replies and retrieval bypasses are reported separately. Timing begins before JSON serialization/send and ends after the complete bridge body is read and parsed. It excludes session creation, model loading and Unity rendering. Concurrency is one, with no retries. All attempts and errors are retained; failed attempts are not successful-completion latencies. An error or failed automatic check returns a nonzero exit status. Semantic checks include the authored-choice errors found in the post-run audit, not merely exact bytes for whichever line was chosen.

`quality.py` records 29 harder LLM turns and 24 fast history-fill turns, including negation, third-party/quoted identity, corrections, reset, forget, actual transcript eviction and unsupported world facts. Each output records a manual review requirement. A zero exit status only means automatic checks passed; complete replies still require semantic review. Neither script certifies a production p99 or general dialogue accuracy.

## Platform scope

This launcher and measured GPU configuration are macOS-only. The shared .NET bridge has platform branches, but this trial was not run on Windows and the shell launcher does not claim Windows support. Weights, binaries, raw transcripts, logs, temporary builds and unrelated model research are excluded from Git.
