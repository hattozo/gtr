# Fetches what the GTA side builds against and installs, into third_party/ (none of it may be redistributed, so
# none of it is in the repository):
#   shv/       ScriptHookV SDK: main.h, nativeCaller.h, types.h, enums.h, ScriptHookV.lib
#   reshade/   ReShade's add-on API headers
#   fx/        ReShade.fxh and ReShadeUI.fxh, which the effect includes
#   runtime/   ScriptHookV.dll, its ASI loader dinput8.dll, and ReShade64.dll with add-on support
# dev-c.com turns away clients without browser headers.
param(
    [string]$ReShadeVersion = "6.8.0"
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$root = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "third_party"
$temp = Join-Path $root "download"
foreach ($folder in "shv", "reshade", "fx", "runtime", "download") {
    New-Item -ItemType Directory -Force (Join-Path $root $folder) | Out-Null
}

$agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36"
$shvPage = "https://www.dev-c.com/gtav/scripthookv/"
$page = Invoke-WebRequest -UseBasicParsing -Uri $shvPage -UserAgent $agent -Headers @{ Accept = "text/html"; "Accept-Language" = "en-US" }
$files = [regex]::Matches($page.Content, '/files/ScriptHookV_[^"]*\.zip') | ForEach-Object { $_.Value } | Sort-Object -Unique
if (-not $files) {
    throw "No ScriptHookV downloads found on $shvPage; get ScriptHookV and its SDK there by hand"
}
foreach ($file in $files) {
    $name = Split-Path -Leaf $file
    Invoke-WebRequest -UseBasicParsing -Uri "https://www.dev-c.com$file" -UserAgent $agent -Headers @{ Referer = $shvPage } -OutFile (Join-Path $temp $name)
    Write-Host "fetched $name"
}

$sdkZip = Get-ChildItem $temp -Filter "ScriptHookV_SDK_*.zip" | Select-Object -First 1
$runtimeZip = Get-ChildItem $temp -Filter "ScriptHookV_*.zip" | Where-Object { $_.Name -notmatch "SDK" } | Select-Object -First 1
Expand-Archive -Force $sdkZip.FullName (Join-Path $temp "sdk")
Expand-Archive -Force $runtimeZip.FullName (Join-Path $temp "rt")
Copy-Item (Join-Path $temp "sdk\inc\*.h") (Join-Path $root "shv")
Copy-Item (Join-Path $temp "sdk\lib\ScriptHookV.lib") (Join-Path $root "shv")
Copy-Item (Join-Path $temp "rt\bin\ScriptHookV.dll"), (Join-Path $temp "rt\bin\dinput8.dll") (Join-Path $root "runtime")
Set-Content -Encoding ascii (Join-Path $root "runtime\ScriptHookV.version.txt") $runtimeZip.Name

# The setup exe carries its DLLs as a zip appended to it
$setup = Join-Path $temp "ReShade_Setup_Addon.exe"
Invoke-WebRequest -UseBasicParsing -Uri "https://reshade.me/downloads/ReShade_Setup_${ReShadeVersion}_Addon.exe" -UserAgent $agent -Headers @{ Referer = "https://reshade.me/" } -OutFile $setup
Add-Type -AssemblyName System.IO.Compression
# The zip's offsets count from where the zip starts, not from the start of the exe, so it is cut out first: the end record
# says how long the central directory is and where in the zip it begins
$bytes = [IO.File]::ReadAllBytes($setup)
$end = -1
for ($i = $bytes.Length - 22; $i -ge 0; $i--) {
    if ($bytes[$i] -eq 0x50 -and $bytes[$i + 1] -eq 0x4B -and $bytes[$i + 2] -eq 5 -and $bytes[$i + 3] -eq 6) { $end = $i; break }
}
if ($end -lt 0) { throw "The ReShade setup holds no zip; was the download blocked?" }
$zipStart = $end - [BitConverter]::ToUInt32($bytes, $end + 12) - [BitConverter]::ToUInt32($bytes, $end + 16)
$stream = New-Object IO.MemoryStream($bytes, $zipStart, ($bytes.Length - $zipStart))
try {
    $archive = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Read)
    $entry = $archive.Entries | Where-Object { $_.FullName -eq "ReShade64.dll" }
    if (-not $entry) { throw "ReShade64.dll is not in the ReShade setup" }
    $out = [IO.File]::Create((Join-Path $root "runtime\ReShade64.dll"))
    try { $entry.Open().CopyTo($out) } finally { $out.Dispose() }
} finally {
    $stream.Dispose()
}

foreach ($header in "reshade.hpp", "reshade_api.hpp", "reshade_api_device.hpp", "reshade_api_pipeline.hpp", "reshade_api_resource.hpp",
                    "reshade_api_format.hpp", "reshade_events.hpp", "reshade_overlay.hpp") {
    Invoke-WebRequest -UseBasicParsing -Uri "https://raw.githubusercontent.com/crosire/reshade/v$ReShadeVersion/include/$header" -OutFile (Join-Path $root "reshade\$header")
}
foreach ($include in "ReShade.fxh", "ReShadeUI.fxh") {
    Invoke-WebRequest -UseBasicParsing -Uri "https://raw.githubusercontent.com/crosire/reshade-shaders/slim/Shaders/$include" -OutFile (Join-Path $root "fx\$include")
}

Remove-Item -Recurse -Force $temp
Get-ChildItem $root -Recurse -File | ForEach-Object { "{0,10}  {1}" -f $_.Length, $_.FullName.Substring($root.Length + 1) }
