<#
.SYNOPSIS
    PowerShell launcher for JPEGView Real-World Photo Benchmark Suite.

.DESCRIPTION
    Runs systematic performance comparison on the actual human-clicked camera photos in benchmarks/actual_test_data.

.PARAMETER Targets
    Comma-separated targets: original-fork,last-commit,current (Default: original-fork,last-commit,current)

.PARAMETER Baseline
    Baseline target: original-fork (Default: original-fork)

.PARAMETER NavSteps
    Number of photos to navigate through (Default: 100)

.PARAMETER Iterations
    Number of measurement runs per target (Default: 5)

.PARAMETER ForceRebuild
    Force recompilation of target binaries

.EXAMPLE
    .\benchmarks\benchmark_actual_data.ps1 -NavSteps 100
#>

param(
    [string]$Targets = "original-fork,last-commit,current",
    [string]$Baseline = "original-fork",
    [int]$NavSteps = 50,
    [int]$Iterations = 5,
    [switch]$ForceRebuild
)

$ErrorActionPreference = "Stop"

# Locate Python 3.14
$CandidatePaths = @(
    "C:\Users\My_Home\AppData\Local\Programs\Python\Python314\python.exe",
    "C:\Program Files\Python314\python.exe"
)

$PythonExe = $null
foreach ($path in $CandidatePaths) {
    if (Test-Path $path) {
        $PythonExe = $path
        break
    }
}

if (-not $PythonExe) {
    $PythonCmd = Get-Command python -ErrorAction SilentlyContinue
    if ($PythonCmd) {
        $PythonExe = $PythonCmd.Source
    } else {
        Write-Error "Python 3.14+ was not found on this machine."
        exit 1
    }
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RunnerScript = Join-Path $ScriptDir "benchmark_actual_data.py"

$ArgsList = @(
    $RunnerScript,
    "--targets", $Targets,
    "--baseline", $Baseline,
    "--nav-steps", $NavSteps,
    "--iterations", $Iterations
)

if ($ForceRebuild) {
    $ArgsList += "--force-rebuild"
}

Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host " Launching JPEGView Real-World Photo Benchmark Suite" -ForegroundColor Cyan
Write-Host " Dataset: benchmarks\actual_test_data (537 Photos)" -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan

& $PythonExe @ArgsList
