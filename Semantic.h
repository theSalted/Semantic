#ifndef SEMANTIC_H
#define SEMANTIC_H

// Semantic: authored-dialogue matching with an optional local-LLM hybrid mode.
//
// One API for both modes. The mode comes from what Semantic_CreateMatcher receives as `model`:
//   - an embedding .gguf: retrieval mode, a stateless nearest-row match over the bank;
//   - a .json config with "mode":"hybrid": hybrid mode, one conversation per matcher. It takes a
//     latest-input shortlist from the bank, then a local chat model either selects an authored row
//     verbatim or generates an untagged reply.
//
// Hybrid config (relative paths resolve against the config file):
//   { "mode": "hybrid", "embedding_model": "...gguf", "llm_model": "...gguf",
//     "llm_url": "https://.../pinned/model.gguf", "profile": "character.profile.json",
//     "candidates": 12, "session_memory": true, "gpu_layers": 99,
//     "fast_path": { "enabled": true, "min_similarity": 0.99, "min_margin": 0.06,
//                    "eligible_prompts": ["..."] } }
// A chat model missing from disk is downloaded once from llm_url into the per-user cache
// (~/Library/Caches/Semantic/models, %LOCALAPPDATA%\Semantic\models); interrupted downloads resume.
// The chat model must use the ChatML format (e.g. Qwen3).
//
// Bank: UTF-8 TSV with a "prompt<TAB>response" or "prompt<TAB>response<TAB>emote_tag" header.
// Paths and text are UTF-8. Use each matcher from one thread at a time.

#if defined(_WIN32)
#if defined(SEMANTIC_NATIVE_BUILD)
#define SEMANTIC_API __declspec(dllexport)
#else
#define SEMANTIC_API __declspec(dllimport)
#endif
#define SEMANTIC_CALL __cdecl
#else
#define SEMANTIC_API __attribute__((visibility("default")))
#define SEMANTIC_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Semantic_Matcher Semantic_Matcher;

// Returns NULL on error (see Semantic_LastError). In hybrid mode a new conversation is a new
// matcher; the chat model stays loaded while any matcher or a preparation uses it, so create the
// replacement before destroying the old one.
SEMANTIC_API Semantic_Matcher* SEMANTIC_CALL Semantic_CreateMatcher(const char* model, const char* bank);
SEMANTIC_API void SEMANTIC_CALL Semantic_DestroyMatcher(Semantic_Matcher* matcher);

// Returns the reply, or NULL on error. Retrieval mode: owned by the matcher until it is destroyed.
// Hybrid mode: valid until the next match; a failed turn is not added to the conversation.
SEMANTIC_API const char* SEMANTIC_CALL Semantic_Match(Semantic_Matcher* matcher, const char* input, float* score);
// Also returns the authored tag of the reply's row. Two-column banks and hybrid generated replies
// return an empty tag; generated replies also return a zero score. On error returns NULL, sets
// *emote_tag to NULL and *score to zero.
SEMANTIC_API const char* SEMANTIC_CALL Semantic_MatchWithTag(Semantic_Matcher* matcher,
    const char* input, float* score, const char** emote_tag);

// Background preparation, so the first conversation is ready before it is needed: downloads a
// missing chat model, loads the models and embeds the bank on a worker thread. The next
// Semantic_CreateMatcher with the same paths takes the prepared matcher instantly, or waits for a
// preparation still running. A successful preparation keeps its models loaded for the life of the
// process. Returns 1 when started, already running or ready; 0 on bad arguments. Calling it again
// after a failure retries.
SEMANTIC_API int SEMANTIC_CALL Semantic_Prepare(const char* model, const char* bank);
// Returns 0=not started, 1=running, 2=ready, -1=failed. *progress is the download fraction (0..1)
// while downloading and 1 when ready. *stage is "downloading", "loading", "ready" or the failure
// message, borrowed until the next call on this thread. Either output may be NULL.
SEMANTIC_API int SEMANTIC_CALL Semantic_PrepareStatus(const char* model, const char* bank, float* progress, const char** stage);

// Borrowed error text for the last failed call on this thread.
SEMANTIC_API const char* SEMANTIC_CALL Semantic_LastError(void);
// Compute backends available in this build (e.g. Metal, CUDA, CPU features).
SEMANTIC_API const char* SEMANTIC_CALL Semantic_BackendInfo(void);

#ifdef __cplusplus
}
#endif
#endif
