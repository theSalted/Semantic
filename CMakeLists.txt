cmake_minimum_required(VERSION 3.24)
project(Semantic LANGUAGES CXX)

# cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSEMANTIC_BUILD_INTEROP_SMOKE=ON
# cmake --build build --config Release
# ctest --test-dir build -C Release --output-on-failure
# Run scripts/download-model.sh before configuring to enable real embedding checks.

add_library(Semantic INTERFACE)
target_include_directories(Semantic INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(Semantic INTERFACE cxx_std_17)

# Distributable plugin builds (CI, Unity) avoid host-specific CPU features.
option(SEMANTIC_PORTABLE "Build for redistribution rather than this machine's CPU" OFF)
option(SEMANTIC_VULKAN "Windows: build the Vulkan backend (needs the Vulkan SDK)" OFF)
option(SEMANTIC_CUDA "Windows: build the CUDA backend (needs the CUDA Toolkit)" OFF)

# Compile the pinned backend into the bridge; keep downloads inside the build tree.
include(FetchContent)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    # Backends are separate DLLs chosen at runtime: CUDA or Vulkan when the machine has a
    # supported GPU, otherwise the fastest CPU variant it can run.
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    # One folder for every DLL and check, as the plugin folder will be.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(GGML_BACKEND_DL ON CACHE BOOL "" FORCE)
    set(GGML_CPU_ALL_VARIANTS ON CACHE BOOL "" FORCE)
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
    set(GGML_VULKAN ${SEMANTIC_VULKAN} CACHE BOOL "" FORCE)
    set(GGML_CUDA ${SEMANTIC_CUDA} CACHE BOOL "" FORCE)
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
endif()
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
# nlohmann/json ships in llama.cpp's vendor tree; no separate dependency.
target_include_directories(Semantic.Native PRIVATE "${llama_cpp_SOURCE_DIR}/vendor")
target_link_libraries(Semantic.Native PRIVATE Semantic llama)
target_compile_definitions(Semantic.Native PRIVATE SEMANTIC_NATIVE_BUILD)
if(WIN32)
    target_link_libraries(Semantic.Native PRIVATE winhttp)
    target_compile_definitions(Semantic.Native PRIVATE SEMANTIC_BACKEND_DL)
    set_target_properties(Semantic.Native PROPERTIES PREFIX "")
else()
    find_package(CURL REQUIRED)
    target_link_libraries(Semantic.Native PRIVATE CURL::libcurl)
endif()
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
add_executable(Semantic.HybridChecks experiments/HybridChecks.cpp)
target_include_directories(Semantic.HybridChecks PRIVATE "${llama_cpp_SOURCE_DIR}/vendor")
target_link_libraries(Semantic.HybridChecks PRIVATE Semantic)
add_executable(Semantic.HybridSmoke experiments/HybridSmoke.cpp)
target_include_directories(Semantic.HybridSmoke PRIVATE bindings)
target_link_libraries(Semantic.HybridSmoke PRIVATE Semantic.Native)
add_executable(Semantic.DownloadChecks experiments/DownloadChecks.cpp)
target_include_directories(Semantic.DownloadChecks PRIVATE bindings)
if(WIN32)
    target_link_libraries(Semantic.DownloadChecks PRIVATE winhttp)
else()
    target_link_libraries(Semantic.DownloadChecks PRIVATE CURL::libcurl)
endif()
foreach(check Semantic.HybridChecks Semantic.HybridSmoke Semantic.DownloadChecks)
    set_target_properties(${check} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
    target_compile_options(${check} PRIVATE -UNDEBUG) # Checks rely on assert in every configuration.
endforeach()
enable_testing()
add_test(NAME Semantic.CoreChecks
    COMMAND Semantic.CoreChecks "${CMAKE_CURRENT_SOURCE_DIR}/experiments/phrases.tsv")
add_test(NAME Semantic.HybridChecks COMMAND Semantic.HybridChecks)
set(SEMANTIC_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/models/embeddinggemma-2-Q8_0.gguf"
    CACHE FILEPATH "Local EmbeddingGemma2 GGUF for the phrase experiment")
if(EXISTS "${SEMANTIC_MODEL}")
    add_executable(Semantic.PoolingChecks experiments/PoolingChecks.cpp)
    target_link_libraries(Semantic.PoolingChecks PRIVATE Semantic llama)
    add_test(NAME Semantic.PoolingChecks COMMAND Semantic.PoolingChecks "${SEMANTIC_MODEL}")
    add_test(NAME Semantic.PhraseMatch
        COMMAND Semantic.PhraseMatch "${SEMANTIC_MODEL}" "${CMAKE_CURRENT_SOURCE_DIR}/experiments/phrases.tsv")
    # Retrieval and preparation through the C API; hybrid turns run when SEMANTIC_HYBRID_CONFIG is set.
    add_test(NAME Semantic.HybridSmoke
        COMMAND Semantic.HybridSmoke "${SEMANTIC_MODEL}" "${CMAKE_CURRENT_SOURCE_DIR}/experiments/tagged-phrases.tsv")
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
