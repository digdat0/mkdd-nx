# Replays a controller recording (runtime/padrec.c) in the PC build and prints the perf log lines.
#   powershell -File tools\padplay.ps1 -name race1 [-run build-win\run] [-extra 60]
# Records are made by playing with MKDD_PADREC=<name> set (tools\padrec.ps1). The game runs in its own
# window and needs no focus: the recording replaces player 1's controller and keyboard.
param([Parameter(Mandatory = $true)][string]$name, [string]$run = "", [int]$extra = 60)
$ErrorActionPreference = "Stop"
if (-not $run) { $run = Join-Path $PSScriptRoot "..\build-win\run" }
$run = (Resolve-Path $run).Path
$exe = Join-Path $run "MKDD-NX.exe"
$file = Join-Path $run "padrec\$name.txt"
if (-not (Test-Path $file)) { "no recording $file"; exit 1 }
# its length: frames at 60 a second, plus loads
$frames = 0
foreach ($l in [IO.File]::ReadLines($file)) {
  if ($l.Length -eq 0 -or $l[0] -eq '#' -or $l[0] -eq '@') { continue }
  $frames += [int]($l.Split(' ')[0])
}
$secs = [int]($frames / 60) + $extra
"replaying $name ($frames frames, up to $secs s)"
$log = Join-Path $run "logs\mkdd.log"
$env:MKDD_PADPLAY = $name
try { $p = Start-Process -FilePath $exe -ArgumentList "--play" -WorkingDirectory $run -PassThru } finally { Remove-Item Env:MKDD_PADPLAY }
$t0 = Get-Date
$done = $false
while (((Get-Date) - $t0).TotalSeconds -lt $secs -and -not $p.HasExited) {
  Start-Sleep -Seconds 2
  if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $t0 -and (Select-String -Path $log -Pattern "padplay: done" -SimpleMatch -Quiet)) { $done = $true; break }
}
Start-Sleep -Seconds 2
if (-not $p.HasExited) { $p | Stop-Process }
if (-not $done) { "padplay: did not finish in $secs s" }
Select-String -Path $log -Pattern "padplay:|vcache:|vjobs:|hitch" | ForEach-Object { $_.Line }
$trace = Join-Path $run "logs\mkdd_trace.log"   # pacing and game core lines
if (Test-Path $trace) { Select-String -Path $trace -Pattern "game core|pacing:" | ForEach-Object { $_.Line } }
