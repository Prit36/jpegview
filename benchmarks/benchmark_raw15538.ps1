<#
.SYNOPSIS
    Dedicated benchmark for opening RAW15538.JPG (17.13 MB photo).
.EXAMPLE
    .\benchmark_raw15538.ps1
    .\benchmark_raw15538.ps1 -Targets "original-fork,current" -Iterations 5
#>

[CmdletBinding()]
param(
    [string]$Targets = "original-fork,current",
    [string]$Baseline = "original-fork",
    [int]$Iterations = 5,
    [int]$Warmups = 2,
    [switch]$ForceRebuild
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Locate Python 3.12+ / 3.14
$PythonCmd = $null
if (Get-Command py -ErrorAction SilentlyContinue) {
    $PythonCmd = "py -3.14"
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    $PythonCmd = "python"
} else {
    Write-Error "Python 3.12+ is required but not found in PATH."
}

$ArgsList = @(
    "$ScriptDir\benchmark_raw15538.py",
    "--targets", $Targets,
    "--baseline", $Baseline,
    "--iterations", $Iterations,
    "--warmups", $Warmups
)

if ($ForceRebuild) {
    $ArgsList += "--force-rebuild"
}

Write-Host "[*] Executing dedicated RAW15538 benchmark..." -ForegroundColor Cyan
if ($PythonCmd -like "py *") {
    & py -3.14 @ArgsList
} else {
    & python @ArgsList
}
