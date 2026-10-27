# Local Windows plugin build with the same settings as CI.
# Needs Visual Studio 2022 (C++), the Vulkan SDK and the CUDA Toolkit 12.x.
# Usage: scripts\build-plugin.ps1 [-UnityProject C:\path\to\UnityProject] [-NoCuda] [-NoVulkan]
param([string]$UnityProject, [switch]$NoCuda, [switch]$NoVulkan)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build-plugin'
$cuda = if ($NoCuda) { 'OFF' } else { 'ON' }
$vulkan = if ($NoVulkan) { 'OFF' } else { 'ON' }
cmake -S $repo -B $build -G 'Visual Studio 17 2022' -A x64 -DSEMANTIC_PORTABLE=ON "-DSEMANTIC_CUDA=$cuda" "-DSEMANTIC_VULKAN=$vulkan" `
    '-DCMAKE_CUDA_ARCHITECTURES=61-real;75-real;86-real;89-real;89-virtual'
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $build --config Release --parallel
if ($LASTEXITCODE) { exit $LASTEXITCODE }
ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE) { exit $LASTEXITCODE }
if ($UnityProject) {
    $plugins = Join-Path $UnityProject 'Assets\Plugins\Windows\x86_64'
    New-Item -ItemType Directory -Force $plugins | Out-Null
    $bin = Join-Path $build 'bin\Release'
    Copy-Item (Join-Path $bin 'Semantic.Native.dll'), (Join-Path $bin 'llama.dll') $plugins -Force
    Copy-Item (Join-Path $bin 'ggml*.dll') $plugins -Force
    if (-not $NoCuda) {
        foreach ($dll in 'cudart64_12.dll', 'cublas64_12.dll', 'cublasLt64_12.dll') { Copy-Item (Join-Path $env:CUDA_PATH "bin\$dll") $plugins -Force }
    }
    Write-Host "Installed into $plugins. New DLLs need .meta files: run scripts/sync-unity-plugins.sh once, or let Unity import them and enable Win64 + Editor (Windows, x86_64)."
}
