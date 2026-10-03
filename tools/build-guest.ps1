# Builds gtr-guest.exe inside the private clone of Vanadium (the vanadium/ submodule, see MODDING_PLAN.md) with
# MSYS2's clang64. The first build compiles the whole engine and its dependencies; later ones only what changed.
#   -Clone   check out the submodule's pinned commit first and apply patches/. The Vanadium checkout beside this
#            repository is only read, for the dependency sources it already downloaded.
param(
    [switch]$Clone,
    [string]$VanadiumSource = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "vanadium"),
    [string]$Msys = "C:\msys64\clang64\bin",
    [int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount - 1)
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tree = Join-Path $root "vanadium"
$build = Join-Path $root "build\vanadium"

# A fresh checkout of this repository has the submodule's folder but nothing in it
$cloned = Test-Path (Join-Path $tree "CMakeLists.txt")
if ($Clone -and -not $cloned) {
    git -C $root submodule update --init vanadium
    if ($LASTEXITCODE -ne 0) { throw "could not check out the vanadium submodule" }
    # The dependency sources the checkout already downloaded, so the clone doesn't fetch them again
    if (Test-Path (Join-Path $VanadiumSource ".cache")) {
        robocopy (Join-Path $VanadiumSource ".cache") (Join-Path $tree ".cache") /E /NFL /NDL /NJH /NJS /NP /MT:16 | Out-Null
    }
    foreach ($patch in Get-ChildItem (Join-Path $root "patches") -Filter *.patch) {
        git -C $tree apply $patch.FullName
        if ($LASTEXITCODE -ne 0) { throw "$($patch.Name) no longer applies to Vanadium" }
    }
}
if (-not (Test-Path (Join-Path $tree "CMakeLists.txt"))) { throw "no Vanadium clone at $tree; run with -Clone" }

$env:PATH = "$Msys;$env:PATH"
$forward = { param($path) $path -replace "\\", "/" }
if (-not (Test-Path (Join-Path $build "build.ninja"))) {
    cmake -S (& $forward $tree) -B (& $forward $build) -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
        "-DCMAKE_PROJECT_Vanadium_INCLUDE=$(& $forward (Join-Path $root 'cmake\GtrInject.cmake'))"
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
}
cmake --build (& $forward $build) --target Gtr.Guest -- "-j$Jobs"
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Host "built $(Join-Path $build 'GtrGuest\gtr-guest.exe')"
