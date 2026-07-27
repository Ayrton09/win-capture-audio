<#
.SYNOPSIS
    Assembles a minimal libobs SDK (headers + import library) for building this plugin.

.DESCRIPTION
    OBS Studio's Windows installer ships no development files: no headers, no import
    library, no CMake package. Building a plugin therefore normally means building all
    of OBS from source.

    This script takes the cheaper and more accurate route:

      * headers  -> downloaded from the obs-studio git tag that matches the OBS build
                    already installed on this machine, so the struct layouts we compile
                    against are exactly the ones the installed obs.dll uses;
      * obs.lib  -> generated directly from the installed obs.dll's export table with
                    dumpbin + lib, so the import library can never drift from the DLL
                    it is meant to link against.

    The result is written to <repo>/deps/libobs-sdk and consumed by CMake.

.PARAMETER ObsDir
    Root of the OBS Studio installation (the folder containing bin\, data\, obs-plugins\).
    Auto-detected from the registry when omitted.

.PARAMETER ObsVersion
    obs-studio git tag to take headers from. Defaults to the version reported by the
    installed obs.dll.

.PARAMETER OutDir
    Where to write the SDK. Defaults to <repo>/deps/libobs-sdk.

.PARAMETER Force
    Rebuild even if the SDK already looks up to date.
#>
[CmdletBinding()]
param(
    [string]$ObsDir,
    [string]$ObsVersion,
    [string]$OutDir,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $repoRoot 'deps\libobs-sdk' }

function Write-Step($msg) { Write-Host "[obs-sdk] $msg" }

# --- Locate the OBS installation -------------------------------------------------

function Find-ObsDir {
    $candidates = @()
    foreach ($key in 'HKLM:\SOFTWARE\OBS Studio', 'HKLM:\SOFTWARE\WOW6432Node\OBS Studio') {
        try {
            $v = (Get-ItemProperty -Path $key -ErrorAction Stop).'(default)'
            if ($v) { $candidates += $v }
        } catch { }
    }
    $candidates += "$env:ProgramFiles\obs-studio"
    $candidates += "${env:ProgramFiles(x86)}\obs-studio"

    foreach ($c in $candidates) {
        if ($c -and (Test-Path (Join-Path $c 'bin\64bit\obs.dll'))) { return $c }
    }
    return $null
}

if (-not $ObsDir) { $ObsDir = Find-ObsDir }
if (-not $ObsDir) {
    throw "Could not find an OBS Studio installation. Pass -ObsDir <path to obs-studio>."
}

$obsDll = Join-Path $ObsDir 'bin\64bit\obs.dll'
if (-not (Test-Path $obsDll)) { throw "obs.dll not found under '$ObsDir'." }
Write-Step "OBS installation: $ObsDir"

if (-not $ObsVersion) {
    # obs.dll carries the canonical libobs version in its version resource.
    $raw = (Get-Item $obsDll).VersionInfo.ProductVersion
    if (-not $raw) { $raw = (Get-Item $obsDll).VersionInfo.FileVersion }
    if ($raw -match '(\d+)\.(\d+)\.(\d+)') { $ObsVersion = "$($matches[1]).$($matches[2]).$($matches[3])" }
}
if (-not $ObsVersion) { throw "Could not determine the OBS version. Pass -ObsVersion <e.g. 32.2.1>." }
Write-Step "Target libobs version: $ObsVersion"

# --- Skip if already current -----------------------------------------------------

$stampFile = Join-Path $OutDir 'sdk-stamp.txt'
$stamp = "$ObsVersion|$((Get-Item $obsDll).Length)|$((Get-Item $obsDll).LastWriteTimeUtc.Ticks)"
if (-not $Force -and (Test-Path $stampFile) -and ((Get-Content $stampFile -Raw).Trim() -eq $stamp)) {
    Write-Step "SDK already up to date ($OutDir) - nothing to do."
    return
}

# --- Locate the MSVC tools we need -----------------------------------------------

function Find-MsvcTool($name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $roots = @()
    if (Test-Path $vswhere) {
        $roots += & $vswhere -latest -products * -property installationPath
    }
    $roots += Get-ChildItem 'C:\Program Files\Microsoft Visual Studio' -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue } |
        ForEach-Object { $_.FullName }

    foreach ($root in ($roots | Where-Object { $_ } | Select-Object -Unique)) {
        $tool = Get-ChildItem (Join-Path $root 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName "bin\Hostx64\x64\$name" } |
            Where-Object { Test-Path $_ } |
            Select-Object -First 1
        if ($tool) { return $tool }
    }
    return $null
}

$dumpbin = Find-MsvcTool 'dumpbin.exe'
$libExe  = Find-MsvcTool 'lib.exe'
if (-not $dumpbin -or -not $libExe) {
    throw "dumpbin.exe / lib.exe not found. Run this from a Visual Studio developer prompt, or install the MSVC C++ build tools."
}

# --- Fetch the libobs headers ----------------------------------------------------

$includeDir = Join-Path $OutDir 'include'
$libDir     = Join-Path $OutDir 'lib'
$tmpDir     = Join-Path $OutDir '.tmp'

foreach ($d in $includeDir, $libDir) {
    if (Test-Path $d) { Remove-Item $d -Recurse -Force }
}
if (Test-Path $tmpDir) { Remove-Item $tmpDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $includeDir, $libDir, $tmpDir | Out-Null

$tarball = Join-Path $tmpDir "obs-studio-$ObsVersion.tar.gz"
$url = "https://github.com/obsproject/obs-studio/archive/refs/tags/$ObsVersion.tar.gz"
Write-Step "Downloading libobs headers from $url"
try {
    Invoke-WebRequest -Uri $url -OutFile $tarball -UseBasicParsing
} catch {
    throw "Failed to download obs-studio $ObsVersion. Check that the tag exists, or pass -ObsVersion explicitly. ($($_.Exception.Message))"
}

# Extract libobs/ only. The full tree contains symlinks (.github/scripts/*, build-aux/*)
# that Windows tar cannot create without developer mode / elevation, and we need none of them.
Write-Step "Extracting libobs/"
tar -xzf $tarball -C $tmpDir "obs-studio-$ObsVersion/libobs"
if ($LASTEXITCODE -ne 0) { throw "tar failed to extract $tarball" }

$srcRoot = Get-ChildItem $tmpDir -Directory | Where-Object { $_.Name -like 'obs-studio-*' } | Select-Object -First 1
if (-not $srcRoot) { throw "Unexpected archive layout in $tmpDir" }
$libobsSrc = Join-Path $srcRoot.FullName 'libobs'
if (-not (Test-Path (Join-Path $libobsSrc 'obs.h'))) { throw "libobs/obs.h missing from the downloaded archive" }

Copy-Item (Join-Path $libobsSrc '*.h') $includeDir
foreach ($sub in 'util', 'media-io', 'graphics', 'callback') {
    $target = Join-Path $includeDir $sub
    New-Item -ItemType Directory -Force $target | Out-Null
    Copy-Item (Join-Path $libobsSrc "$sub\*.h") $target
}

# obsconfig.h is generated by OBS's own CMake run and is therefore absent from the
# source tree. obs.h -> obs-config.h -> obsconfig.h, so plugins cannot compile without
# it. Only the path macros matter to a plugin; they mirror an official Windows build.
@'
#pragma once

/* Generated by cmake/bootstrap-obs-sdk.ps1 as a stand-in for the obsconfig.h that
 * OBS Studio's own CMake run produces. Values mirror an official Windows build. */

#define OBS_DATA_PATH "../../data"
#define OBS_PLUGIN_PATH "../../obs-plugins/64bit"
#define OBS_PLUGIN_DESTINATION "obs-plugins/64bit"

#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
'@ | Set-Content -Path (Join-Path $includeDir 'obsconfig.h') -Encoding ascii

$headerCount = (Get-ChildItem $includeDir -Recurse -Filter *.h).Count
Write-Step "Headers: $headerCount files -> $includeDir"

# --- Generate the import library from the installed obs.dll ----------------------

Write-Step "Reading exports from $obsDll"
$exports = & $dumpbin /EXPORTS $obsDll
$names = $exports | ForEach-Object {
    if ($_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)') { $matches[1] }
}
$names = $names | Where-Object { $_ } | Sort-Object -Unique
if ($names.Count -lt 100) { throw "Only $($names.Count) exports found in obs.dll - that cannot be right." }

$defPath = Join-Path $libDir 'obs.def'
Set-Content -Path $defPath -Value (@('EXPORTS') + $names) -Encoding ascii

$libPath = Join-Path $libDir 'obs.lib'
& $libExe /nologo /def:$defPath /machine:x64 /out:$libPath | Out-Null
if (-not (Test-Path $libPath)) { throw "lib.exe failed to produce obs.lib" }
Write-Step "Import library: $($names.Count) exports -> $libPath"

# --- Finish ----------------------------------------------------------------------

Remove-Item $tmpDir -Recurse -Force
Set-Content -Path $stampFile -Value $stamp -Encoding ascii
Write-Step "Done. SDK ready at $OutDir"
