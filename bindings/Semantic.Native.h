#ifndef SEMANTIC_NATIVE_H
#define SEMANTIC_NATIVE_H

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
typedef struct Semantic_Session Semantic_Session;
typedef struct Semantic_Result {
    const char* response;
    float score;
    int index; // Zero-based row, scoped to this immutable bank snapshot.
    int tokens; // Prefix and special tokens included; Blend also embeds input alone.
    int exchanges; // Prior complete exchanges used by this preview.
    const char* context_text; // Borrowed until next preview on this session.
} Semantic_Result;

typedef struct Semantic_CatalogResult {
    const char* response_id;
    const char* response;
    const char* key_id;
    const char* key_text;
    float score;
    int tokens;
    int exchanges;
    const char* context_text;
} Semantic_CatalogResult;

// Additive catalog API. Arrays are copied, not retained. Zero keys is valid.
SEMANTIC_API Semantic_Matcher* SEMANTIC_CALL Semantic_CreateCatalogMatcher(
    const char* model, const char* const* response_ids, const char* const* response_texts, int response_count,
    const char* const* key_ids, const char* const* key_response_ids, const char* const* key_texts, int key_count, int capacity);
// Max similarity over keys per response, unique responses before top-k (1..128).
// Returns 0 for a valid catalog with no keys, -1 on error. Preview never commits.
SEMANTIC_API int SEMANTIC_CALL Semantic_RetrieveResponses(Semantic_Session* session, const char* input,
    int mode, int k, Semantic_CatalogResult* results);
SEMANTIC_API int SEMANTIC_CALL Semantic_CommitResponse(Semantic_Session* session, const char* input, const char* response_id);

// All calls sharing a matcher (including its sessions) must be serialized.
// A session owns its history and keeps the immutable model/bank alive independently.
SEMANTIC_API Semantic_Matcher* SEMANTIC_CALL Semantic_CreateMatcherWithCapacity(const char* model, const char* bank, int capacity);
SEMANTIC_API int SEMANTIC_CALL Semantic_TokenLimit(Semantic_Matcher* matcher, int trained);
SEMANTIC_API Semantic_Session* SEMANTIC_CALL Semantic_CreateSession(Semantic_Matcher* matcher, int budget);
SEMANTIC_API void SEMANTIC_CALL Semantic_DestroySession(Semantic_Session* session);
SEMANTIC_API void SEMANTIC_CALL Semantic_ResetSession(Semantic_Session* session);
// mode: 0=input only, 1=history, 2=last exchange, 3=70% current + 30% last-exchange vector.
// Preview never changes history. Response is borrowed until the owning session is destroyed.
SEMANTIC_API int SEMANTIC_CALL Semantic_Preview(Semantic_Session* session, const char* input, int mode, Semantic_Result* result);
// Latest-input-only candidate retrieval: history is deliberately inaccessible.
// Returns number written, or -1 on error. Results have zero exchanges. No commit.
// k must be 1..128; caller supplies room for k Semantic_Result objects.
SEMANTIC_API int SEMANTIC_CALL Semantic_Retrieve(Semantic_Session* session, const char* input, int k, Semantic_Result* results);
// Explicitly record the user and actually selected authored response. 1=success, 0=error.
SEMANTIC_API int SEMANTIC_CALL Semantic_Commit(Semantic_Session* session, const char* input, int index);
// Record validated externally rendered text only after delivery. The framework
// does not generate or judge facts; callers must label generation separately.
SEMANTIC_API int SEMANTIC_CALL Semantic_CommitRendered(Semantic_Session* session, const char* input, const char* response);
SEMANTIC_API int SEMANTIC_CALL Semantic_HistoryCount(Semantic_Session* session);
// Borrowed text is valid until the next commit/reset/destroy. character: 0=user, 1=character.
SEMANTIC_API const char* SEMANTIC_CALL Semantic_HistoryText(Semantic_Session* session, int index, int character);

// Paths and text are UTF-8. Use each matcher serially.
SEMANTIC_API Semantic_Matcher* SEMANTIC_CALL Semantic_CreateMatcher(const char* model, const char* bank);
SEMANTIC_API void SEMANTIC_CALL Semantic_DestroyMatcher(Semantic_Matcher* matcher);
// Response belongs to the matcher and is valid until it is destroyed. NULL means error.
SEMANTIC_API const char* SEMANTIC_CALL Semantic_Match(Semantic_Matcher* matcher, const char* input, float* score);
// Borrowed error text, valid until the next call on this thread.
SEMANTIC_API const char* SEMANTIC_CALL Semantic_LastError(void);
SEMANTIC_API const char* SEMANTIC_CALL Semantic_BackendInfo(void);

#ifdef __cplusplus
}
#endif
#endif
