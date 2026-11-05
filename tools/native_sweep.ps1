# Load a list of stages on the native path and on D3D12 (ng2_autopilot_stage) and compare.
# Usage: powershell -File tools\native_sweep.ps1 -Name a -Stages 0x05,0x09 [-Scale 1] [-NativeOnly]
# Writes notes\logs\native\<name>_sweep.csv: stage, both statuses, MAE, miss counters, errors.
param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][string[]]$Stages, [int]$Scale = 1,
      [int]$Sec = 5, [switch]$NativeOnly)
# Stages load through the first Mission Mode entry, which spawns the player at the stage default.
$route = 'ng2_autopilot_menu_item=2,ng2_autopilot_script=wwawwawwa'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$csv = "notes\logs\native\${Name}_sweep.csv"
'stage,native,d3d12,mae,fps,gpu,no_vs,no_ps,no_pipe,no_target,bad_prim,tex_miss,skipped,errors' | Set-Content $csv -Encoding ascii
foreach ($s in ($Stages | ForEach-Object { $_ -split ',' } | Where-Object { $_ })) {
  $id = [Convert]::ToInt32($s, 16)
  $n = '{0}-n{1:x3}' -f $Name, $id
  $d = '{0}-d{1:x3}' -f $Name, $id
  $r1 = & powershell -NoProfile -File tools\native_test.ps1 -Name $n -Route gameplay -Backend vulkan -Scale $Scale -MeasureSec $Sec -TimeoutSec 25 -Cvars "ng2_native_d3d=true,$route,ng2_autopilot_stage=$id" | Select-Object -First 1
  $st1 = if ($r1 -like 'PASS*') { 'PASS' } else { 'FAIL' }
  $fps = if ($r1 -match 'fps=([\d.]+)') { $Matches[1] } else { '' }
  $gpu = if ($r1 -match 'gpu=([\d.]+)') { $Matches[1] } else { '' }
  $log = "notes\logs\native\$n\run.log"
  $last = (Select-String -Path $log -Pattern 'NATIVE frames' -Encoding UTF8 | Select-Object -Last 1).Line
  $get = { param($k) if ($last -match "$k=(\d+)") { $Matches[1] } else { '' } }
  $err = @(Select-String -Path $log -Pattern '\[error\]' -Encoding UTF8).Count
  $st2 = ''; $mae = ''
  if (-not $NativeOnly) {
    $r2 = & powershell -NoProfile -File tools\native_test.ps1 -Name $d -Route gameplay -Backend d3d12 -Scale $Scale -MeasureSec $Sec -TimeoutSec 30 -Cvars "$route,ng2_autopilot_stage=$id" | Select-Object -First 1
    $st2 = if ($r2 -like 'PASS*') { 'PASS' } else { 'FAIL' }
    if ((Test-Path "notes\logs\native\$n\shot1.png") -and (Test-Path "notes\logs\native\$d\shot1.png")) {
      $o = & powershell -NoProfile -File tools\native_diff.ps1 -A "notes\logs\native\$d\shot1.png" -B "notes\logs\native\$n\shot1.png" | Out-String
      if ($o -match '(?i)mae[^\d]*([\d.]+)') { $mae = $Matches[1] }
    }
  }
  "$s,$st1,$st2,$mae,$fps,$gpu,$(& $get 'no_vs'),$(& $get 'no_ps'),$(& $get 'no_pipe'),$(& $get 'no_target'),$(& $get 'bad_prim'),$(& $get 'tex_miss'),$(& $get 'skipped'),$err" | Add-Content $csv -Encoding ascii
  Get-Content $csv -Tail 1
}
