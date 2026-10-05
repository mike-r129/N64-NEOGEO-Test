# Run a .z64 in standalone ares, capture ISViewer/stdout telemetry to a file,
# kill after a time limit. This is the automated pre-hardware gate (BizHawk
# cannot run libdragon-audio ROMs).
#
# Usage: powershell -File ps-ares-run.ps1 -Rom <path.z64> -Seconds 120 -Out ares-run.txt [-Ares <ares.exe>]
# The ares path comes from -Ares, else $env:ARES_EXE, else ares.exe on PATH.
param(
    [Parameter(Mandatory=$true)][string]$Rom,
    [int]$Seconds = 120,
    [string]$Out = "ares-run.txt",
    [string]$Ares = $env:ARES_EXE
)

# ONE EMULATOR AT A TIME (2026-09-22: a dozen parallel ares instances froze the
# whole machine). Refuse to start if ares is already running or a headless PC
# emu run is active in WSL; the named mutex closes the race between two
# near-simultaneous launches.
$mtx = New-Object System.Threading.Mutex($false, "Global\mvs64-one-emulator")
$got = $false
try { $got = $mtx.WaitOne(0) } catch [System.Threading.AbandonedMutexException] { $got = $true }
if (-not $got) { Write-Host "[ares-run] REFUSED: another emulator run holds the lock"; exit 2 }
$busy = Get-Process -Name ares -ErrorAction SilentlyContinue
if ($busy) {
    Write-Host "[ares-run] REFUSED: ares already running (pid $($busy.Id -join ','))"
    $mtx.ReleaseMutex(); exit 2
}
$wslEmu = $null
try { $wslEmu = (& wsl.exe -e pgrep -x emu) } catch { }
if ($wslEmu) {
    Write-Host "[ares-run] REFUSED: a PC emu run is active in WSL (pid $wslEmu)"
    $mtx.ReleaseMutex(); exit 2
}

$ErrorActionPreference = "Stop"
if (-not $Ares) {
    $cmd = Get-Command ares.exe -ErrorAction SilentlyContinue
    if ($cmd) { $Ares = $cmd.Source }
}
if (-not $Ares -or -not (Test-Path $Ares)) { $mtx.ReleaseMutex(); throw "ares not found: pass -Ares or set ARES_EXE" }
if (-not (Test-Path $Rom))  { $mtx.ReleaseMutex(); throw "ROM not found: $Rom" }

$outPath = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path (Get-Location) $Out }
$errPath = "$outPath.err"

Write-Host "[ares-run] launching ares: $Rom  (limit ${Seconds}s, log $outPath)"
# WindowStyle Minimized: emulator windows must never spawn in the way or
# steal focus.
$p = Start-Process -FilePath $Ares -ArgumentList "`"$Rom`"" `
     -RedirectStandardOutput $outPath -RedirectStandardError $errPath `
     -PassThru -WindowStyle Minimized

$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
    if ($p.HasExited) { Write-Host "[ares-run] ares exited early (code $($p.ExitCode))"; break }
    Start-Sleep -Seconds 5
}
if (-not $p.HasExited) {
    # CloseMainWindow first: a clean quit flushes ares' block-buffered stdout
    # (force-kill loses the last ~16KB of telemetry).
    $null = $p.CloseMainWindow()
    if (-not $p.WaitForExit(8000)) { $p.Kill(); $p.WaitForExit() | Out-Null }
    Write-Host "[ares-run] ares stopped after ${Seconds}s"
}
$size = (Get-Item $outPath -ErrorAction SilentlyContinue).Length
Write-Host "[ares-run] telemetry: $outPath ($size bytes)"
$mtx.ReleaseMutex()
