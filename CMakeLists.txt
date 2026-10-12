cmake_minimum_required(VERSION 3.24)
project(Semantic LANGUAGES CXX)

# cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSEMANTIC_BUILD_INTEROP_SMOKE=ON
# cmake --build build --config Release
# ctest --test-dir build -C Release --output-on-failure
# Run scripts/download-model.sh before configuring to enable real embedding checks.

add_library(Semantic INTERFACE)
target_include_directories(Semantic INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(Semantic INTERFACE cxx_std_17)

# Compile the pinned backend into the bridge; keep downloads inside the build tree.
include(FetchContent)
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(LLAMA_BUILD_COMMON OFF)
set(LLAMA_BUILD_TESTS OFF)
set(LLAMA_BUILD_TOOLS OFF)
set(LLAMA_BUILD_EXAMPLES OFF)
set(LLAMA_BUILD_SERVER OFF)
set(LLAMA_BUILD_APP OFF)
FetchContent_Declare(llama_cpp
    URL https://github.com/ggml-org/llama.cpp/archive/e937b17a2df081e9c7ae380f0dab4341c1585bce.tar.gz
    URL_HASH SHA256=930c02443621a78a5397db0dcc93e15a368760e39222bac68aa08c2d8575f585
)
FetchContent_MakeAvailable(llama_cpp)

add_library(Semantic.Native SHARED bindings/Semantic.Native.cpp)
target_link_libraries(Semantic.Native PRIVATE Semantic llama)
target_compile_definitions(Semantic.Native PRIVATE SEMANTIC_NATIVE_BUILD)
set_target_properties(Semantic.Native PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

add_executable(Semantic.PhraseMatch experiments/PhraseMatch.cpp)
target_link_libraries(Semantic.PhraseMatch PRIVATE Semantic Semantic.Native)
add_executable(Semantic.CoreChecks experiments/CoreChecks.cpp)
target_link_libraries(Semantic.CoreChecks PRIVATE Semantic)
enable_testing()
add_test(NAME Semantic.CoreChecks
    COMMAND Semantic.CoreChecks "${CMAKE_CURRENT_SOURCE_DIR}/experiments/phrases.tsv")
set(SEMANTIC_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/models/embeddinggemma-2-Q8_0.gguf"
    CACHE FILEPATH "Local EmbeddingGemma2 GGUF for the phrase experiment")
if(EXISTS "${SEMANTIC_MODEL}")
    add_test(NAME Semantic.PhraseMatch
        COMMAND Semantic.PhraseMatch "${SEMANTIC_MODEL}" "${CMAKE_CURRENT_SOURCE_DIR}/experiments/phrases.tsv")
endif()

# C++ consumers only need the header; .NET is required only for this smoke check.
option(SEMANTIC_BUILD_INTEROP_SMOKE "Build the C# interop smoke check" OFF)
if(SEMANTIC_BUILD_INTEROP_SMOKE)
    find_program(DOTNET_EXECUTABLE dotnet REQUIRED)
    set(SMOKE_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/smoke/$<CONFIG>")
    add_custom_target(Semantic.InteropSmoke ALL
        COMMAND "${DOTNET_EXECUTABLE}" build
            "${CMAKE_CURRENT_SOURCE_DIR}/experiments/InteropSmoke/Semantic.InteropSmoke.csproj"
            --configuration "$<IF:$<CONFIG:Debug>,Debug,Release>"
            --output "${SMOKE_OUTPUT_DIR}"
            --nologo
            --disable-build-servers
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:Semantic.Native>" "${SMOKE_OUTPUT_DIR}/"
        DEPENDS Semantic.Native
        VERBATIM
    )
    add_test(NAME Semantic.InteropSmoke
        COMMAND "${DOTNET_EXECUTABLE}" "${SMOKE_OUTPUT_DIR}/Semantic.InteropSmoke.dll"
    )
    if(EXISTS "${SEMANTIC_MODEL}")
        add_test(NAME Semantic.ManagedPhraseMatch
            COMMAND "${DOTNET_EXECUTABLE}" "${SMOKE_OUTPUT_DIR}/Semantic.InteropSmoke.dll"
                "${SEMANTIC_MODEL}" "${CMAKE_CURRENT_SOURCE_DIR}/experiments/phrases.tsv")
    endif()
endif()
