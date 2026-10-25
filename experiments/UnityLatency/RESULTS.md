# Full-completion observations — 2026-10-25

The smaller original Qwen3-4B Q4_K_M reached the requested latency range on the tested short-response workload, but failed semantic checks. **The combined speed-and-quality goal is not met.** The user requested an explicitly temporary in-game trial; 14B remains the production default.

Hardware: Apple M5 Max, 18 CPU cores, 32 GPU cores, 36 GiB unified memory. Existing llama.cpp Release build `e937b17a2df081e9c7ae380f0dab4341c1585bce`, Metal, all layers offloaded (41/41 for 14B, 37/37 for 4B), six CPU threads, one slot, 8192-token context. Temperature 0, seed 19, thinking disabled, 12 candidates, output ceiling 512 tokens and 1600 displayed characters. No output or context limit was reduced for speed.

## Same workload, real LLM calls

| Model/configuration | Calls | Median | p95 | Observed p99 | Maximum | Below 1 s |
|---|---:|---:|---:|---:|---:|---:|
| Original 14B | 100 | 916.7 ms | 2125.2 ms | 2280.6 ms | 2295.9 ms | 56/100 |
| 14B, ngram-simple speculative trial | 100 | 943.0 ms | — | 2319.2 ms | 2323.8 ms | — |
| Original 4B, unchanged prompt | 100 | 384.4 ms | 613.5 ms | 686.8 ms | 739.7 ms | 100/100 |

Boundary: request serialization/send through receipt/parsing of the complete bridge response, including retrieval, queue, Qwen preflight/prefill/decode, validation, history commit and loopback transport. Concurrency one, warm model, normal desktop apps, no Unity rendering in this measurement. Five repeated 20-turn episodes are correlated; nearest-rank sample p99 is **not** a population p99 guarantee. Cold loading, concurrency tails and maximum-length output were not established. No failed calls were excluded; these runs had no HTTP or length-limit errors.

First requests were reported separately: 14B 1255.0 ms, 4B 562.1 ms. Five retrieval-only controls per model were also separate (median 23.3 ms and 20.3 ms respectively). All 75 ordinary 4B hybrid calls remained below one second even excluding 25 schema-constrained memory calls. Thus retrieval bypasses did not conceal model latency.

The 4B workload emitted at most **59 tokens including JSON / 167 displayed characters**, with prompts of 831–1051 tokens. The allowed ceiling remains 512 tokens / 1600 characters; sub-second completion at that ceiling was not demonstrated and would take several seconds at observed decode speed. Median 4B prefill was about 117 ms and decoding 104 tokens/s, compared with about 507 ms and 35 tokens/s for 14B. Retrieval was about 25 ms on the baseline; bridge validation/serialization/transport were a small fraction of the delay. The dominant improvement is lower model prefill and decode cost.

Thinking was already disabled and stable-prefix reuse was active. The speculative trial accepted only 156 of 1816 draft tokens and did not improve the tail; it was rejected. No production flags were changed. The 14B run had existing swap occupancy, but zero swap-outs in the measurement interval; the data do not establish paging as the primary cause.

## Semantic failure findings

Structural exact-response, emote and history checks do not prove that the selected line answers the player. A post-run authored-choice audit found **15/35 failures for 4B versus 0/35 for 14B**: contextual beer orders chose ale, availability questions chose a recommendation, and an identity question generated a paraphrase when an exact authored introduction fit. All five later prior-order recalls then reported ale instead of the user's beer. The appropriate candidates and actual user history were present; there was no missing-history transport or off-by-one ID defect.

Direct constrained name recall, correction and reset worked. Twenty-nine harder 4B calls (maximum 624 ms) exposed eight clear semantic failures, including incorrect speaker attribution, ignored corrected name/drink in a greeting, character facts attributed to the player after reset, a quoted traveler's name treated as the player, and irrelevant candidate text reused for lost-item advice. The explicit ledger did survive transcript eviction, but broader use of its facts remained unreliable.

The same 29 harder probes on 14B reached 1979 ms and also exposed pre-existing semantic weaknesses with identities, unknown locations and unsupported advice. The preserved 14B setup is not a universal quality guarantee. Small role-message, selector-only and explicit-decision diagnostic variants did not resolve the failures and were not adopted. The committed trial keeps the original measured prompt.

## In-game trial and validation

Unity 6000.6.2f1 opened the existing Playground scene and entered real Play Mode with the original cube, subtitle and controls. Model identity and matching 33-row bank health were verified. The user began testing during setup, so their conversation was retained instead of reset. Eight observed live server HTTP completions ranged from **432.9 to 748.3 ms**; this passive observation excludes UI rendering and is not another p99 measurement. No benchmark was run concurrently.

The existing 14B launcher, runtime source, fast-path settings, Unity scene and bank remained unchanged. Snapshot hashes were checked. The prior baseline also passed deterministic session/fast-path checks and real cancel/reset and service-unavailable probes. Those are integration checks, not model-quality approval. Raw synthetic probe results and operational evidence remain local and are excluded from this source commit.

Packaging verification: the preparer reproduced all eleven measured 4B source files byte-for-byte. The portable launcher's `--build-only` path compiled with zero warnings/errors; its occupied-port check refused the live trial without modifying it. Python and shell syntax checks passed. Offline replay of 265 saved responses exercised the portable probes without inference: 14B passed its 106 automatic checks/turns, the 4B workload correctly returned failure for 20 turns (15 authored-choice failures plus five order recalls), and the harder 4B probe returned failure while retaining manual review requirements. Staged whitespace and credential/machine-path scans passed. Raw replay outputs are not part of Git.
