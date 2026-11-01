# Automated test run of the ng2 recomp: launch, press menu keys, take screenshots, close.
# Usage: powershell -File tools\autotest.ps1 -Name run-13 [-Seconds 60] [-KeyEvery 3] [-ShotEvery 10]
#        [-Keys "{ENTER},{ENTER}, ,..."]   (SendKeys syntax; cycled; " " = Space = A button)
param(
  [Parameter(Mandatory)] [string]$Name,
  [int]$Seconds = 60,
  [int]$KeyEvery = 3,
  [int]$KeyUntil = 100000,
  [int]$ShotEvery = 10,
  [string[]]$Keys = @('{ENTER}', ' '),
  [string]$Preset = 'win-amd64-relwithdebinfo',
  [string]$LogLevel = 'info',
  [string]$Extra = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root "project\ng2\out\build\$Preset"
$logs = Join-Path $root 'notes\logs'
$shots = Join-Path $logs "shots\$Name"
New-Item -ItemType Directory -Force $shots | Out-Null
$log = Join-Path $logs "$Name.log"
Remove-Item $log, (Join-Path $bin 'ng2_crash.dmp') -ErrorAction SilentlyContinue

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
'@

$argList = @("--game_data_root=`"$root\game`"", "--log_file=`"$log`"", "--log_level=$LogLevel",
  '--mnk_mode=true', "--perf_log_csv=`"$logs\$Name.frames.csv`"") + @($Extra -split ';' | Where-Object { $_ })
Remove-Item "$logs\$Name.frames.csv" -ErrorAction SilentlyContinue
$p = Start-Process (Join-Path $bin 'ng2.exe') -WorkingDirectory $bin -PassThru -ArgumentList $argList

function Shot($tag) {
  $p.Refresh(); $h = $p.MainWindowHandle
  if ($h -eq [IntPtr]::Zero) { return }
  $r = New-Object W+RECT; [void][W]::GetWindowRect($h, [ref]$r)
  $w = $r.R - $r.L; $hh = $r.B - $r.T
  if ($w -le 0 -or $hh -le 0) { return }
  $bmp = New-Object System.Drawing.Bitmap $w, $hh
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); [void][W]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc)
  $small = New-Object System.Drawing.Bitmap $bmp, ([int]($w / 2)), ([int]($hh / 2))
  $small.Save((Join-Path $shots "$tag.png"), [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose(); $small.Dispose()
}

$start = Get-Date; $k = 0; $nextKey = 8; $nextShot = $ShotEvery
while (-not $p.HasExited) {
  $t = ((Get-Date) - $start).TotalSeconds
  if ($t -ge $Seconds) { break }
  if ($t -ge $nextKey -and $t -lt $KeyUntil) {
    $p.Refresh()
    if ($p.MainWindowHandle -ne [IntPtr]::Zero) {
      [void][W]::ShowWindow($p.MainWindowHandle, 9); [void][W]::SetForegroundWindow($p.MainWindowHandle)
      Start-Sleep -Milliseconds 150
      [System.Windows.Forms.SendKeys]::SendWait($Keys[$k % $Keys.Count]); $k++
    }
    $nextKey += $KeyEvery
  }
  if ($t -ge $nextShot) { Shot ('t{0:D3}' -f [int]$t); $nextShot += $ShotEvery }
  Start-Sleep -Milliseconds 200
}
if ($p.HasExited) { "EXITED code=$($p.ExitCode) after $([int]((Get-Date)-$start).TotalSeconds)s" }
else { Shot 'final'; Stop-Process $p -Force; "ran $Seconds s" }
"shots: $shots"




