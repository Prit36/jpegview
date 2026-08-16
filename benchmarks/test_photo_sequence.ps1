<#
.SYNOPSIS
    Quick photo sequence viewer test — opens JPEGView and auto-advances through a sequence of photos.

.DESCRIPTION
    Launches JPEGView with the first photo in benchmarks/actual_test_data, then simulates
    right-arrow key presses to navigate through Count images at Delay-second intervals.
    Useful for visually verifying navigation performance without running the full benchmark suite.

.PARAMETER Count
    Number of photos to advance through (Default: 20)

.PARAMETER Delay
    Seconds to pause between each photo (Default: 1.5)

.PARAMETER DataDir
    Path to the photo folder (Default: benchmarks\actual_test_data)

.PARAMETER Exe
    Path to JPEGView.exe (Default: auto-detected from benchmarks\.cache\bin\current)

.EXAMPLE
    .\benchmarks\test_photo_sequence.ps1 -Count 20 -Delay 2.0
    .\benchmarks\test_photo_sequence.ps1 -Count 10 -Delay 1.0 -DataDir "C:\Photos"
#>

param(
    [int]$Count = 20,
    [double]$Delay = 1.5,
    [string]$DataDir = "",
    [string]$Exe = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

# -- Resolve data directory --
if (-not $DataDir) {
    $DataDir = Join-Path $PSScriptRoot "actual_test_data"
}
if (-not (Test-Path $DataDir)) {
    Write-Error "Photo folder not found: $DataDir"
    exit 1
}

$Photos = @(Get-ChildItem -Path $DataDir -Include "*.jpg","*.jpeg","*.png","*.bmp","*.tif","*.tiff" -File | Sort-Object Name)
if ($Photos.Count -eq 0) {
    Write-Error "No photos found in: $DataDir"
    exit 1
}

$ActualCount = [Math]::Min($Count, $Photos.Count)
Write-Host "==================================================================" -ForegroundColor Cyan
Write-Host "  JPEGView Photo Sequence Test" -ForegroundColor Cyan
Write-Host "  Folder : $DataDir ($($Photos.Count) photos)" -ForegroundColor Cyan
Write-Host "  Steps  : $ActualCount   Delay: ${Delay}s" -ForegroundColor Cyan
Write-Host "==================================================================" -ForegroundColor Cyan

# -- Resolve JPEGView exe --
if (-not $Exe) {
    $CacheBin = Join-Path $PSScriptRoot ".cache\bin\current\JPEGView.exe"
    $BuildBin = Join-Path $RepoRoot "build\bin\Release\JPEGView.exe"
    if (Test-Path $CacheBin) {
        $Exe = $CacheBin
    } elseif (Test-Path $BuildBin) {
        $Exe = $BuildBin
    } else {
        Write-Error "JPEGView.exe not found. Build first with: python benchmarks\benchmark_actual_data.py --targets current"
        exit 1
    }
}
Write-Host "  Exe    : $Exe" -ForegroundColor Gray
Write-Host ""

# -- Launch JPEGView with first photo --
$FirstPhoto = $Photos[0].FullName
Write-Host "[*] Launching JPEGView with: $($Photos[0].Name)" -ForegroundColor Yellow
$Proc = Start-Process -FilePath $Exe -ArgumentList "`"$FirstPhoto`"" -PassThru

# Wait for window to appear (up to 10s)
$WinAPI = Add-Type -PassThru -Name WinHelper -Namespace Native -MemberDefinition @"
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr extra);
    public delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr extra);
"@

function Find-WindowForPid($Pid) {
    $found = $null
    $cb = [Native.WinHelper+EnumWindowsProc]{
        param($hwnd, $extra)
        $pid2 = [uint32]0
        [Native.WinHelper]::GetWindowThreadProcessId($hwnd, [ref]$pid2) | Out-Null
        if ($pid2 -eq $Pid -and [Native.WinHelper]::IsWindowVisible($hwnd)) {
            $script:found = $hwnd
            return $false
        }
        return $true
    }
    [Native.WinHelper]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:found
}

Write-Host "[*] Waiting for JPEGView window..." -ForegroundColor Yellow
$Hwnd = $null
$deadline = (Get-Date).AddSeconds(10)
while ((Get-Date) -lt $deadline -and -not $Hwnd) {
    Start-Sleep -Milliseconds 100
    $Hwnd = Find-WindowForPid($Proc.Id)
}

if (-not $Hwnd) {
    Write-Warning "Window not found within 10s. JPEGView may have crashed."
    exit 1
}

Write-Host "[+] Window ready (HWND: $Hwnd)" -ForegroundColor Green
Write-Host ""

# Give first image time to fully render before starting
Start-Sleep -Milliseconds 500

# VK_RIGHT = 0x27, WM_KEYDOWN = 0x100
$WM_KEYDOWN = [uint32]0x100
$VK_RIGHT   = [IntPtr]0x27

$t0 = Get-Date
for ($i = 1; $i -le ($ActualCount - 1); $i++) {
    if ($Proc.HasExited) {
        Write-Warning "JPEGView exited early after $i steps."
        break
    }

    [Native.WinHelper]::PostMessage($Hwnd, $WM_KEYDOWN, $VK_RIGHT, [IntPtr]0) | Out-Null

    $elapsed = ((Get-Date) - $t0).TotalSeconds
    $photoName = if ($i -lt $Photos.Count) { $Photos[$i].Name } else { "..." }
    Write-Host ("  [{0,3}/{1}] {2,-30} ({3:F1}s elapsed)" -f $i, ($ActualCount-1), $photoName, $elapsed) -ForegroundColor White

    Start-Sleep -Seconds $Delay
}

$totalSec = ((Get-Date) - $t0).TotalSeconds
Write-Host ""
Write-Host "[+] Sequence complete: $($ActualCount-1) navigations in $($totalSec.ToString('F1'))s" -ForegroundColor Green
Write-Host "    JPEGView is still open — close it manually when done." -ForegroundColor Gray
