# Windows: use llama.cpp's official prebuilt release instead of compiling ggml.
# Downloads (once, SHA-256 verified) the CPU build plus the Vulkan and optionally CUDA backends,
# generates MSVC import libraries from the release DLLs, and defines imported `llama`/`ggml`
# targets. Every runtime DLL is copied beside Semantic.Native so the folder is the plugin.

set(SEMANTIC_LLAMA_RELEASE_DIR "${CMAKE_BINARY_DIR}/llama-release" CACHE PATH "Download cache for llama.cpp release archives")
set(_release "https://github.com/ggml-org/llama.cpp/releases/download/${SEMANTIC_LLAMA_TAG}")

function(_semantic_fetch name url sha256)
    set(archive "${SEMANTIC_LLAMA_RELEASE_DIR}/${name}.zip")
    set(dir "${SEMANTIC_LLAMA_RELEASE_DIR}/${name}")
    if(NOT EXISTS "${dir}/.extracted")
        file(DOWNLOAD "${url}" "${archive}" EXPECTED_HASH SHA256=${sha256} SHOW_PROGRESS)
        file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${dir}")
        file(TOUCH "${dir}/.extracted")
    endif()
endfunction()

_semantic_fetch(cpu "${_release}/llama-${SEMANTIC_LLAMA_TAG}-bin-win-cpu-x64.zip"
    a8c9ad8c39957294dbb3d694251fe43f98f481bff118e579f133b4dbbd545099)
_semantic_fetch(vulkan "${_release}/llama-${SEMANTIC_LLAMA_TAG}-bin-win-vulkan-x64.zip"
    2ef19cf51dd5f037dcac25e57f2299b4ffae0c9620b83a14cf9b3f60860b72fc)
set(_runtime
    "${SEMANTIC_LLAMA_RELEASE_DIR}/cpu/ggml-base.dll" "${SEMANTIC_LLAMA_RELEASE_DIR}/cpu/ggml.dll"
    "${SEMANTIC_LLAMA_RELEASE_DIR}/cpu/llama.dll" "${SEMANTIC_LLAMA_RELEASE_DIR}/cpu/libomp.dll"
    "${SEMANTIC_LLAMA_RELEASE_DIR}/vulkan/ggml-vulkan.dll")
file(GLOB _cpu_variants "${SEMANTIC_LLAMA_RELEASE_DIR}/cpu/ggml-cpu-*.dll")
list(APPEND _runtime ${_cpu_variants})
if(SEMANTIC_CUDA)
    _semantic_fetch(cuda "${_release}/llama-${SEMANTIC_LLAMA_TAG}-bin-win-cuda-12.4-x64.zip"
        5b0e7d7d6a0c6908005459a3371463d1b125b036ba99fbe9cb2f5e1636d85725)
    _semantic_fetch(cudart "${_release}/cudart-llama-bin-win-cuda-12.4-x64.zip"
        8c79a9b226de4b3cacfd1f83d24f962d0773be79f1e7b75c6af4ded7e32ae1d6)
    list(APPEND _runtime "${SEMANTIC_LLAMA_RELEASE_DIR}/cuda/ggml-cuda.dll"
        "${SEMANTIC_LLAMA_RELEASE_DIR}/cudart/cudart64_12.dll" "${SEMANTIC_LLAMA_RELEASE_DIR}/cudart/cublas64_12.dll"
        "${SEMANTIC_LLAMA_RELEASE_DIR}/cudart/cublasLt64_12.dll")
endif()
set(SEMANTIC_RUNTIME_DLLS ${_runtime} CACHE INTERNAL "")

# Import library from a DLL's C exports (the release ships DLLs only).
get_filename_component(_msvc_bin "${CMAKE_LINKER}" DIRECTORY)
find_program(SEMANTIC_DUMPBIN dumpbin HINTS "${_msvc_bin}" REQUIRED)
function(_semantic_import target dll)
    get_filename_component(stem "${dll}" NAME_WE)
    set(lib "${CMAKE_BINARY_DIR}/import/${stem}.lib")
    if(NOT EXISTS "${lib}")
        execute_process(COMMAND "${SEMANTIC_DUMPBIN}" /nologo /exports "${dll}" OUTPUT_VARIABLE dump COMMAND_ERROR_IS_FATAL ANY)
        string(REGEX MATCHALL "\n +[0-9]+ +[0-9A-Fa-f]+ +[0-9A-Fa-f]+ +[A-Za-z_][A-Za-z0-9_]*" rows "${dump}")
        set(def "LIBRARY ${stem}\nEXPORTS\n")
        foreach(row IN LISTS rows)
            string(REGEX REPLACE ".* ([A-Za-z_][A-Za-z0-9_]*)$" "\\1" name "${row}")
            string(APPEND def "    ${name}\n")
        endforeach()
        file(WRITE "${CMAKE_BINARY_DIR}/import/${stem}.def" "${def}")
        execute_process(COMMAND "${CMAKE_AR}" /nologo "/def:${CMAKE_BINARY_DIR}/import/${stem}.def" /machine:x64 "/out:${lib}"
            COMMAND_ERROR_IS_FATAL ANY)
    endif()
    add_library(${target} SHARED IMPORTED GLOBAL)
    set_target_properties(${target} PROPERTIES IMPORTED_LOCATION "${dll}" IMPORTED_IMPLIB "${lib}")
endfunction()

_semantic_import(ggml-base "${SEMANTIC_LLAMA_RELEASE_DIR}/cpu/ggml-base.dll")
_semantic_import(ggml "${SEMANTIC_LLAMA_RELEASE_DIR}/cpu/ggml.dll")
_semantic_import(llama "${SEMANTIC_LLAMA_RELEASE_DIR}/cpu/llama.dll")
set_target_properties(ggml PROPERTIES INTERFACE_LINK_LIBRARIES ggml-base)
set_target_properties(llama PROPERTIES
    INTERFACE_LINK_LIBRARIES ggml
    INTERFACE_INCLUDE_DIRECTORIES "${llama_cpp_SOURCE_DIR}/include;${llama_cpp_SOURCE_DIR}/ggml/include"
    INTERFACE_COMPILE_DEFINITIONS "LLAMA_SHARED;GGML_SHARED;GGML_BACKEND_SHARED;GGML_BACKEND_DL")

# Executables and the plugin share one folder with every runtime DLL.
function(semantic_copy_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${SEMANTIC_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${target}>")
endfunction()
