# Installs the host side into GTA V (story mode), Legacy or Enhanced: ScriptHookV and its ASI loader, GtrHost.asi, and
# ReShade with the compositor add-on and effect. It only adds files, lists them in gtr_installed.txt, and -Remove deletes
# exactly those. Run fetch_deps.ps1 and build.ps1 first.
#   -Edition     legacy or enhanced; needed only when both are installed and -GtaDir isn't given
#   -GtaDir      the folder with GTA5.exe or GTA5_Enhanced.exe (default: found in the Steam libraries)
#   -ReShadeAs   asi:  ReShade is loaded by the ASI loader. Legacy's default: it takes the system's dxgi.dll over one
#                      beside the game
#                dxgi: ReShade is the dxgi.dll beside the game. Enhanced's default: seen to load there
#   -Force       replace files of the same names that this script didn't install
#   -Remove      take everything this script installed out again
param(
    [ValidateSet("legacy", "enhanced")][string]$Edition,
    [string]$GtaDir,
    [ValidateSet("dxgi", "asi")][string]$ReShadeAs,
    [switch]$Force,
    [switch]$Remove
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$third = Join-Path $root "third_party"
$built = Join-Path $root "build\gta"
$manifestName = "gtr_installed.txt"
$editions = @{
    legacy = @{ Folder = "Grand Theft Auto V"; Exe = "GTA5.exe"; Process = "GTA5"; ReShadeAs = "asi" }
    enhanced = @{ Folder = "Grand Theft Auto V Enhanced"; Exe = "GTA5_Enhanced.exe"; Process = "GTA5_Enhanced"; ReShadeAs = "dxgi" }
}
# BattlEye off, which ScriptHookV needs and which keeps GTA Online from starting
$battlEyeOff = "-nobattleye -noBE"

function Find-Gta($name) {
    $steams = @("${env:ProgramFiles(x86)}\Steam", "$env:ProgramFiles\Steam") | Where-Object { Test-Path "$_\steamapps\libraryfolders.vdf" }
    foreach ($steam in $steams) {
        $libraries = @($steam) + (Select-String -Path "$steam\steamapps\libraryfolders.vdf" -Pattern '"path"\s+"(.+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' })
        foreach ($library in $libraries) {
            # Joined by hand: a library on a drive that is no longer there makes Join-Path fail
            $candidate = "$library\steamapps\common\$($editions[$name].Folder)"
            if (Test-Path "$candidate\$($editions[$name].Exe)") { return $candidate }
        }
    }
    return $null
}

if ($GtaDir) {
    $found = @($editions.Keys | Where-Object { Test-Path (Join-Path $GtaDir $editions[$_].Exe) })
    if (-not $found) { throw "neither GTA5.exe nor GTA5_Enhanced.exe is in $GtaDir" }
    if (-not $Edition) { $Edition = $found[0] }
} else {
    $found = @($(if ($Edition) { $Edition } else { $editions.Keys }) | Where-Object { Find-Gta $_ })
    if (-not $found) { throw "GTA V not found in the Steam libraries; pass -GtaDir, the folder with GTA5.exe or GTA5_Enhanced.exe" }
    if ($found.Count -gt 1) { throw "both editions are installed; pass -Edition legacy or -Edition enhanced" }
    $Edition = $found[0]
    $GtaDir = Find-Gta $Edition
}
$game = $editions[$Edition]
if (-not (Test-Path (Join-Path $GtaDir $game.Exe))) { throw "$($game.Exe) is not in $GtaDir" }
if (-not $ReShadeAs) { $ReShadeAs = $game.ReShadeAs }
if (Get-Process $game.Process -ErrorAction SilentlyContinue) { throw "GTA V is running; close it first" }
$manifest = Join-Path $GtaDir $manifestName
$installed = @(if (Test-Path $manifest) { Get-Content $manifest })

if ($Remove) {
    foreach ($file in $installed) {
        $path = Join-Path $GtaDir $file
        if (Test-Path $path) { Remove-Item $path -Force; Write-Host "removed $file" }
    }
    foreach ($folder in "reshade-shaders\Shaders", "reshade-shaders") {
        $path = Join-Path $GtaDir $folder
        if ((Test-Path $path) -and -not (Get-ChildItem $path)) { Remove-Item $path }
    }
    if (Test-Path $manifest) { Remove-Item $manifest }
    Write-Host "removed from $GtaDir (the logs the game wrote there, ReShade.log, asiloader.log and ScriptHookV.log, are left)"
    return
}

# source -> name in the game folder
$files = [ordered]@{
    (Join-Path $third "runtime\ScriptHookV.dll") = "ScriptHookV.dll"
    (Join-Path $third "runtime\dinput8.dll") = "dinput8.dll"
    (Join-Path $built "GtrHost.asi") = "GtrHost.asi"
    (Join-Path $built "GtrCompositor.addon64") = "GtrCompositor.addon64"
    (Join-Path $third "runtime\ReShade64.dll") = $(if ($ReShadeAs -eq "dxgi") { "dxgi.dll" } else { "ReShade64.asi" })
    (Join-Path $PSScriptRoot "shaders\GtrPassthrough.fx") = "reshade-shaders\Shaders\GtrPassthrough.fx"
    (Join-Path $third "fx\ReShade.fxh") = "reshade-shaders\Shaders\ReShade.fxh"
    (Join-Path $third "fx\ReShadeUI.fxh") = "reshade-shaders\Shaders\ReShadeUI.fxh"
}
# Where GTA's sun is through its day, once host\sun_calibrate.py has measured it; without it the guest casts no sun shadows
$sun = Join-Path $PSScriptRoot "GtrSun.txt"
if (Test-Path $sun) { $files[$sun] = "GtrSun.txt" }
$written = @("ReShade.ini", "ReShadePreset.ini")
foreach ($source in $files.Keys) {
    if (-not (Test-Path $source)) { throw "$source is missing: run fetch_deps.ps1 and build.ps1 first" }
}

# ScriptHookV refuses game builds it doesn't know, so say so here instead of in a message box in the game
$version = (Get-Item (Join-Path $GtaDir $game.Exe)).VersionInfo.FileVersion
$hook = Get-Content (Join-Path $third "runtime\ScriptHookV.version.txt") -ErrorAction SilentlyContinue
$build = ($version -split '\.')[2..3] -join '.'
if ($hook -and $hook -notmatch [regex]::Escape($build)) { Write-Warning "the game is $version but this ScriptHookV is $hook; it may refuse to load" }

# Legacy reads its launch arguments from args.txt, which this script writes when there is none; Enhanced from
# commandline.txt, which is the owner's to set
$writesArguments = $false
if ($Edition -eq "legacy") {
    $arguments = Join-Path $GtaDir "args.txt"
    $writesArguments = -not (Test-Path $arguments) -or ($installed -contains "args.txt")
    if ($writesArguments) { $written += "args.txt" }
    elseif ((Get-Content $arguments -Raw) -notmatch "-nobattleye") { Write-Warning "args.txt has no -nobattleye: ScriptHookV only runs with BattlEye off (story mode only)" }
} else {
    $arguments = Join-Path $GtaDir "commandline.txt"
    if (-not (Test-Path $arguments) -or (Get-Content $arguments -Raw) -notmatch "-nobattleye") {
        Write-Warning "commandline.txt has no -nobattleye: ScriptHookV only runs with BattlEye off (story mode only)"
    }
}

# Someone else's loader, ReShade or settings stay unless -Force; a previous install by this script is replaced freely
$clashes = @($files.Values + $written | Where-Object { (Test-Path (Join-Path $GtaDir $_)) -and ($installed -notcontains $_) })
if ($clashes -and -not $Force) { throw "not replacing what is already in ${GtaDir}: $($clashes -join ', ') (-Force to replace)" }
# The other way of loading ReShade, left by an earlier install, would load it twice
foreach ($stale in @($installed | Where-Object { $_ -in "dxgi.dll", "ReShade64.asi" -and $files.Values -notcontains $_ })) {
    Remove-Item (Join-Path $GtaDir $stale) -Force
}

New-Item -ItemType Directory -Force (Join-Path $GtaDir "reshade-shaders\Shaders") | Out-Null
foreach ($source in $files.Keys) {
    Copy-Item $source (Join-Path $GtaDir $files[$source]) -Force
}
Set-Content -Encoding ascii (Join-Path $GtaDir "ReShade.ini") "[GENERAL]`r`nEffectSearchPaths=.\reshade-shaders\Shaders`r`nTextureSearchPaths=.\reshade-shaders\Textures`r`nPresetPath=.\ReShadePreset.ini`r`n`r`n[OVERLAY]`r`nTutorialProgress=4`r`nShowClock=0`r`nShowFPS=0`r`n"
# The technique is always on; the add-on's GtrActive uniform keeps it a plain passthrough until guest frames arrive
Set-Content -Encoding ascii (Join-Path $GtaDir "ReShadePreset.ini") "Techniques=GtrPassthrough@GtrPassthrough.fx`r`nTechniqueSorting=GtrPassthrough@GtrPassthrough.fx`r`n"
if ($writesArguments) { Set-Content -Path (Join-Path $GtaDir "args.txt") -Value $battlEyeOff -Encoding ascii -NoNewline }
Set-Content -Encoding ascii $manifest (@($files.Values) + $written)
Write-Host "installed into $GtaDir ($Edition $version, ReShade as $ReShadeAs):"
@($files.Values) + $written | ForEach-Object { Write-Host "  $_" }
