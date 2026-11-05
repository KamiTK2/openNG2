# Build ng2, run one native test, print the result and the renderer's log lines.
# Usage: powershell -File tools\native_iter.ps1 -Name x [-Route gameplay] [-Sec 8] [-Extra 'a=1,b=2'] [-NoBuild]
param([Parameter(Mandatory)][string]$Name, [string]$Route = 'gameplay', [int]$Sec = 8, [string]$Extra = '', [switch]$NoBuild)
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
if (-not $NoBuild) {
  $b = & cmd /c "project\ng2\build.cmd" 2>&1
  $bad = $b | Select-String -Pattern ' error|FAILED'
  if ($bad) { $bad | Select-Object -First 15 | ForEach-Object { $_.Line.Substring(0, [math]::Min(220, $_.Line.Length)) }; exit 1 }
}
$cv = 'ng2_native_d3d=true'; if ($Extra) { $cv = "$cv,$Extra" }
& powershell -NoProfile -File tools\native_test.ps1 -Name $Name -Route $Route -Backend vulkan -Cvars $cv -MeasureSec $Sec -TimeoutSec 40 | Select-Object -Last 2
$log = "notes\logs\native\$Name\run.log"
$l = Select-String -Path $log -Pattern 'NG2 NATIVE|ERROR|\[error\]|\[warn' -Encoding UTF8
"--- log lines: $($l.Count)"
$l | Where-Object { $_.Line -notmatch 'NATIVE DRAW|NATIVE PASS|NATIVE swaps' } | Select-Object -First 30 | ForEach-Object { $_.Line.Substring(0, [math]::Min(240, $_.Line.Length)) }
"--- last"
$l | Where-Object { $_.Line -match 'NATIVE frames' } | Select-Object -Last 1 | ForEach-Object { $_.Line }
