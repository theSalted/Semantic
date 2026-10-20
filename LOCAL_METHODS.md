# Local method integration

The playground adapters preserve explicit framework sessions. The checks below verify implementation plumbing, not conversation quality. The framework and Unity host are included; optional paper-model implementations, training jobs, datasets, model weights and raw evaluation artifacts are not part of this release. See `unity-host/README.md` for the hybrid Unity service and its external project requirements.

## Run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSEMANTIC_BUILD_INTEROP_SMOKE=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
dotnet build temp/Semantic.Playground.csproj -c Release
scripts/run-local-llm.sh
# In another terminal:
temp/run.sh --bank "$PWD/experiments/phrases.tsv" --port 5187
```

The commands assume the embedding and Qwen model files and the separate llama.cpp server build have already been provisioned. The included `experiments/phrases.tsv` is a small example bank. The local LIGHT bank and optional paper-service launchers are excluded; substitute an explicitly supplied bank or start your separately installed adapter service when needed. Without a paper service, its methods report unavailable.

Open <http://localhost:5187>. Refresh methods starts a new conversation and rediscovers live adapters. All available methods receive the same input, with separate ordered queues and histories. New conversation cancels pending work and invalidates late commits. Save comparison exports complete browser transcripts, errors, response provenance, IDs, candidates, prompt metadata and model identities. Transcripts remain in this page's memory until exported; reloading the page loses them.

The framework's history budget is **512 EmbeddingGemma tokens**, including role labels and task prefix, and at most 16 complete exchanges. This bound applies to all methods for this integration. Older exchanges and an individually oversized exchange are omitted whole; exports retain the browser transcript. LLM adapters additionally apply Qwen's real chat template and tokenizer to the complete request, reserving output tokens inside `min(8192, server n_ctx)`. They evict oldest complete exchanges, then lowest-ranked candidates, before rejecting a request that still cannot fit. These are different tokenizers and limits; 512 retained embedding tokens do not mean 512 Qwen prompt tokens.

## Methods

| ID | Behavior |
|---|---|
| input/history/recent/blend | Existing authored-response embedding selectors. Blend is 70% current input, 30% last exchange. |
| hybrid | Latest-input-only embedding retrieval of 12 candidates; Qwen chooses an exact stored response ID or explicitly generates a response using the bounded actual history and optional profile. |
| llm_id | Qwen selects an exact authored ID from latest-input top 32. It is candidate-scoped, not full-bank LLM ranking. |
| rewrite | Qwen receives the latest input and actual history, with no candidate answers, and returns a standalone query. Input-only embedding retrieval chooses the authored response. This is a prompted adaptation, not trained CONQRR. |
| light_biranker/poly_encoder/smn | Stateless paper service ranks the full supplied response bank. Exact trained checkpoint, preprocessing, truncation and port metadata come from that service. |

Paper-method availability comes from the separately installed loopback service at runtime; the source names in the table do not imply that trained checkpoints are included or ready. No substitute model fills a missing method. `METHOD_ADAPTER.md` specifies the optional service contract. Training/release evidence from local paper-method experiments is excluded from this framework release.

Qwen is the installed official `Qwen3-14B-Q4_K_M.gguf`, SHA256 `500a8806e85ee9c83f3ae08420295592451379b4f8cf2d0f41c15dffeb6b81f0`. The server alias is `semantic-qwen3-14b-q4km`, loopback port 5191, Metal, one slot, 8192 context tokens, temperature 0, seed 19 and thinking disabled. Maximum output is 512 tokens for select/generate and ID selection, 256 for rewriting. All Qwen and paper inference initiated by the playground is serialized through one gate. Other external clients must coordinate their own work; the server's single slot only serializes Qwen.

## Character facts

Pass `--character-profile /absolute/path/profile.json` to the playground to supply an explicit profile:

```json
{"name":"Librarian","style":"Natural and concise","scene":"A library","era":"Today","facts":[{"id":"book","text":"The book is blue."}]}
```

The default is a generic character with no supplied world facts. Generation is instructed to avoid unsupported assertions and ask for clarification when needed. It must cite only valid profile fact IDs. This is **not a semantic factuality guarantee**: review generated outputs, especially on character/era consistency. Candidate replies themselves are not treated as verified facts for generation. The profile is included in export metadata with its hash. Paper adapters currently receive plain dialogue without profile-specific persona formatting; this reduced context and their domain mismatch are explicit service metadata.

## API and ownership

`POST /api/session {"methodId":"hybrid"}` creates a method-bound session. `POST /api/respond {"methodId":"hybrid","sessionId":"...","requestId":"UUID","input":"...","preview":false}` returns response, selectedId, provenance, metadata and bounded history. Preview never commits. The most recent successful request ID is idempotent; reuse with different input is rejected. There is no automatic network retry and no durable delivery guarantee. `POST /api/reset {"sessionId":"..."}` cancels pending work, increments the generation and clears history.

Model work uses immutable owned snapshots outside the bank lock. A result must still match the same session generation and bank revision before committing. A selected ID must resolve to the exact stored bank response. Generated text commits through `CommitRendered`; rewriting commits the original user input and actual selected response. One pending request per session is allowed. Bank edits are refused while any conversation has turns or work in flight, even if an oversized exchange was dropped from native history. External bank changes cause an explicit error rather than wrong-ID rendering; restart to reload them. `POST /api/close` disposes an unused session; Refresh methods closes its previous sessions. Idle sessions expire after 30 minutes, with a 128-session cap.

The legacy `/api/match` and `/api/preview` endpoints remain for unbound sessions. They cannot operate on method-bound comparison sessions.

## Verification

```sh
dotnet run --project experiments/MethodChecks -c Release -- "$PWD"
```

The deterministic contract runner uses fake network adapters and the real embedding backend, with its own three-row bank in `artifacts/llm_methods`. It covers isolation, preview purity, reset/cancellation races, duplicate requests, exact IDs/text, complete-exchange prompt trimming, generated history, answer-free rewriting and external bank changes. These are implementation tests, not model quality results.

The five-turn actual Qwen smoke runner uses that same deliberately trivial library bank and profile, in a separate playground on port 5188:

```sh
temp/run.sh --bank "$PWD/artifacts/llm_methods/contract-bank.tsv" \
  --character-profile "$PWD/experiments/MethodChecks/profile.json" --port 5188
python3 experiments/MethodChecks/smoke_llm.py --output artifacts/llm_methods/qwen-smoke.json
```

It verifies authored selection, explicit generation, ID selection, contextual rewriting and exact agreement between preflight token counts and completion usage. It is not held out and cannot establish an accuracy rate. Raw requests, responses and metadata are saved in the output JSON. Any optional paper adapter requires its own real-checkpoint validation. Those implementations and results are not included here; these smoke checks do not establish comparative quality.

The optional `experiments/MethodChecks/smoke_paper.py` runner and its training-dependent fixtures are excluded. The included deterministic runner instead tests the adapter boundary with fake network responses and a small synthetic bank; no paper-model service or private dataset is required.

## Separate response catalog and keys

The existing `--bank` TSV path and editor remain unchanged. Catalog mode is opt-in and read-only:

```sh
temp/run.sh --catalog /absolute/path/to/manifest.json --port 5188
```

Do not pass `--bank` and `--catalog` together. The manifest uses this exact shape:

```json
{
  "schema_version": "semantic.catalog-keys.v1",
  "responses": {"path": "responses.jsonl", "sha256": "<exact file SHA256>"},
  "keys": {"path": "keys.jsonl", "sha256": "<exact file SHA256>"},
  "target_speaker": "Example character",
  "status": "preparation"
}
```

Paths are relative to the manifest and must resolve within its directory. Files are strict UTF-8 JSONL. Response records contain exactly `{"response_id":"c00000","text":"Exact response."}`; key records contain exactly `{"key_id":"k00000","response_id":"c00000","text":"Reviewed trigger exemplar"}`. Extra review, source, count and closure metadata belongs in sidecars, never model input. IDs are opaque and case-sensitive; duplicate response IDs or key IDs are rejected. Unknown response references and blank text are rejected. Identical response text under different IDs stays separate. Identical key text under distinct key IDs is allowed, including ambiguous mappings to different responses.

Zero keys is valid: use an empty file with its actual SHA256. Every response remains in the catalog. Full-response paper adapters enumerate all catalog responses exactly once, including unkeyed replies. Key-based methods cannot retrieve those unkeyed replies; with an entirely empty key index they report unavailable/explicit failure without generation or fabricated fallback keys. Coverage counts are visible in the playground. This does not redefine candidate quality or require a perfect semantic guard for approximate retrieval.

Catalog retrieval takes the **maximum cosine score over each response's keys**, then selects the top unique responses. Equal scores use UTF-8 bytewise response IDs; equal winning-key scores use key IDs. Duplicate keys cannot boost a score or occupy additional shortlist positions. Metadata carries the winning key ID/text and response score. Qwen receives only shortlist response IDs/texts, not review evidence or key provenance. Stable response IDs survive reordering; the byte-bound bank revision deliberately changes when either file or manifest changes.

No catalog text is trimmed or normalized. JSON escapes preserve tabs, line endings, Unicode and surrounding whitespace. Imports support up to 20,000 responses (8,000 characters each), 200,000 keys, and 64 MB per component. Keys must fit the configured embedding token capacity; unsupported lengths fail rather than being silently shortened. Native/C# catalog APIs are additive, and selected responses commit by stable-ID lookup. Existing numeric row IDs and tie behavior remain unchanged for legacy TSV banks.

`target_speaker` is descriptive metadata, not injected scene/persona context. `status` accepts `preparation` or `released`; it records the data owner's declaration and does not establish semantic approval or open the comparison gate. Loading a preparation catalog is not a benchmark release.

The normal `experiments/MethodChecks` runner also runs 35 self-contained catalog checks. Raw results are generated locally at `artifacts/llm_methods/catalog-checks/results.json`. It uses tiny real EmbeddingGemma fixtures and fake remote adapters. An optional 36th offline check runs only when the excluded local `artifacts/chandler/fixtures-v2/candidate-catalog-projection.jsonl` fixture exists; this fixture is neither needed nor distributed for the core checks. Core tests exercise exact text, zero/multiple keys, maximum aggregation, deduplication before top-k, tie ordering and reorder invariance. No test results are conversation-quality measurements.
