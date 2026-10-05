# Run several ares jobs STRICTLY ONE AT A TIME (user rule: never more than one
# emulator). Each job goes through the guarded ps-ares-run.ps1 and must exit
# before the next starts; a refused launch (exit 2) stops the queue.
#
# Usage: powershell -File ps-ares-queue.ps1 -JobList "C:\a.z64|C:\a.txt|480;C:\b.z64|C:\b.txt|480"
param([Parameter(Mandatory=$true)][string]$JobList)
$Jobs = @($JobList.Split(';') | Where-Object { $_.Trim() })
$runner = Join-Path $PSScriptRoot "ps-ares-run.ps1"
$i = 0
foreach ($j in $Jobs) {
    $i++
    $rom, $out, $sec = $j.Trim().Split('|')
    if (-not $sec) { $sec = 480 }
    Write-Host "[ares-queue] job $i/$($Jobs.Count): $rom (${sec}s)"
    & powershell -NoProfile -File $runner -Rom $rom -Seconds ([int]$sec) -Out $out
    if ($LASTEXITCODE -eq 2) { Write-Host "[ares-queue] launch refused - stopping queue"; exit 2 }
    # Belt and braces: never start the next job while any ares is still alive.
    $t0 = Get-Date
    while (Get-Process -Name ares -ErrorAction SilentlyContinue) {
        if (((Get-Date) - $t0).TotalSeconds -gt 60) { Write-Host "[ares-queue] ares did not exit - stopping queue"; exit 3 }
        Start-Sleep -Seconds 2
    }
}
Write-Host "[ares-queue] all $($Jobs.Count) jobs done"
