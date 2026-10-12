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
