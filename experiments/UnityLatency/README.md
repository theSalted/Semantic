# Opt-in Unity 4B latency trial

This packages the measured **original Qwen3-4B Q4_K_M** candidate without changing the production 14B default. The 4B trial is fast on short replies but **has known dialogue quality failures**. It is available for in-game evaluation, not approved as a production replacement. See [RESULTS.md](RESULTS.md) for measured boundaries and limitations.

## Prerequisites and provenance

Use the existing setup described in [the Unity host guide](../../unity-host/README.md): macOS, .NET 10, Python 3, system `patch`, `build/libSemantic.Native.dylib`, `build-llm/bin/llama-server`, `models/embeddinggemma-2-Q8_0.gguf`, and the separate Unity 6000.6.2f1 project with its current bank/profile. This Git repository does not contain the Unity project or its authored bank.

The additional model is the official [Qwen3-4B-Q4_K_M.gguf](https://huggingface.co/Qwen/Qwen3-4B-GGUF/blob/main/Qwen3-4B-Q4_K_M.gguf): 2,497,280,256 bytes; SHA256 `7485fe6f11af29433bc51cab58009521f205840f5b4ae3a32fa7f92e8534fdf5`. Supply an already downloaded file with that exact filename. The scripts never download weights or install dependencies. This is not Qwen3-4B-Instruct-2507.

## Try in the existing game

Exit Play Mode and stop the previous model/bridge deliberately before switching. Do not stop someone else's active request. From the Semantic repository root:

```sh
bash experiments/UnityLatency/run-4b-trial-mac.sh \
  /absolute/path/to/UnityProject /absolute/path/to/Qwen3-4B-Q4_K_M.gguf
```

The launcher verifies model and source hashes, creates an isolated copy under ignored `artifacts/unity-4b-trial/`, applies [4b-model.patch](4b-model.patch), builds it, starts the model on loopback 5194 and bridge on 5192, and waits for health. It refuses occupied ports rather than replacing existing services. Fresh run directories preserve diagnostics; they can be removed after their launcher has stopped. `--build-only` prepares and compiles without starting services or contacting the live bridge.

Only model hash/alias/path and model endpoint change. Prompt, 12-candidate shortlist, memory, authored/emote validation, reset/cancellation and fast-path logic remain identical to the measured source. [source-manifest.json](source-manifest.json) pins those inputs to framework commit `962f5c6cc4ce02baf4b740022c92438b4a82368f`. A source mismatch stops preparation; review/revalidate before updating the manifest. It is deliberately an isolated trial, not a general model-switching API.

Open the existing `Assets/Scenes/Playground.unity` and enter Play Mode. Use **Your reply...**, then **Reply** or Enter. **New conversation** clears its history and explicit facts. Keep the launcher terminal open. Ctrl-C stops only the bridge and model that this launcher owns.

To restore 14B, exit Play Mode, Ctrl-C the trial launcher, then run:

```sh
bash unity-host/run-mac.sh /absolute/path/to/UnityProject
```

Enter Play Mode again after readiness. No Unity source, scene or bank settings need restoring. A service switch loses in-memory conversations. Do not run measurement scripts while someone is testing in-game.

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

For a 14B comparison, stop the 4B launcher, use the normal 14B launcher on 5192 and pass `--port 5192` with a fresh output directory. Do not run both models or other inference workloads concurrently. Start each comparison on a newly loaded model if comparing the separately reported first request. The probes do not load/restart servers themselves.

`measure.py` sends five 20-turn episodes with real rolling history, plus one separately reported first request and five retrieval controls. Actual LLM calls, schema-constrained memory calls, ordinary hybrid calls, generated replies and retrieval bypasses are reported separately. Timing begins before JSON serialization/send and ends after the complete bridge body is read and parsed. It excludes session creation, model loading and Unity rendering. Concurrency is one, with no retries. All attempts and errors are retained; failed attempts are not successful-completion latencies. An error or failed automatic check returns a nonzero exit status. Semantic checks include the authored-choice errors found in the post-run audit, not merely exact bytes for whichever line was chosen.

`quality.py` records 29 harder LLM turns and 24 fast history-fill turns, including negation, third-party/quoted identity, corrections, reset, forget, actual transcript eviction and unsupported world facts. Each output records a manual review requirement. A zero exit status only means automatic checks passed; complete replies still require semantic review. Neither script certifies a production p99 or general dialogue accuracy.

## Platform scope

This launcher and measured GPU configuration are macOS-only. The shared .NET bridge has platform branches, but this trial was not run on Windows and the shell launcher does not claim Windows support. Weights, binaries, raw transcripts, logs, temporary builds and unrelated model research are excluded from Git.
