# Fast unattended test run: launch, let the in-process autopilot (project\ng2\src\ng2_autopilot.cpp)
# reach the goal from what the game shows, measure, screenshot, close. No window focus or
# simulated key presses are needed, and a timeout stops a hang.
#
# The game runs on a sandbox copy of Documents\ng2 (--user_data_root), so the real settings and
# saves are never swapped. A dated backup is taken first and the real folder is fingerprinted
# before and after; if anything in it changed the run reports ALERT (exit 2).
#
# Usage: powershell -File tools\native_test.ps1 -Name t1 [-Route gameplay|menu|options]
#          [-Backend d3d12|vulkan] [-Scale 1] [-MeasureSec 15] [-TimeoutSec 60]
#          [-Cvars 'a=1','b=true'] [-PulseMs 50] [-Shots 1] [-Movies]
# Results: one line per run in notes\logs\native\results.csv; files in notes\logs\native\<name>\.
param(
  [Parameter(Mandatory)] [string]$Name,
  [ValidateSet('gameplay', 'menu', 'options', 'cutscene')] [string]$Route = 'gameplay',
  [ValidateSet('d3d12', 'vulkan')] [string]$Backend = 'd3d12',
  [int]$Scale = 1,
  [int]$MeasureSec = 15,
  [int]$TimeoutSec = 60,
  [string[]]$Cvars = @(),
  [switch]$Movies,   # let the boot movie play (it is skipped by the autopilot's Start) instead of Skip Intro
  [int]$PulseMs = 0,
  [int]$Shots = 1,
  [string]$Preset = 'win-amd64-relwithdebinfo'
)
$ErrorActionPreference = 'Stop'
# powershell -File passes 'a=1,b=2' as one string
$Cvars = @($Cvars | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root "project\ng2\out\build\$Preset"
$native = Join-Path $root 'notes\logs\native'
$out = Join-Path $native $Name
$log = Join-Path $out 'run.log'
$real = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'ng2'
$sandbox = Join-Path $env:LOCALAPPDATA 'openNG2-test\userdata'
$stamp = Get-Date -Format 'yyyyMMdd'
$backup = Join-Path $root "notes\backups\native-test-$stamp"

function Fail-Early($msg) { "FAIL $Name : $msg"; exit 1 }
function Robo($from, $to, [string[]]$more) {
  & robocopy $from $to /NFL /NDL /NJH /NJS /NP /R:1 /W:1 @more | Out-Null
  if ($LASTEXITCODE -ge 8) { Fail-Early "robocopy $from -> $to failed ($LASTEXITCODE)" }
  $global:LASTEXITCODE = 0
}
function Fingerprint {
  $files = Get-ChildItem $real -Recurse -File |
    Where-Object { $_.FullName -notlike "$real\cache\*" -and $_.FullName -notlike "$real\save_backups\*" } |
    Sort-Object FullName
  $text = ($files | ForEach-Object { "$($_.FullName)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }) -join "`n"
  $sha = [System.Security.Cryptography.SHA256]::Create()
  [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)))
}

# Guards
$free = (Get-PSDrive C).Free
if ($free -lt 2GB) { Fail-Early "under 2 GB free on C:" }
if (Get-Process ng2 -ErrorAction SilentlyContinue) { Fail-Early 'ng2.exe is already running' }
if (-not (Test-Path (Join-Path $bin 'ng2.exe'))) { Fail-Early "no ng2.exe in $bin" }

New-Item -ItemType Directory -Force $out, $sandbox | Out-Null
Remove-Item "$out\*" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $bin 'ng2_crash.dmp') -ErrorAction SilentlyContinue

# Backup once a day, then refresh the sandbox from the real folder (shader cache only the first time)
$settingsReal = Join-Path $real 'recomp_settings.toml'
if (-not (Test-Path (Join-Path $backup 'recomp_settings.toml'))) {
  Robo $real $backup @('/E', '/XD', 'cache', 'save_backups')
  if ((Get-FileHash $settingsReal).Hash -ne (Get-FileHash (Join-Path $backup 'recomp_settings.toml')).Hash) {
    Fail-Early 'settings backup does not match the original'
  }
}
$firstCopy = -not (Test-Path (Join-Path $sandbox 'cache'))
$xd = @('/XD', 'save_backups'); if (-not $firstCopy) { $xd += 'cache' }
Robo $real $sandbox (@('/MIR') + $xd)
$before = Fingerprint
$settingsHash = (Get-FileHash $settingsReal).Hash

# Test settings: the user's file with the display / audio / update keys pinned
$overrides = [ordered]@{
  fullscreen = 'false'; resolution_scale = "$Scale"; frame_generation = 'false'
  present_half_rate = 'false'; present_effect = '"none"'; swap_post_effect = '"none"'
  ng2_framerate_cap = '60'; ng2_check_updates = 'false'; ng2_pause_in_background = 'false'
  ng2_master_volume = '0'; ng2_skip_intro = 'true'; ng2_skip_any_cutscene = 'true'
  ng2_fast_loading = 'true'; ng2_window_width = '1280'; ng2_window_height = '720'
  ng2_window_x = '0'; ng2_window_y = '0'
}
if ($Movies) { $overrides['ng2_skip_intro'] = 'false' }
if ($Route -eq 'cutscene') { $overrides['ng2_skip_any_cutscene'] = 'false' }
$settingsFile = Join-Path $sandbox 'recomp_settings.toml'
$lines = Get-Content $settingsFile | Where-Object { $overrides.Keys -notcontains (($_ -split '=')[0].Trim()) }
$lines += $overrides.GetEnumerator() | ForEach-Object { "$($_.Key) = $($_.Value)" }
[IO.File]::WriteAllLines($settingsFile, [string[]]$lines)

# Launch
$argList = @(
  "--game_data_root=`"$root\game`"", "--user_data_root=`"$sandbox`"", "--log_file=`"$log`"",
  '--log_level=info', "--ng2_autopilot=$Route", "--gpu_backend=$Backend",
  $(if ($Backend -eq 'vulkan') { '--ng2_gpu_frame_time=true' } else { '--diag_gpu_passes=30' })
)
if ($PulseMs -gt 0) { $argList += "--ng2_autopilot_pulse_ms=$PulseMs" }
$argList += $Cvars | ForEach-Object { "--$_" }

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class NT {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
'@
function Shot($tag) {
  $p.Refresh(); $h = $p.MainWindowHandle
  if ($h -eq [IntPtr]::Zero) { return }
  $r = New-Object NT+RECT; [void][NT]::GetWindowRect($h, [ref]$r)
  $w = $r.R - $r.L; $hh = $r.B - $r.T
  if ($w -le 0 -or $hh -le 0) { return }
  $bmp = New-Object System.Drawing.Bitmap $w, $hh
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); [void][NT]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc)
  $bmp.Save((Join-Path $out "$tag.png"), [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
}
function Read-Log {
  if (-not (Test-Path $log)) { return @() }
  try {
    $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite,Delete')
    $sr = New-Object IO.StreamReader $fs
    $text = $sr.ReadToEnd(); $sr.Close(); $fs.Close()
    return $text -split "`r?`n"
  } catch { return @() }
}

$status = 'FAIL'; $reason = ''; $goalSec = -1; $fps = 'n/a'; $gpu = 'n/a'; $exitCode = 1
$smi = $null; $smiFile = Join-Path $out 'gpu_clocks.csv'
$clock = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process (Join-Path $bin 'ng2.exe') -WorkingDirectory $bin -PassThru -ArgumentList $argList
try {
  while ($true) {
    if ($p.HasExited) { $reason = "exited early, code $($p.ExitCode)"; break }
    if ($clock.Elapsed.TotalSeconds -ge $TimeoutSec) { $reason = "timeout after $TimeoutSec s"; break }
    if (Read-Log | Select-String -SimpleMatch 'NG2 AUTOPILOT' | Select-String -SimpleMatch ' GOAL ') {
      $goalSec = [math]::Round($clock.Elapsed.TotalSeconds, 1); break
    }
    Start-Sleep -Milliseconds 100
  }
  if ($goalSec -ge 0) {
    $measureEnd = $clock.Elapsed.TotalSeconds + $MeasureSec
    # GPU clocks and power (#131): capped frame times mean nothing without them
    if (Test-Path $smiFile) { Remove-Item $smiFile -Force }
    $smi = Start-Process nvidia-smi -PassThru -WindowStyle Hidden -ArgumentList '--query-gpu=clocks.gr,utilization.gpu,power.draw', '--format=csv,noheader,nounits', '-lms', '500', '-f', "`"$smiFile`""
    for ($i = 1; $i -le $Shots; ++$i) {
      Start-Sleep -Milliseconds ([int]($MeasureSec * 1000 / ($Shots + 1)))
      if (-not $p.HasExited) { Shot "shot$i" }
    }
    while ($clock.Elapsed.TotalSeconds -lt $measureEnd -and -not $p.HasExited) { Start-Sleep -Milliseconds 100 }
    if ($p.HasExited) { $reason = "exited after goal, code $($p.ExitCode)" }
    else { $status = 'PASS'; $exitCode = 0 }
  } else {
    Shot 'failed'
  }
} finally {
  if ($smi -and -not $smi.HasExited) { Stop-Process $smi -Force -ErrorAction SilentlyContinue }
  if (-not $p.HasExited) { Stop-Process $p -Force; $p.WaitForExit(5000) | Out-Null }
}

# Results
$all = Read-Log
$goalIdx = -1
for ($i = 0; $i -lt $all.Count; ++$i) { if ($all[$i] -like '*NG2 AUTOPILOT*GOAL*') { $goalIdx = $i; break } }
$after = if ($goalIdx -ge 0) { $all[$goalIdx..($all.Count - 1)] } else { @() }
$fpsVals = @($after | ForEach-Object { if ($_ -match 'NG2 AUTOPILOT fps ([\d.]+)') { [double]$Matches[1] } })
if ($fpsVals.Count) { $fps = [math]::Round(($fpsVals | Measure-Object -Average).Average, 1) }
$gpuVals = @($after | ForEach-Object {
    if ($_ -match 'NG2 GPU passes: frame ([\d.]+) ms') { [double]$Matches[1] }
    elseif ($_ -match 'NG2 VK GPU frame time: ([\d.]+) ms') { [double]$Matches[1] } })
# GPU time ramps up for ~12 s after the HUD appears (2 -> 5.3 ms at 1x, s1), so only the settled
# tail counts: median of the last 40%. Windows under 15 s are not comparable (see comparable).
$comparable = $MeasureSec -ge 15
if ($gpuVals.Count) {
  $tail = @($gpuVals | Select-Object -Last ([math]::Max(1, [int]($gpuVals.Count * 0.4))) | Sort-Object)
  $gpu = [math]::Round($tail[[int]($tail.Count / 2)], 2)
}

# Medians over the settled tail, like the frame time
$mhz = 'n/a'; $util = 'n/a'; $watts = 'n/a'
if (Test-Path $smiFile) {
  $rows = @(Get-Content $smiFile | ForEach-Object { $f = $_ -split ',\s*'; if ($f.Count -ge 3) { , $f } })
  $rows = @($rows | Select-Object -Last ([math]::Max(1, [int]($rows.Count * 0.4))))
  function Med($i) { $v = @($rows | ForEach-Object { [double]$_[$i] } | Sort-Object); if ($v.Count) { $v[[int]($v.Count / 2)] } else { 'n/a' } }
  if ($rows.Count) { $mhz = Med 0; $util = Med 1; $watts = Med 2 }
}

$csv = Join-Path $native 'results.csv'
if (-not (Test-Path $csv)) { 'time,name,route,backend,scale,status,goal_s,fps,gpu_ms,measure_s,cvars,mhz,util,watts' | Set-Content $csv -Encoding ascii }
elseif ((Get-Content $csv -TotalCount 1) -notmatch ',mhz,') {
  # Older rows just lack the three new columns
  $old = Get-Content $csv; $old[0] = "$($old[0]),mhz,util,watts"; $old | Set-Content $csv -Encoding ascii
}
$baseline = ''
if ($gpu -ne 'n/a' -and $Backend -ne 'd3d12' -and $comparable) {
  $ref = @(Import-Csv $csv | Where-Object { $_.backend -eq 'd3d12' -and $_.scale -eq "$Scale" -and $_.route -eq $Route -and $_.status -eq 'PASS' -and $_.gpu_ms -ne 'n/a' -and -not $_.cvars -and [int]$_.measure_s -ge 15 }) | Select-Object -Last 1
  if ($ref) { $baseline = " | D3D12 baseline $($ref.gpu_ms) ms, delta $([math]::Round($gpu - [double]$ref.gpu_ms, 2)) ms" }
}
"$(Get-Date -Format s),$Name,$Route,$Backend,$Scale,$status,$goalSec,$fps,$gpu,$MeasureSec,$($Cvars -join ' '),$mhz,$util,$watts" | Add-Content $csv -Encoding ascii

# The real folder must be untouched
$alert = $false
if ((Get-FileHash $settingsReal).Hash -ne $settingsHash) {
  $alert = $true
  Copy-Item (Join-Path $backup 'recomp_settings.toml') "$settingsReal.restored" -Force
}
if ((Fingerprint) -ne $before) { $alert = $true }

"$status $Name route=$Route backend=$Backend scale=${Scale}x goal=${goalSec}s fps=$fps gpu=${gpu}ms$(if (-not $comparable) { '(short window)' }) ${mhz}MHz ${util}% ${watts}W$baseline $reason"
if ($status -ne 'PASS') {
  if (Test-Path (Join-Path $bin 'ng2_crash.dmp')) { "crash dump: $(Join-Path $bin 'ng2_crash.dmp')" }
  '--- autopilot'; $all | Select-String -SimpleMatch 'NG2 AUTOPILOT' | Select-Object -Last 12 | ForEach-Object { $_.Line }
  '--- log tail'; $all | Select-Object -Last 25
}
"files: $out"
if ($alert) { "ALERT: $real changed during the test; compare with $backup"; exit 2 }
exit $exitCode
