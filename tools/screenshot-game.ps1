# Saves the game's finished picture (GTA with the guest composited in) as a PNG, by way of the compositor add-on.
# The game only draws while it is in front, and may sit in its pause menu when it comes back, where scripts don't run and
# the guest is hidden. So this brings the game forward, presses Esc only if the script isn't running, takes the picture,
# and gives the window that was in front its place back.
#   -Out      where the PNG goes (default out\game\<time>.png)
#   -Send     JSON messages for the guest's link, sent once the game is running: {"t":"host",...} ones reach the script
#   -SendFile the same, one a line in a file: the way to pass them from another shell, which would eat their quotes
#   -Wait     seconds between sending and taking the picture
param(
    [string]$Out,
    [string[]]$Send = @(),
    [string]$SendFile,
    [double]$Wait = 1.5,
    [int]$SettleSeconds = 3
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$python = Join-Path $root ".venv\Scripts\python.exe"
if (-not $Out) { $Out = Join-Path $root ("out\game\{0}.png" -f (Get-Date -Format "HHmmss")) }
New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null

Add-Type @"
using System; using System.Runtime.InteropServices;
public class GtrWindows {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
  public static uint ForegroundProcess() { uint process; GetWindowThreadProcessId(GetForegroundWindow(), out process); return process; }
  public static void Hold(byte vk, byte scan, int milliseconds) {
    keybd_event(vk, scan, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(milliseconds); keybd_event(vk, scan, 2, UIntPtr.Zero);
  }
}
"@

# Whether the host's script is ticking, which it only does while the game is unpaused: its heartbeat moves
function Test-ScriptRunning {
    & $python (Join-Path $root "host\capture.py") --running | Out-Null
    return $LASTEXITCODE -eq 0
}

$game = Get-Process GTA5, GTA5_Enhanced -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $game) { throw "GTA V isn't running" }
$before = [GtrWindows]::ForegroundProcess()
$shell = New-Object -ComObject WScript.Shell
$wasInFront = $before -eq $game.Id
try {
    if (-not $wasInFront) {
        [void]$shell.AppActivate($game.Id)
        Start-Sleep -Seconds $SettleSeconds
        if ([GtrWindows]::ForegroundProcess() -ne $game.Id) { throw "Windows didn't bring the game to the front" }
    }
    if (-not (Test-ScriptRunning)) {
        [GtrWindows]::Hold(0x1B, 0x01, 150)
        Start-Sleep -Seconds $SettleSeconds
        if (-not (Test-ScriptRunning)) { throw "the game's script isn't running (still paused, or not in story mode)" }
    }
    if ($SendFile) { $Send = @(Get-Content $SendFile) }
    if ($Send) {
        $Send | & $python (Join-Path $root "host\send.py")
        Start-Sleep -Milliseconds ([int]($Wait * 1000))
    }
    & $python (Join-Path $root "host\capture.py") $Out
    if ($LASTEXITCODE -ne 0) { throw "the compositor saved no picture" }
} finally {
    if (-not $wasInFront) { Start-Sleep -Milliseconds 300; [void]$shell.AppActivate([int]$before) }
}
