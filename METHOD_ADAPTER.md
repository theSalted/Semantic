# Local method adapter v1

This is the boundary between the included C++/C#/playground framework and an optional external paper-method service. Paper implementations, trained checkpoints, datasets and local release artifacts are not included in this framework release.

The playground serializes its own heavy adapter requests; external clients must coordinate resource use separately. Local orchestration status files under `artifacts/` are optional workspace data and are not distributed or required by the framework.

Paper service: loopback `http://127.0.0.1:8091`. No generation. Stateless requests; the playground owns isolated per-method sessions, commits the actual response, and resets history. Methods must not accumulate hidden history.

`GET /methods` (read-only; do not implicitly load weights):

```json
{"methods":[{"id":"light_biranker","name":"LIGHT bi-ranker","available":true,"reason":"","metadata":{"checkpoint":"...","stage":"trained response selection","implementation":"original or declared port"}}]}
```

`POST /rank`:

```json
{"protocol_version":1,"method":"light_biranker","bank_revision":"sha256","query":"latest user input","history":["prior user","actual selected reply"],"candidates":[{"id":"0","text":"exact authored response"}],"candidate_scope":"full_bank"}
```

The framework stores explicit user/character exchanges; this boundary flattens them into chronological alternating utterance strings as required by the paper adapter. The full current response bank is supplied. Legacy TSV banks use zero-based row IDs scoped to the snapshot. Catalog mode uses persistent opaque `response_id` strings from the full response catalog, including replies with zero retrieval keys. Never parse these IDs as integers or infer an ID from text. Different IDs may legitimately have identical reply text. The adapter may cache candidate encodings by `bank_revision` but must verify content or include a content hash; never reuse wrong-bank rows. Actual model token limits and truncation must be returned in metadata.

Response:

```json
{"selected_id":"0","selected_text":"exact authored response","scores":[{"id":"0","score":0.5}],"metadata":{"checkpoint":"...","candidate_scope":"full_bank","candidate_count":1,"history_turns_used":1,"truncation":"none","latency_ms":12}}
```

`selected_id` must be one of the provided IDs. `selected_text` is optional but, if present, must equal that exact bank text. The playground renders the stored text by ID. Missing checkpoints, errors or unsupported methods return a non-2xx error and an honest `/methods` status, never a substitute model. Return checkpoint identity/hash, runtime, dataset/training stage and any port differences. Discover availability at runtime; integration status is not model-quality validation.

Catalog interchange is `semantic.catalog-keys.v1`, documented in `LOCAL_METHODS.md`. Additional local contract records under `artifacts/` are excluded. The format separates exact response records `{response_id,text}` from zero-or-more key records `{key_id,response_id,text}`. Keys and their review sidecars are not sent to paper rankers. Key-based shortlists use maximum key similarity per response with deterministic ID ties, deduplicate before top-k, and expose winning-key provenance in result metadata. No fallback key is synthesized for an unkeyed response. The HTTP `/rank` shape and `candidate_scope:"full_bank"` remain compatible; in catalog mode “full bank” means the complete response catalog, not the subset with keys.
