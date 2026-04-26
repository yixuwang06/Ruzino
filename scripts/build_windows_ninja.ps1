#!/usr/bin/env pwsh

$ErrorActionPreference = "Stop"

$rootDir = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $rootDir "build"
$logPath = Join-Path $buildDir "build_windows_ninja.log"
$cmakeLogPath = Join-Path $buildDir "build_windows_ninja.cmake.log"
$ninjaLogPath = Join-Path $buildDir "build_windows_ninja.ninja.log"

foreach ($path in @($logPath, $cmakeLogPath, $ninjaLogPath)) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Force
    }
}

function Merge-NativeLogs {
    foreach ($path in @($cmakeLogPath, $ninjaLogPath)) {
        if (Test-Path -LiteralPath $path) {
            Add-Content -LiteralPath $logPath -Value ""
            Add-Content -LiteralPath $logPath -Value ("===== " + (Split-Path $path -Leaf) + " =====")
            Get-Content -LiteralPath $path | Add-Content -LiteralPath $logPath
            Remove-Item -LiteralPath $path -Force
        }
    }
}

function Invoke-LoggedNativeCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Command,

        [Parameter(Mandatory = $true)]
        [string]$NativeLogPath,

        [string[]]$Arguments = @()
    )

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $Command @Arguments 2>&1 | Tee-Object -FilePath $NativeLogPath | Out-Host
        return $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
}

function Normalize-PathEnvironment {
    [System.Environment]::SetEnvironmentVariable("PATH", $null, "Process")

    $machinePath = [System.Environment]::GetEnvironmentVariable("Path", "Machine")
    $userPath = [System.Environment]::GetEnvironmentVariable("Path", "User")

    if ([string]::IsNullOrWhiteSpace($machinePath)) {
        $env:Path = $userPath
    }
    elseif ([string]::IsNullOrWhiteSpace($userPath)) {
        $env:Path = $machinePath
    }
    else {
        $env:Path = $machinePath + ";" + $userPath
    }
}

trap {
    $message = ($_ | Out-String).TrimEnd()
    if (-not [string]::IsNullOrWhiteSpace($message)) {
        Write-Host $message
    }

    try {
        Stop-Transcript | Out-Null
    }
    catch {
    }

    Merge-NativeLogs

    exit 1
}

Start-Transcript -Path $logPath -Force | Out-Null

Set-Location $buildDir
Normalize-PathEnvironment
$vsPath = & (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe") -property installationPath
Import-Module (Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64"
$cmakeExitCode = Invoke-LoggedNativeCommand -Command "cmake" -Arguments @("..", "-G", "Ninja") -NativeLogPath $cmakeLogPath
if ($cmakeExitCode -ne 0) {
    throw "cmake .. -G Ninja failed with exit code $cmakeExitCode."
}

$ninjaExitCode = Invoke-LoggedNativeCommand -Command "ninja" -NativeLogPath $ninjaLogPath
if ($ninjaExitCode -ne 0) {
    throw "ninja failed with exit code $ninjaExitCode."
}

Stop-Transcript | Out-Null
Merge-NativeLogs
exit 0
