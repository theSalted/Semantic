cmake_minimum_required(VERSION 3.24)
project(Semantic LANGUAGES CXX)

# Semantic.h is the API; Semantic.cpp is the implementation, built as the Semantic.Native plugin.
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#   ctest --test-dir build -C Release --output-on-failure
# Run scripts/download-model.sh before configuring to add the model-backed smoke test.

option(SEMANTIC_PORTABLE "Build for redistribution rather than this machine's CPU" OFF)
option(SEMANTIC_CUDA "Windows: include the CUDA backend and runtime (~1.15 GB)" ON)

# llama.cpp release b11560. macOS builds it from source into one static dylib; Windows uses the
# release's prebuilt DLLs unchanged (cmake/LlamaRelease.cmake) and compiles only Semantic.
set(SEMANTIC_LLAMA_TAG b11560)
set(SEMANTIC_LLAMA_COMMIT b13a93a3d5fa44b57899d86467a4de463e7415a7)
set(SEMANTIC_LLAMA_SOURCE_SHA256 3d89883ba9020aef07c14371f884564f4b3177b9ff8c442b34de276f817ef835)
include(FetchContent)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
FetchContent_Declare(llama_cpp
    URL https://github.com/ggml-org/llama.cpp/archive/${SEMANTIC_LLAMA_COMMIT}.tar.gz
    URL_HASH SHA256=${SEMANTIC_LLAMA_SOURCE_SHA256}
)
if(WIN32)
    # Headers and vendored JSON only; nothing in ggml/llama is compiled on Windows.
    # One output folder for the plugin, its runtime DLLs and the checks that load them.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    FetchContent_GetProperties(llama_cpp)
    if(NOT llama_cpp_POPULATED)
        cmake_policy(PUSH)
        if(POLICY CMP0169)
            cmake_policy(SET CMP0169 OLD)
        endif()
        FetchContent_Populate(llama_cpp)
        cmake_policy(POP)
    endif()
    include(cmake/LlamaRelease.cmake)
else()
    set(BUILD_SHARED_LIBS OFF)
    if(SEMANTIC_PORTABLE)
        set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
        set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
        foreach(feature AVX AVX2 FMA F16C)
            set(GGML_${feature} OFF CACHE BOOL "" FORCE)
        endforeach()
        set(GGML_CPU_ARM_ARCH armv8-a CACHE STRING "" FORCE)
    endif()
    foreach(part COMMON TESTS TOOLS EXAMPLES SERVER APP)
        set(LLAMA_BUILD_${part} OFF)
    endforeach()
    FetchContent_MakeAvailable(llama_cpp)
    find_package(CURL REQUIRED)
endif()

# Platform dependencies shared by the plugin and the checks that compile Semantic.cpp directly.
add_library(Semantic.Dependencies INTERFACE)
# nlohmann/json ships in llama.cpp's vendor tree; no separate dependency.
target_include_directories(Semantic.Dependencies INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}" "${llama_cpp_SOURCE_DIR}/vendor")
target_link_libraries(Semantic.Dependencies INTERFACE llama)
target_compile_features(Semantic.Dependencies INTERFACE cxx_std_17)
if(WIN32)
    target_link_libraries(Semantic.Dependencies INTERFACE winhttp)
    target_compile_definitions(Semantic.Dependencies INTERFACE SEMANTIC_BACKEND_DL)
else()
    target_link_libraries(Semantic.Dependencies INTERFACE CURL::libcurl)
endif()

add_library(Semantic.Native SHARED Semantic.cpp)
target_link_libraries(Semantic.Native PRIVATE Semantic.Dependencies)
target_compile_definitions(Semantic.Native PRIVATE SEMANTIC_NATIVE_BUILD)
set_target_properties(Semantic.Native PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
if(WIN32)
    set_target_properties(Semantic.Native PROPERTIES PREFIX "")
    semantic_copy_runtime(Semantic.Native)
endif()

# Checks and DownloadChecks compile Semantic.cpp to reach its internals; Smoke uses the C API.
enable_testing()
set(tests "${CMAKE_CURRENT_SOURCE_DIR}/tests")
add_executable(Semantic.Checks tests/Checks.cpp)
add_executable(Semantic.DownloadChecks tests/DownloadChecks.cpp)
foreach(check Semantic.Checks Semantic.DownloadChecks)
    target_link_libraries(${check} PRIVATE Semantic.Dependencies)
    target_compile_definitions(${check} PRIVATE SEMANTIC_NATIVE_BUILD)
endforeach()
add_executable(Semantic.Smoke tests/Smoke.cpp)
target_link_libraries(Semantic.Smoke PRIVATE Semantic.Native)
foreach(check Semantic.Checks Semantic.DownloadChecks Semantic.Smoke)
    set_target_properties(${check} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
    target_compile_options(${check} PRIVATE -UNDEBUG) # Checks rely on assert in every configuration.
endforeach()
add_test(NAME Semantic.Checks COMMAND Semantic.Checks "${tests}/phrases.tsv" "${tests}/tagged-phrases.tsv")

set(SEMANTIC_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/models/embeddinggemma-2-Q8_0.gguf"
    CACHE FILEPATH "Local EmbeddingGemma2 GGUF for the model-backed smoke test")
if(EXISTS "${SEMANTIC_MODEL}")
    # Retrieval and preparation through the C API; hybrid turns run when SEMANTIC_HYBRID_CONFIG is set.
    add_test(NAME Semantic.Smoke COMMAND Semantic.Smoke "${SEMANTIC_MODEL}" "${tests}/tagged-phrases.tsv")
endif()
