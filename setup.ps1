# NinjaGaiden2-Recomp - one-time toolchain setup
# Run via Setup.bat (or: powershell -ExecutionPolicy Bypass -File setup.ps1)
# Re-running is safe: finished steps are skipped.
#
# Flags:
#   -SkipVS         don't install Visual Studio Build Tools
#   -SkipGhidra     don't download Ghidra / XEXLoaderWV
#   -SkipReference  don't clone the existing Ninja's Dawn NG2 recomp for reference
#   -SkipSdkBuild   clone ReXGlue but don't build/install it

param(
    [switch]$SkipVS,
    [switch]$SkipGhidra,
    [switch]$SkipReference,
    [switch]$SkipSdkBuild
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'   # makes Invoke-WebRequest much faster
$Root  = $PSScriptRoot
$Tools = Join-Path $Root 'tools'
$Ref   = Join-Path $Root 'reference'
$Log   = Join-Path $Root 'setup.log'
New-Item -ItemType Directory -Force -Path $Tools, $Ref | Out-Null
Start-Transcript -Path $Log -Append | Out-Null

function Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }
function Ok($msg)   { Write-Host "  [ok] $msg"   -ForegroundColor Green }
function Warn($msg) { Write-Host "  [!]  $msg"   -ForegroundColor Yellow }

function Refresh-Path {
    $env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' +
                [Environment]::GetEnvironmentVariable('Path','User')
}

function Winget-Install($id, $extra = @()) {
    $listed = winget list --id $id -e --accept-source-agreements 2>$null | Out-String
    if ($listed -match [regex]::Escape($id)) { Ok "$id already installed"; return }
    Write-Host "  installing $id ..."
    $wargs = @('install','--id',$id,'-e','--accept-package-agreements','--accept-source-agreements','--silent') + $extra
    & winget @wargs
    if ($LASTEXITCODE -ne 0) { Warn "${id}: winget exit code $LASTEXITCODE (check setup.log)" } else { Ok "$id installed" }
}

function Gh-Releases($repo) {
    Invoke-RestMethod -Headers @{ 'User-Agent' = 'ng2-recomp-setup' } `
        -Uri "https://api.github.com/repos/$repo/releases?per_page=30"
}

# ---------------------------------------------------------------- 0. sanity
Step 'Checking environment'
if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw 'winget not found. Install "App Installer" from the Microsoft Store, then re-run.'
}
if (-not [Environment]::Is64BitOperatingSystem) { throw 'A 64-bit Windows install is required.' }
Ok "Windows x64, winget present. Project root: $Root"

# ---------------------------------------------------------------- 1. base tools
Step 'Installing base tools (Git, Node.js 24 LTS, CMake, Ninja, Python, JDK 21)'
Winget-Install 'Git.Git'
Winget-Install 'OpenJS.NodeJS.LTS'                 # REA needs Node ^24.22.1 / ^22.23.4 / >=26.12.1
Winget-Install 'Kitware.CMake'                     # ReXGlue needs 3.25+
Winget-Install 'Ninja-build.Ninja'
Winget-Install 'Python.Python.3.12'
Winget-Install 'EclipseAdoptium.Temurin.21.JDK'    # Ghidra 12.x needs a full 64-bit JDK 21+
Refresh-Path

# ---------------------------------------------------------------- 2. Visual Studio (clang-cl)
if (-not $SkipVS) {
    Step 'Installing Visual Studio 2022 Build Tools with C++ and Clang (large download, ~10 GB)'
    Winget-Install 'Microsoft.VisualStudio.2022.BuildTools' @(
        '--override',
        '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset'
    )
}

# ---------------------------------------------------------------- 3. REA
Step 'Installing REA (Reverse Engineer Anything) from npm: rea-agents'
Refresh-Path
if (-not (Get-Command npm -ErrorAction SilentlyContinue)) {
    Warn 'npm not on PATH yet - close this window and run Setup.bat again.'
} else {
    $nodeVer = [version]((node --version) -replace '^v','')
    if ($nodeVer -lt [version]'24.22.1') {
        Write-Host "  node $nodeVer is older than REA needs (24.22.1+); upgrading ..."
        winget upgrade --id OpenJS.NodeJS.LTS -e --accept-package-agreements --accept-source-agreements --silent
        Refresh-Path
        $nodeVer = [version]((node --version) -replace '^v','')
    }
    if ($nodeVer -lt [version]'24.22.1') { Warn "node is still $nodeVer - install Node 24.22.1+ from nodejs.org, then re-run" } else { Ok "node $nodeVer" }
    npm install --global rea-agents@latest
    if ($LASTEXITCODE -eq 0) { Ok ("rea " + (rea --version 2>$null)) } else { Warn 'npm install rea-agents failed (see setup.log)' }
}

# ---------------------------------------------------------------- 4. Ghidra + XEXLoaderWV
$GhidraDir = $null
if (-not $SkipGhidra) {
    # 12.1.3 = the version the published XEXLoaderWV build targets (and inside REA's 12.1+ range)
    Step 'Downloading Ghidra 12.1.3 (matches XEXLoaderWV; also used by REA)'
    $existing = Get-ChildItem $Tools -Directory -Filter 'ghidra_12.1*' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($existing) {
        $GhidraDir = $existing.FullName; Ok "already present: $GhidraDir"
    } else {
        $rel = Invoke-RestMethod -Headers @{ 'User-Agent' = 'ng2-recomp-setup' } `
               -Uri 'https://api.github.com/repos/NationalSecurityAgency/ghidra/releases/tags/Ghidra_12.1.3_build'
        if (-not $rel) { Warn 'Ghidra 12.1.3 release not found; download manually from github.com/NationalSecurityAgency/ghidra/releases' }
        else {
            $asset = $rel.assets | Where-Object { $_.name -like '*.zip' } | Select-Object -First 1
            $zip = Join-Path $Tools $asset.name
            Write-Host "  downloading $($asset.name) ..."
            Invoke-WebRequest $asset.browser_download_url -OutFile $zip
            Expand-Archive $zip -DestinationPath $Tools -Force
            Remove-Item $zip
            $GhidraDir = (Get-ChildItem $Tools -Directory -Filter 'ghidra_12.1*' | Select-Object -First 1).FullName
            Ok "Ghidra at $GhidraDir"
        }
    }

    Step 'Fetching XEXLoaderWV (Ghidra loader for Xbox 360 .xex files)'
    $extDir = Join-Path $Tools 'ghidra-extensions'
    New-Item -ItemType Directory -Force -Path $extDir | Out-Null
    try {
        $gv = if ($GhidraDir) { (Split-Path $GhidraDir -Leaf) -replace '^ghidra_','' -replace '_PUBLIC.*$','' } else { '12.1' }
        $xrel = Gh-Releases 'zeroKilo/XEXLoaderWV'
        $xasset = $xrel.assets | Where-Object { $_.name -like "*$gv*" -and $_.name -like '*.zip' } | Select-Object -First 1
        if (-not $xasset) { $xasset = $xrel.assets | Where-Object { $_.name -like '*12.1*' -and $_.name -like '*.zip' } | Select-Object -First 1 }
        if ($xasset) {
            Invoke-WebRequest $xasset.browser_download_url -OutFile (Join-Path $extDir $xasset.name)
            Ok "downloaded $($xasset.name) -> tools\ghidra-extensions (install via Ghidra: File > Install Extensions)"
        } else {
            Warn "No XEXLoaderWV build matching Ghidra $gv was published. Build it from source (github.com/zeroKilo/XEXLoaderWV) or use IDA + idaxex."
        }
    } catch { Warn "XEXLoaderWV lookup failed: $_" }
}

# ---------------------------------------------------------------- 5. ReXGlue SDK
Step 'Cloning ReXGlue SDK (Xbox 360 static recompiler)'
Refresh-Path
$Sdk = Join-Path $Tools 'rexglue-sdk'
if (Test-Path (Join-Path $Sdk '.git')) {
    git -C $Sdk pull --ff-only; git -C $Sdk submodule update --init --recursive
    Ok 'updated existing clone'
} else {
    git clone --recursive https://github.com/rexglue/rexglue-sdk.git $Sdk
    Ok "cloned to $Sdk"
}
git -C $Sdk submodule update --init --recursive

# Git on Windows (core.symlinks=false) checks symlinks out as tiny text files holding
# the link path, e.g. thirdparty/libmspack/cabextract/mspack/lzxd.c -> "../../libmspack/mspack/lzxd.c".
# Replace every such stub in the SDK and its submodules with a real copy of its target.
Step 'Materialising git symlinks in ReXGlue (Windows checkout fix)'
$repos = @($Sdk) + @(git -C $Sdk submodule foreach --recursive --quiet 'cygpath -w "$PWD"')
$fixed = 0
foreach ($r in $repos) {
    foreach ($line in (git -C $r ls-files -s)) {
        if ($line -notmatch '^120000 \S+ \d+\t(.+)$') { continue }
        $link = Join-Path $r ($Matches[1] -replace '/','\')
        if (-not (Test-Path $link -PathType Leaf)) { continue }        # already a real dir/symlink
        $item = Get-Item $link -Force
        if ($item.LinkType) { continue }                                 # real symlink, fine
        $targetRel = (Get-Content $link -Raw).Trim()
        $target = [IO.Path]::GetFullPath((Join-Path (Split-Path $link) ($targetRel -replace '/','\')))
        if (-not (Test-Path $target)) { Warn "symlink target missing: $targetRel (from $link)"; continue }
        Remove-Item $link -Force
        Copy-Item $target $link -Recurse -Force
        $fixed++
    }
}
Ok "replaced $fixed symlink stub(s) with real files"

if (-not $SkipSdkBuild) {
    Step 'Building and installing ReXGlue (cmake --preset win-amd64), this takes a while'
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath } else { $null }
    if (-not $vsPath) {
        Warn 'Visual Studio with the Clang component not found yet. If VS just installed, reboot and run Setup.bat again.'
    } else {
        $devCmd = Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'
        $cmd = "`"$devCmd`" -arch=amd64 -host_arch=amd64 && cd /d `"$Sdk`" && cmake --preset win-amd64 && cmake --build out/build/win-amd64 --target install"
        $buildLog = Join-Path $Tools 'rexglue-build.log'
        # redirect inside cmd: PowerShell 5.1 treats native stderr as a terminating error under 'Stop'
        cmd /c "($cmd) 2>&1" | Tee-Object -FilePath $buildLog
        if ($LASTEXITCODE -eq 0) { Ok 'ReXGlue built and installed (registered for find_package(rexglue))' }
        else { Warn "ReXGlue build failed - full compiler output is in tools\rexglue-build.log" }
    }
}

# ---------------------------------------------------------------- 6. reference port
if (-not $SkipReference) {
    Step "Cloning reference: Ninja's Dawn (existing ReXGlue NG2 recomp by TheSaltTrader)"
    $nd = Join-Path $Ref 'ninjas-dawn'
    if (Test-Path (Join-Path $nd '.git')) { git -C $nd pull --ff-only; Ok 'updated' }
    else {
        git clone --depth 1 https://github.com/TheSaltTrader/Ninja-s-Dawn---A-Ninja-Gaiden-2-Recompilation-Project.git $nd
        if ($LASTEXITCODE -eq 0) { Ok "cloned to $nd" } else { Warn "Ninja's Dawn repo isn't publicly reachable (private, renamed or removed) - skipping. Use -SkipReference to silence this." }
    }
}

# ---------------------------------------------------------------- 7. env.ps1
Step 'Writing env.ps1 (dot-source it in any new terminal: . .\env.ps1)'
$jdk = Get-ChildItem 'C:\Program Files\Eclipse Adoptium' -Directory -Filter 'jdk-21*' -ErrorAction SilentlyContinue | Select-Object -First 1
$rexBin = Get-ChildItem $Sdk -Recurse -Filter 'rexglue.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
$lines = @(
    '# Generated by setup.ps1 - dot-source me:  . .\env.ps1',
    "`$env:NG2_ROOT = '$Root'"
)
if ($GhidraDir) { $lines += "`$env:GHIDRA_INSTALL_DIR = '$GhidraDir'" }
if ($jdk)       { $lines += "`$env:JAVA_HOME = '$($jdk.FullName)'" }
$lines += "`$env:REA_ANALYSIS_PROVIDER = 'ghidra'"
if ($rexBin)    { $lines += "`$env:Path = '$($rexBin.DirectoryName);' + `$env:Path" }
$lines += "Write-Host 'NG2 recomp environment loaded.' -ForegroundColor Green"
Set-Content -Path (Join-Path $Root 'env.ps1') -Value $lines -Encoding UTF8
Ok 'env.ps1 written'

# ---------------------------------------------------------------- 8. summary
Step 'Summary'
. (Join-Path $Root 'env.ps1')
foreach ($t in 'git','node','npm','cmake','ninja','java','rea','rexglue') {
    $c = Get-Command $t -ErrorAction SilentlyContinue
    if ($c) { Ok "$t -> $($c.Source)" } else { Warn "$t not found on PATH" }
}
Write-Host @"

Next steps
  1. Open a NEW terminal in this folder and run:   . .\env.ps1
  2. Register REA with your agent (interactive):   rea setup
     then check it:                                rea doctor
  3. Put your own NG2 dump in .\game\  (see game\README.txt)
  4. Follow README.md, "Phase 1".
Log saved to setup.log
"@ -ForegroundColor White
Stop-Transcript | Out-Null
