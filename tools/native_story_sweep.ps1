# Loads each story stage from a patched sandbox save (u16 stage at 0xC52, entry at 0xC54, #140)
# and reports what the native renderer did there. -Backend d3d12 gives the same spots as a baseline.
# Usage: powershell -File tools\native_story_sweep.ps1 -Prefix sw [-From 1] [-To 58] [-Backend vulkan]
param([string]$Prefix = 'story', [int]$From = 1, [int]$To = 58, [ValidateSet('d3d12', 'vulkan')] [string]$Backend = 'vulkan', [int]$Entry = 0)
$root = Split-Path $PSScriptRoot -Parent
$csv = Join-Path $root "notes\logs\native\$Prefix.csv"
'stage,status,fps,gpu_ms,textures,tex_miss,no_vs,no_ps,no_pipe,no_target,bad_prim,reason' | Set-Content $csv -Encoding ascii
foreach ($s in $From..$To) {
  $hex = '{0:X2}' -f $s; $name = "$Prefix-$hex"
  $patch = "ng2stryd23.dat:0xC53=$hex,ng2stryd23.dat:0xC54=$('{0:X2}' -f ($Entry -shr 8)),ng2stryd23.dat:0xC55=$('{0:X2}' -f ($Entry -band 255))"
  $cv = if ($Backend -eq 'vulkan') { @('-Cvars', 'ng2_native_d3d=true') } else { @() }
  $line = & powershell -NoProfile -NonInteractive -File (Join-Path $PSScriptRoot 'native_test.ps1') -Name $name -Route gameplay -Backend $Backend -MeasureSec 5 -TimeoutSec 40 @cv -SavePatch $patch 2>&1 | Where-Object { "$_" -match '^(PASS|FAIL) ' } | Select-Object -First 1
  $text = "$line"
  $log = Join-Path $root "notes\logs\native\$name\run.log"
  $f = if (Test-Path $log) { Select-String -Path $log -Pattern 'NATIVE frames' -Encoding UTF8 | Select-Object -Last 1 } else { $null }
  $get = { param($k) if ($f -and $f.Line -match "$k=(\d+)") { $Matches[1] } else { '' } }
  $status = if ($text -match '^PASS') { 'PASS' } else { 'FAIL' }
  $fps = if ($text -match 'fps=([\d.na/]+)') { $Matches[1] } else { '' }
  $gpu = if ($text -match 'gpu=([\d.na/]+)') { $Matches[1] } else { '' }
  $reason = if ($text -match 'MB (.*)$') { ($Matches[1] -replace ',', ';') } else { '' }
  "$hex,$status,$fps,$gpu,$(& $get 'textures'),$(& $get 'tex_miss'),$(& $get 'no_vs'),$(& $get 'no_ps'),$(& $get 'no_pipe'),$(& $get 'no_target'),$(& $get 'bad_prim'),$reason" | Add-Content $csv -Encoding ascii
}
