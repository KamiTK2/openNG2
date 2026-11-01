# Launch an autotest run and ask ng2.exe to log every thread's stack (crash_handler.cpp
# watches for a "ng2_dumpstacks" file next to the exe), either at a fixed time or when
# a log heartbeat stops.
# Usage: powershell -File tools\stackdump.ps1 -Name run-39 [-Seconds 180]
#          [-AfterSeconds 110]                         fixed-time dump, or
#          [-Watch 'NG2DIAG AUDIO' -StallSeconds 8]    dump when that log line stops appearing
# Prints "STALL at Ns" when a watched heartbeat stopped.
param(
  [Parameter(Mandatory)] [string]$Name,
  [int]$Seconds = 180,
  [int]$AfterSeconds = 0,
  [string]$Watch = '',
  [int]$StallSeconds = 8,
  [string]$Extra = '--log_noisy=false',
  [string]$Preset = 'win-amd64-relwithdebinfo'
)
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root "project\ng2\out\build\$Preset"
$log = Join-Path $root "notes\logs\$Name.log"
$job = Start-Job -ScriptBlock {
  param($root, $name, $secs, $extra)
  & powershell -NoProfile -ExecutionPolicy Bypass -File "$root\tools\autotest.ps1" -Name $name `
    -Seconds $secs -Keys '{ENTER}' -KeyEvery 4 -KeyUntil 26 -ShotEvery 30 -Extra $extra
} -ArgumentList $root, $Name, $Seconds, $Extra

function Count-Matches {
  if (-not (Test-Path $log)) { return 0 }
  try {
    $fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    $sr = New-Object System.IO.StreamReader($fs); $txt = $sr.ReadToEnd(); $sr.Close()
    return ([regex]::Matches($txt, [regex]::Escape($Watch))).Count
  } catch { return 0 }
}

$start = Get-Date; $dumped = $false; $last = 0; $lastChange = Get-Date
while ($job.State -eq 'Running' -and -not $dumped) {
  Start-Sleep -Seconds 1
  $t = ((Get-Date) - $start).TotalSeconds
  if (-not (Get-Process ng2 -ErrorAction SilentlyContinue)) { continue }
  if ($Watch) {
    $c = Count-Matches
    if ($c -ne $last) { $last = $c; $lastChange = Get-Date }
    elseif ($c -gt 0 -and ((Get-Date) - $lastChange).TotalSeconds -ge $StallSeconds) {
      "STALL at $([int]$t)s (heartbeat '$Watch' count $c)"
      New-Item -ItemType File -Force (Join-Path $bin 'ng2_dumpstacks') | Out-Null; $dumped = $true
    }
  } elseif ($AfterSeconds -gt 0 -and $t -ge $AfterSeconds) {
    "stack dump requested at $([int]$t)s"
    New-Item -ItemType File -Force (Join-Path $bin 'ng2_dumpstacks') | Out-Null; $dumped = $true
  }
}
Receive-Job $job -Wait
if ($Watch -and -not $dumped) { "no stall (heartbeat count $last)" }
