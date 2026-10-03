# Starts the guest: Vanadium with a place's tools and scripts but not its map, waiting for the host (GTA V) to connect.
# Start it before or after the game; the host keeps trying to connect. Close its window to stop it.
#   -Place     the place whose tools the player gets (default: places\Tools.rbxlx, the classic tools and nothing else)
#   -KeepMap   keep the place's own map too
#   -Empty     no place at all: an empty world, which the tests without a character use
param(
    [string]$Place = (Join-Path (Split-Path -Parent $PSScriptRoot) "places\Tools.rbxlx"),
    [switch]$KeepMap,
    [switch]$Empty,
    [string]$Msys = "C:\msys64\clang64\bin"
)
$ErrorActionPreference = "Stop"
if ($Empty) { $Place = "" }
$folder = Join-Path (Split-Path -Parent $PSScriptRoot) "build\vanadium\GtrGuest"
if (-not (Test-Path (Join-Path $folder "gtr-guest.exe"))) { throw "gtr-guest.exe isn't built: run tools\build-guest.ps1" }
if ($Place -and -not (Test-Path $Place)) { throw "no place at $Place" }

# The guest was built with MSYS2's clang64 and needs its runtime DLLs
$env:PATH = "$Msys;$env:PATH"
$arguments = @()
if ($Place) { $arguments += "`"$Place`"" }
if ($KeepMap) { $arguments += "--keep-map" }
if ($arguments) {
    Start-Process -FilePath (Join-Path $folder "gtr-guest.exe") -ArgumentList $arguments -WorkingDirectory $folder
} else {
    Start-Process -FilePath (Join-Path $folder "gtr-guest.exe") -WorkingDirectory $folder
}
Write-Host "guest started; its log is the newest file in $folder\logs"
