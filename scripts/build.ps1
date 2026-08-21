#Requires -Version 5.1
<#
.SYNOPSIS
    Configures and builds mstar-flasher with MSVC, without requiring a
    "Developer PowerShell for VS" shell - locates the newest Visual Studio
    install via vswhere and loads its x64 toolchain environment itself.

.PARAMETER Preset
    CMake preset to use (from CMakePresets.json). Default: windows

.PARAMETER Clean
    Delete the preset's build directory before configuring.

.PARAMETER Test
    Run ctest for the preset after a successful build.

.EXAMPLE
    ./scripts/build.ps1
.EXAMPLE
    ./scripts/build.ps1 -Clean -Test
#>
[CmdletBinding()]
param(
    [string]$Preset = "windows",
    [switch]$Clean,
    [switch]$Test
)

$ErrorActionPreference = "Stop"

function Find-VsInstallation {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found at '$vswhere'. Is Visual Studio installed?"
    }

    $installPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $installPath) {
        throw "vswhere found no Visual Studio installation with the 'VC.Tools.x86.x64' component."
    }
    return $installPath
}

function Import-VcvarsEnvironment {
    param([string]$VsInstallPath)

    $vcvars = Join-Path $VsInstallPath "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) {
        throw "vcvars64.bat not found under '$VsInstallPath'."
    }

    Write-Host "Loading MSVC x64 environment from: $vcvars" -ForegroundColor Cyan
    $envDump = cmd /c "call `"$vcvars`" >nul && set"
    foreach ($line in $envDump) {
        if ($line -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "cl.exe still not on PATH after loading vcvars64.bat."
    }
}

$vsPath = Find-VsInstallation
Write-Host "Using Visual Studio at: $vsPath" -ForegroundColor Green
Import-VcvarsEnvironment -VsInstallPath $vsPath

if (-not $env:VCPKG_ROOT) {
    throw "VCPKG_ROOT is not set. Set it to your vcpkg checkout before running this script."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
    if ($Clean) {
        $binaryDir = Join-Path $repoRoot "build\$Preset"
        if (Test-Path $binaryDir) {
            Write-Host "Removing $binaryDir" -ForegroundColor Yellow
            Remove-Item -Recurse -Force $binaryDir
        }
    }

    Write-Host "Configuring preset '$Preset'..." -ForegroundColor Cyan
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (exit $LASTEXITCODE)." }

    Write-Host "Building preset '$Preset'..." -ForegroundColor Cyan
    cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed (exit $LASTEXITCODE)." }

    if ($Test) {
        Write-Host "Running tests for preset '$Preset'..." -ForegroundColor Cyan
        ctest --preset $Preset --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)." }
    }

    Write-Host "Build succeeded." -ForegroundColor Green
}
finally {
    Pop-Location
}
