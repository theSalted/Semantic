# Unity hybrid dialogue

This local bridge reuses `temp/LocalMethods.cs`, `PhraseBank.cs`, `CatalogData.cs` and the current native session API. It runs only the hybrid method, on loopback port 5192, without exposing bank-editing routes. It reads the Unity project's existing `pell-cask.tsv`; its three columns and authored spelling remain unchanged. A temporary two-column projection feeds retrieval, and selected row IDs resolve back to exact original text/emote tags. The client checks the source bank SHA256 and exact selected row before displaying it. The scene, cube, subtitles and original `CubeMood` script are not regenerated.

## Run on macOS

From the Semantic repository root:

```sh
unity-host/run-mac.sh /absolute/path/to/UnityProject
```

This repository contains the framework and service, not the separate Unity project, scene, client components, authored bank or character profile. The paired Unity project must already contain the described `SemanticDialogue`/`HybridDialogueClient` components and `Assets/StreamingAssets/Semantic/pell-cask.tsv` plus `pell-cask.profile.json`. The test entrypoints below also belong to that separate project.

Before launching, provide a current native build at `build/libSemantic.Native.dylib`, the installed .NET 10 SDK, a compatible llama.cpp server at `build-llm/bin/llama-server`, and the existing embedding and Qwen model files under `models/`. Binaries and weights are not included. The launcher does not provision these prerequisites.

The launcher builds the host using the installed .NET 10 SDK, reuses the existing verified Qwen3-14B Q4_K_M model and `scripts/run-local-llm.sh`, waits for Qwen, and serves the bridge. No model is downloaded. Open the existing Unity project with 6000.6.2f1, open `Assets/Scenes/Playground.unity`, and press Play. `SemanticDialogue` on Canvas defaults to **Use Hybrid**, with endpoint `http://127.0.0.1:5192/`. Send using Reply or Enter. The original greeting is displayed locally; it is not inserted as an invented user exchange. **New conversation** cancels in-flight work, closes the old session, creates a new one and restores that greeting.

Keep the launcher terminal open during use. Ctrl-C stops its bridge and stops Qwen only if this launcher started Qwen. A previously running Qwen process is reused and never killed by this launcher. Play Mode exit closes its conversation on a best-effort basis; the service expires abandoned idle sessions after 30 minutes. Exiting Unity does not kill a shared service. Stop the launcher when finished to release model memory. Do not run competing local model workloads during use.

If the service is unavailable, the current subtitle shows an explicit error and input controls recover. A failed send keeps its input for retry and abandons the uncertain conversation rather than silently replaying a potentially committed turn. Restart the launcher and send again, or click New conversation. There is no automatic network retry and no hidden fallback to the legacy matcher. Changing the source bank requires restarting the bridge and conversation. An expired/restarted session reports one failure, then the next send reconnects with fresh history.

To use the previous offline matcher deliberately, clear **Use Hybrid** on Canvas before Play. It retains the original bundled native plugin and embedding model. Runtime mode switching is not supported.

## Temporary 4B trial

The production launcher above still uses 14B. An explicitly opt-in [4B trial and reproducible latency probes](../experiments/UnityLatency/README.md) build an isolated copy without editing Unity assets or the production host. The measured 4B candidate is faster on short replies but has known dialogue quality failures; it is not a production-approved replacement. The guide includes setup, model hash, measured limits and rollback.

## Behavior and limits

Latest-input-only embedding retrieval supplies up to 12 authored candidates. Unless the conservative fast path below passes, Qwen receives those candidates, bounded **actual** user/character exchanges and `Assets/StreamingAssets/Semantic/pell-cask.profile.json`. The small profile derives name, bar ownership, drinks and duty behavior from the existing bank; it supplies no invented era. Authored selections preserve exact text and tags. Generated replies have null authored ID/tag and reset the cube's prior color to neutral without claiming an authored Neutral tag.

The shared schema enforces separate select and generate branches. Its fit instruction requires an authored reply to answer the whole current request consistently with history. The existing runtime checks still reject invalid IDs/text, unknown profile fact IDs and incomplete decisions. These controls do not guarantee semantic accuracy or factuality of generated dialogue. The first integration runs exposed a mixed decision and a vague authored memory disclaimer; raw evidence was retained locally and is not distributed with this repository.

Framework history remains bounded to 512 EmbeddingGemma tokens and at most 16 complete exchanges. Qwen's complete prompt is additionally bounded by its real tokenizer/chat template to an 8192-token context, reserving 512 output tokens. Older complete exchanges, then low-ranked candidates, may be removed. This rolling transcript is separate from the bounded explicit player facts described below. Both are in-memory and disappear when the conversation is closed, expires, or the bridge restarts.

## Latency and conservative retrieval path

The Unity host opts into compact Qwen decisions: an authored selection emits only `selected_id`; generated output contains `response` and `used_fact_ids`. The shared implementation normalizes this to the original decision contract. Other callers retain the original format by default. Prompt JSON avoids unnecessary Unicode escaping while preserving the same decoded text. The model, 12-candidate retrieval, history limits, 8192-token context and 512-token output ceiling are unchanged. Thinking was already disabled; the ceiling is not a requirement to generate 512 tokens. Qwen used all 41 GPU layers on the tested Mac and its existing prefix cache remains enabled.

`unity-host/fast-path.json` controls `enabled`, `minSimilarity`, `minMargin`, and the reviewed `eligiblePrompts`. Defaults require all of:

- An exact normalized match to a reviewed standalone trigger (case, whitespace, curly apostrophe and final punctuation normalization only).
- Cosine similarity at least **0.99**, plus a **0.06** lead over the next distinct authored response. These are score gates, not probabilities or a claim of 99% confidence.
- An unambiguous trigger mapping, including its original emote tag, and a retrieval winner agreeing with that mapping.
- No detected negation, correction, quoted input, memory/name query, reference or relevant negation/allergy in retained user history. Unreviewed short/generic/contextual utterances stay on Qwen.

On the current 33-row bank, six exact triggers pass: whiskey, wine, water, milk, drink price and alcohol origin. Beer and ale remain on Qwen because their distinct-response margin is only 0.035. Fifteen focused near-miss/negation/reference probes all stayed on Qwen. This deliberately does not accept arbitrary semantic paraphrases. These probes are not a universal semantic-safety guarantee; the historical guard is conservative and lexical, and only retained transcript history is inspected. Review any new trigger list against the actual bank before enabling it. Set `enabled` false and restart the bridge to disable bypass.

Fast replies preserve the exact authored text and tag, and commit to the same history and player-memory transaction as Qwen replies. No model call is made on this path. Responses expose `metadata.path` (`retrieval-fast`, `qwen-hybrid`, or `qwen-memory`), `modelInvoked`, gate reason/scores, retrieval/commit/queue timings, completion timing and llama prompt/decode/cache timings. `SemanticDialogue.LastHybridReply` exposes the path and `LastResponseJson` preserves full diagnostics; the visible UI remains the existing single subtitle.

## Explicit conversation facts

The Unity host keeps at most seven explicit player facts, separately from rolling dialogue: name, favorite drink/food/color, where the player lives, where they are from, and a lost item. For example, use `My name is Mira.`, `Actually, call me Jun.`, or `My favorite drink is tea.` Corrections replace the old value. `Forget my name.` clears it; explicitly denying the currently stored name removes it without inventing a replacement.

Extraction is a small local parser, not a second model pass. It accepts clear first-person declaration forms, bounds values and rejects quotes, third-party attribution, hypotheticals and ambiguous forms such as `I'm tired.` It intentionally does not understand every paraphrase. It never learns player identity from the character's replies. The current input's prospective facts are supplied to Qwen immediately, then committed only if that turn successfully commits. Preview, failed, cancelled or stale replies cannot commit facts.

Recognized direct recall questions (such as `What's my name?`) and standalone name corrections use a fact-constrained generated reply through Qwen. Unknown facts receive an explicit “you haven't told me” response. An authored reply is eligible only if its exact text answers that fact; authored text is never rewritten and relabeled. These direct answers are reliable because of the explicit store and response constraint, not merely a prompt instruction. Broader follow-ups still use Qwen with the current ledger and actual history, so their wording and correctness remain model dependent. Generated answers retain null authored ID/emote.

**New conversation** clears both history and facts, including during an in-flight request. Memory is scoped to that service conversation, has no cross-session disk persistence, and is lost on service restart or an uncertain-network recovery that abandons the old conversation. Test evidence contains synthetic player facts only.

## Verification

The opt-in `HybridPlayModeChecks.Run` editor entrypoint runs the current scene in real Play Mode. It checks fast-path exact selection without Qwen, unknown/corrected/reset name memory, exact Qwen authored selection/emote/color, generated fallback without an authored tag, actual generated text in follow-up history, a contextual answer, reset during a pending turn, fresh post-reset history, and real service outage recovery. It never runs in ordinary Play Mode. The verifier uses `SEMANTIC_UNITY_EVIDENCE` for logs/results; after writing `ready-for-outage`, stop the owned launcher and create `host-stopped` in that folder. The existing `ProjectSetup.VerifyPlayMode` and `DialogueVerification` explicitly select legacy mode for the old 33-row regression checks.

```sh
SEMANTIC_UNITY_EVIDENCE=/absolute/evidence/folder \
  /Applications/Unity/Hub/Editor/6000.6.2f1/Unity.app/Contents/MacOS/Unity \
  -projectPath /absolute/UnityProject \
  -executeMethod HybridPlayModeChecks.Run -logFile /absolute/evidence/folder/unity.log
```

Focused final Mac checks passed: 43 live Play Mode assertions, 53 deterministic memory/fast-path checks, and the existing 27 method/session plus 36 catalog/key checks. The earlier 229-assertion legacy Play Mode regression remains valid; the follow-up did not change the legacy Unity path. These are recorded local integration results. The 53-check memory/fast-path harness, Unity project verifiers and raw evidence are not included in this framework release. The repository includes the 27 method/session checks and 35 self-contained catalog checks; a 36th catalog check runs only when its optional local projection fixture is supplied. A single small cache-disabled probe reduced authored model selection from 1.68 s to 1.01 s and contextual generation from 2.19 s to 1.40 s; longer generated output remained about 3 s. An exact whiskey request changed from 800 ms through the old service to 32 ms through the new service; the final actual Unity measurement was 31 ms. These are individual integration observations, not statistical performance guarantees.

No comparative benchmark, unfinished Chandler bank or trained paper method is used by this bridge.

## Windows

The Unity client uses portable C# / HttpClient and does not require an updated native plugin in hybrid mode. The .NET host accepts `--native-library /path/to/Semantic.Native.dll`, and its default library name accommodates Windows. However, Windows needs a **current source-built** native library with the new session APIs, all required native dependencies, .NET 10, and a separately configured Windows llama.cpp server with alias `semantic-qwen3-14b-q4km`, port 5191, 8192 context and one slot. The existing Unity Windows DLL is for the legacy matcher and is not asserted to support the new host. `run-mac.sh` and the existing Metal launcher are Mac-only. No Windows build or Windows execution is claimed.
