# Starts the PC build recording player 1's controller to padrec\<name>.txt (runtime/padrec.c).
#   powershell -File tools\padrec.ps1 -name race1 [-run build-win\run]
# Play normally from the title screen, then quit with the window's close button or the menu's EXIT GAME
# (the last seconds are written at exit). Replay: tools\padplay.ps1 -name race1
param([Parameter(Mandatory = $true)][string]$name, [string]$run = "")
$ErrorActionPreference = "Stop"
if ($name -notmatch '^[A-Za-z0-9_-]{1,64}$') { "name: letters, digits, - and _ only"; exit 1 }
if (-not $run) { $run = Join-Path $PSScriptRoot "..\build-win\run" }
$run = (Resolve-Path $run).Path
$env:MKDD_PADREC = $name
try { $p = Start-Process -FilePath (Join-Path $run "MKDD-NX.exe") -WorkingDirectory $run -PassThru } finally { Remove-Item Env:MKDD_PADREC }
"recording to $run\padrec\$name.txt (pid $($p.Id)); close the game when done"
$p.WaitForExit()
# the launcher starts the game again as a new process (which records): wait for that one too
$exe = Join-Path $run "MKDD-NX.exe"
while (Get-Process MKDD-NX -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) { Start-Sleep -Seconds 2 }
$f = Join-Path $run "padrec\$name.txt"
if (Test-Path $f) { "saved $f ($((Get-Item $f).Length) bytes)" } else { "nothing recorded: see $run\logs\mkdd.log" }
