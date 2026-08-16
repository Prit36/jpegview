<#
.SYNOPSIS
    PowerShell launcher for JPEGView Systematic Performance Benchmark Suite.

.DESCRIPTION
    Builds, benchmarks, and compares JPEGView across Original Fork, Last Commit, and Current changes.

.PARAMETER Profile
    Benchmark intensity profile: quick, standard, stress, ci (Default: quick)

.PARAMETER Targets
    Comma-separated targets: original-fork,last-commit,current (Default: original-fork,last-commit,current)

.EXAMPLE
    .\benchmarks\run_benchmark.ps1 -Profile quick
#>

param(
    [string]$Profile = "quick",
    [string]$Targets = "original-fork,last-commit,current",
    [switch]$ForceRebuild
)

$ErrorActionPreference = "Stop"

# Locate Python
$PythonExe = "C:\Users\My_Home\AppData\Local\Programs\Python\Python312\python.exe"
if (-not (Test-Path $PythonExe)) {
    $PythonCmd = Get-Command python -ErrorAction SilentlyContinue
    if ($PythonCmd) {
        $PythonExe = $PythonCmd.Source
    } else {
        Write-Error "Python 3.12+ was not found on this machine."
        exit 1
    }
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RunnerScript = Join-Path $ScriptDir "run_benchmark.py"

$ArgsList = @(
    $RunnerScript,
    "compare",
    "--targets", $Targets,
    "--profile", $Profile
)

if ($ForceRebuild) {
    $ArgsList += "--force-rebuild"
}

Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host " Launching JPEGView Benchmark Suite (Profile: $Profile)" -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan

& $PythonExe @ArgsList
