# Local Windows plugin build with the same settings as CI. Needs Visual Studio 2022 (C++) only:
# llama.cpp's backends come prebuilt from its pinned release (CPU variants, Vulkan, CUDA).
# Usage: scripts\build-plugin.ps1 [-UnityProject C:\path\to\UnityProject] [-NoCuda]
param([string]$UnityProject, [switch]$NoCuda)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build-plugin'
$cuda = if ($NoCuda) { 'OFF' } else { 'ON' }
cmake -S $repo -B $build -G 'Visual Studio 17 2022' -A x64 -DSEMANTIC_PORTABLE=ON "-DSEMANTIC_CUDA=$cuda"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $build --config Release --parallel
if ($LASTEXITCODE) { exit $LASTEXITCODE }
ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE) { exit $LASTEXITCODE }
if ($UnityProject) {
    $plugins = Join-Path $UnityProject 'Assets\Plugins\Windows\x86_64'
    New-Item -ItemType Directory -Force $plugins | Out-Null
    Copy-Item (Join-Path $build 'bin\Release\*.dll') $plugins -Force
    Write-Host "Installed into $plugins. New DLLs need .meta files: run scripts/sync-unity-plugins.sh once, or let Unity import them and enable Win64 + Editor (Windows, x86_64)."
}
