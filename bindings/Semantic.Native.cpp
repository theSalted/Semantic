#include "Semantic.hpp"
#include "llama.h"

#if defined(_WIN32)
#define SEMANTIC_EXPORT __declspec(dllexport)
#define SEMANTIC_CALL __cdecl
#else
#define SEMANTIC_EXPORT __attribute__((visibility("default")))
#define SEMANTIC_CALL
#endif

// Borrowed diagnostic text; copy immediately and call serially.
extern "C" SEMANTIC_EXPORT const char* SEMANTIC_CALL Semantic_BackendInfo() noexcept
{
    try
    {
        return llama_print_system_info();
    }
    catch (...)
    {
        return nullptr;
    }
}
