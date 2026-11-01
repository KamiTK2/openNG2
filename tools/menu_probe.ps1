# Drive the game with a scripted key sequence and take guest memory snapshots between
# steps (for tools\memdiff.py). Each step is "key" (SendKeys syntax), "wait:<sec>",
# "dump:<tag>", "shot:<tag>" or "touch:<file>" (creates a trigger file next to ng2.exe)
# or "hold:<vk hex>[+<vk hex>]:<ms>" (holds keys, for inputs the game must poll).
# Usage: powershell -File tools\menu_probe.ps1 -Name probe1 -Steps 'wait:9;{ENTER};wait:4;dump:a;...'
param(
  [Parameter(Mandatory)] [string]$Name,
  [Parameter(Mandatory)] [string]$Steps,   # semicolon-separated
  [string]$Preset = 'win-amd64-relwithdebinfo',
  [string]$Extra = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root "project\ng2\out\build\$Preset"
$logs = Join-Path $root 'notes\logs'
$out = Join-Path $logs "probe\$Name"
New-Item -ItemType Directory -Force $out | Out-Null
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class W2 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
'@
$p = Start-Process (Join-Path $bin 'ng2.exe') -WorkingDirectory $bin -PassThru -ArgumentList (@(
  "--game_data_root=`"$root\game`"", "--log_file=`"$logs\$Name.log`"", '--log_level=info', '--mnk_mode=true') + @($Extra -split ';' | Where-Object { $_ }))
function Focus { $p.Refresh(); if ($p.MainWindowHandle -ne [IntPtr]::Zero) { [void][W2]::SetForegroundWindow($p.MainWindowHandle); Start-Sleep -Milliseconds 150 } }
function Shot($tag) {
  $p.Refresh(); $h = $p.MainWindowHandle; $r = New-Object W2+RECT; [void][W2]::GetWindowRect($h, [ref]$r)
  $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T); $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); [void][W2]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc)
  $small = New-Object System.Drawing.Bitmap $bmp, ([int]($bmp.Width / 2)), ([int]($bmp.Height / 2))
  $small.Save((Join-Path $out "$tag.png")); $g.Dispose(); $bmp.Dispose(); $small.Dispose()
}
foreach ($s in ($Steps -split ';' | Where-Object { $_ -ne '' })) {
  if ($p.HasExited) { "game exited"; break }
  if ($s -like 'wait:*') { Start-Sleep -Milliseconds ([int]([double]$s.Substring(5) * 1000)) }
  elseif ($s -like 'shot:*') { Shot $s.Substring(5) }
  elseif ($s -like 'hold:*') {
    # hold:<vk>[+<vk>...]:<ms>  e.g. hold:0x10+0x28:150 = Shift+Down for 150 ms
    $parts = $s.Substring(5) -split ':'; $vks = $parts[0] -split '\+' | ForEach-Object { [byte][Convert]::ToInt32($_, 16) }
    Focus
    foreach ($vk in $vks) { [W2]::keybd_event($vk, 0, 0, [UIntPtr]::Zero) }
    Start-Sleep -Milliseconds ([int]$parts[1])
    [array]::Reverse($vks); foreach ($vk in $vks) { [W2]::keybd_event($vk, 0, 2, [UIntPtr]::Zero) }
  }
  elseif ($s -like 'touch:*') { New-Item -ItemType File -Force (Join-Path $bin $s.Substring(6)) | Out-Null }
  elseif ($s -like 'dump:*') {
    $tag = $s.Substring(5); $dst = Join-Path $bin "memdump_$tag.bin"; Remove-Item $dst -ErrorAction SilentlyContinue
    New-Item -ItemType File (Join-Path $bin "ng2_dumpmem_$tag") | Out-Null
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt 60) {
      Start-Sleep -Milliseconds 300
      if ((Test-Path $dst) -and (Select-String -Path "$logs\$Name.log" -Pattern "memdump_$tag.bin" -Quiet)) { break }
    }
    Move-Item $dst (Join-Path $out "$tag.bin") -Force; "dumped $tag"
  }
  else { Focus; [System.Windows.Forms.SendKeys]::SendWait($s) }
}
if (-not $p.HasExited) { Stop-Process $p -Force }
"probe output: $out"



