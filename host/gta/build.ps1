# Builds the host side with MSVC into build\gta\:
#   GtrCompositor.addon64   the ReShade add-on (src\compositor.cpp)
#   GtrHost.asi             the ScriptHookV script (src\script.cpp)
#   fakegta\                the stand-in host (tests\fakegta.cpp) with ReShade, the add-on and the effect set up beside it
# Run fetch_deps.ps1 first.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$third = Join-Path $root "third_party"
$out = Join-Path $root "build\gta"
$fake = Join-Path $out "fakegta"
if (-not (Test-Path (Join-Path $third "reshade\reshade.hpp"))) { throw "third_party is missing: run fetch_deps.ps1 first" }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "no Visual Studio with the C++ x64 tools" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

foreach ($folder in $out, $fake, (Join-Path $out "obj"), (Join-Path $fake "reshade-shaders\Shaders")) {
    New-Item -ItemType Directory -Force $folder | Out-Null
}

$common = "/nologo /O2 /EHsc /std:c++20 /MT /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I `"$root\shared`" /Fo`"$out\obj\\`""
$addon = "cl $common /LD /I `"$third\reshade`" `"$PSScriptRoot\src\compositor.cpp`" /Fe`"$out\GtrCompositor.addon64`" /link user32.lib"
$script = "cl $common /LD /I `"$third\shv`" `"$PSScriptRoot\src\script.cpp`" `"$PSScriptRoot\src\link.cpp`" /Fe`"$out\GtrHost.asi`" /link `"$third\shv\ScriptHookV.lib`" ws2_32.lib user32.lib"
$test = "cl $common `"$PSScriptRoot\tests\fakegta.cpp`" `"$PSScriptRoot\src\link.cpp`" /Fe`"$fake\fakegta.exe`""
cmd /c "`"$vcvars`" >nul && $addon && $script && $test"
if ($LASTEXITCODE -ne 0) { throw "build failed" }

# For an ordinary program ReShade loads as the dxgi.dll in its folder, and loads the add-ons beside itself
Copy-Item (Join-Path $third "runtime\ReShade64.dll") (Join-Path $fake "dxgi.dll") -Force
Copy-Item (Join-Path $out "GtrCompositor.addon64") $fake -Force
Copy-Item (Join-Path $PSScriptRoot "shaders\GtrPassthrough.fx"), (Join-Path $third "fx\ReShade.fxh"), (Join-Path $third "fx\ReShadeUI.fxh") (Join-Path $fake "reshade-shaders\Shaders") -Force
# The technique is always on; the add-on's GtrActive uniform keeps it a plain passthrough until guest frames arrive
Set-Content -Encoding ascii (Join-Path $fake "ReShade.ini") "[GENERAL]`r`nEffectSearchPaths=.\reshade-shaders\Shaders`r`nTextureSearchPaths=.\reshade-shaders\Textures`r`nPresetPath=.\ReShadePreset.ini`r`n`r`n[OVERLAY]`r`nTutorialProgress=4`r`nShowClock=0`r`nShowFPS=0`r`n"
# The stand-in's test is of where the guest shows and where it is hidden, pixel for pixel, so the relighting, soft edges and
# contact shadows that change colours near the guest are off there
Set-Content -Encoding ascii (Join-Path $fake "ReShadePreset.ini") "Techniques=GtrPassthrough@GtrPassthrough.fx`r`nTechniqueSorting=GtrPassthrough@GtrPassthrough.fx`r`n`r`n[GtrPassthrough.fx]`r`nLightMatch=0.000000`r`nGradeMatch=0.000000`r`nHazeStrength=0.000000`r`nEdgeSoftness=0.000000`r`nContactShadow=0.000000`r`nLampShadows=0.000000`r`n"
Write-Host "built $out"
