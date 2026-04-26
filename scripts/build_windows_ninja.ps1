#!/usr/bin/env pwsh

$ErrorActionPreference = "Stop"

$rootDir = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $rootDir "build"
$logPath = Join-Path $buildDir "build_windows_ninja.log"
$exitCode = 0

if (Test-Path -LiteralPath $logPath) {
    Remove-Item -LiteralPath $logPath -Force
}

Start-Transcript -Path $logPath -Force | Out-Null

try {
    Set-Location $buildDir
    $vsPath = & (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe") -property installationPath
    Import-Module (Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation
    cmake .. -G Ninja
    ninja
}
catch {
    $exitCode = 1
    Write-Error ($_ | Out-String).TrimEnd()
}
finally {
    try {
        Stop-Transcript | Out-Null
    }
    catch {
    }

    exit $exitCode
}
