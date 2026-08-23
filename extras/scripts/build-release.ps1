# Builds the release artifacts for the Prit36/jpegview fork (x64-only):
#   * portable ZIP   - JPEGView_<version>.zip        (contains JPEGView64/)
#   * installer MSI  - JPEGView64_en-us_<version>.msi (built with modern WiX v4+ tooling)
#   * checksums      - SHA256SUMS.txt
#
# Prerequisites:
#   * Release x64 binaries built at src\JPEGView\bin\x64\Release (msbuild or cmake)
#   * codec DLLs present under src\JPEGView\lib*\bin64 (see deps README)
#   * WiX v6 dotnet tool (auto-installed user-locally if missing; no admin required)

#Requires -Version 5.1
[CmdletBinding()]
param(
	[string]$Version = "",
	[string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # extras/scripts -> repo root
$BinDir = Join-Path $RepoRoot "src\JPEGView\bin\x64\Release"
$SetupDir = Join-Path $RepoRoot "src\JPEGView.Setup"

if (-not $Version) {
	# detect version from src/JPEGView/resource.h  ("2, 1, 0, 0" -> "2.1.0")
	$line = Select-String -Path (Join-Path $RepoRoot "src\JPEGView\resource.h") -Pattern '#define JPEGVIEW_VERSION "([0-9, ]+)' |
		Select-Object -First 1
	if (-not $line) { throw "Could not detect version from resource.h; pass -Version <x.y.z>" }
	$parts = ($line.Matches[0].Groups[1].Value -split ",") | ForEach-Object { $_.Trim() } | Where-Object { $_ }
	$Version = ($parts | Select-Object -First 3) -join "."
}
Write-Host "==> Release version: $Version" -ForegroundColor Cyan

if (-not (Test-Path (Join-Path $BinDir "JPEGView.exe"))) {
	throw "Release binaries not found at '$BinDir'. Build first: msbuild /p:Platform=x64 /p:Configuration=Release src\JPEGView.sln"
}

if (-not $OutDir) { $OutDir = Join-Path $RepoRoot "release_out\v$Version" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ---------------------------------------------------------------------------
# Locate (or bootstrap) the modern WiX toolchain - dotnet tool, user-local,
# no admin rights, no system-wide changes.
# ---------------------------------------------------------------------------
$ToolsDir = Join-Path $env:LOCALAPPDATA "jpegview-tools"
New-Item -ItemType Directory -Force -Path $ToolsDir | Out-Null
$WixExe = Join-Path $ToolsDir "wix\wix.exe"
if (-not (Test-Path $WixExe)) {
	Write-Host "==> Installing WiX dotnet tool (user-local)..." -ForegroundColor Cyan
	$DotnetDir = Join-Path $ToolsDir "dotnet"
	if (-not (Test-Path (Join-Path $DotnetDir "dotnet.exe"))) {
		Invoke-WebRequest -Uri "https://dot.net/v1/dotnet-install.ps1" -OutFile "$ToolsDir\dotnet-install.ps1" -UseBasicParsing
		& "$ToolsDir\dotnet-install.ps1" -Channel 8.0 -InstallDir $DotnetDir
	}
	$env:DOTNET_ROOT = $DotnetDir
	# pinned to WiX v6 (stable, MIT, no v7 OSMF EULA gate)
	& (Join-Path $DotnetDir "dotnet.exe") tool install wix --tool-path (Join-Path $ToolsDir "wix") --version "6.*"
}
$env:DOTNET_ROOT = Join-Path $ToolsDir "dotnet"

# one-time: fetch the UI extension into the wix extension cache (idempotent).
# pinned to the same version as the wix tool so v6 tool doesn't grab a v7 ext.
$WixVer = (& $WixExe --version) -replace "[^0-9.].*$", ""
& $WixExe extension add -g "WixToolset.UI.wixext/$WixVer" 2>&1 | ForEach-Object { Write-Verbose $_ }
if ($LASTEXITCODE -ne 0) { throw "Failed to install WixToolset.UI.wixext/$WixVer extension" }

Write-Host "==> Building MSI with WiX..." -ForegroundColor Cyan
$MsiOut = Join-Path $OutDir "JPEGView64_en-us_$Version.msi"
& $WixExe build `
	-arch x64 `
	-ext WixToolset.UI.wixext `
	-loc (Join-Path $SetupDir "wix6\Product_en-us.wxl") `
	-d "JPEGView.ProjectDir=$(Join-Path $RepoRoot 'src\JPEGView')" `
	-d "JPEGView.TargetDir=$BinDir\" `
	-d "SetupResDir=$(Join-Path $SetupDir 'res')" `
	(Join-Path $SetupDir "wix6\Product.wxs") `
	-intermediatefolder (Join-Path $env:TEMP "jpegview-wix-obj") `
	-o $MsiOut
if ($LASTEXITCODE -ne 0) { throw "WiX build failed with exit code $LASTEXITCODE" }
# wixpdb is build metadata, not a release artifact
Remove-Item (Join-Path $OutDir "JPEGView64_en-us_$Version.wixpdb") -Force -ErrorAction SilentlyContinue

# ---------------------------------------------------------------------------
# Portable ZIP - mirrors the upstream release layout: JPEGView64/ folder
# ---------------------------------------------------------------------------
Write-Host "==> Staging portable layout..." -ForegroundColor Cyan
$Stage = Join-Path $env:TEMP "jpegview-zip-stage"
Remove-Item $Stage -Recurse -Force -ErrorAction SilentlyContinue
$Stage64 = Join-Path $Stage "JPEGView64"
New-Item -ItemType Directory -Force -Path $Stage64 | Out-Null

Copy-Item (Join-Path $BinDir "*.*") $Stage64
# strip intermediates, same as CI upload cleanup
foreach ($junk in @("*.pdb", "*.exp", "*.lib")) {
	Get-ChildItem $Stage64 -Filter $junk | Remove-Item -Force
}
# extra docs shipped in releases
foreach ($doc in @("HowToInstall.txt", "HowToInstall_ru.txt", "CHANGELOG.txt")) {
	$src = Join-Path $RepoRoot $doc
	if (Test-Path $src) { Copy-Item $src $Stage64 }
}

Write-Host "==> Creating ZIP..." -ForegroundColor Cyan
$ZipOut = Join-Path $OutDir "JPEGView_$Version.zip"
Compress-Archive -Path (Join-Path $Stage "JPEGView64") -DestinationPath $ZipOut -Force
Remove-Item $Stage -Recurse -Force

# ---------------------------------------------------------------------------
# Checksums
# ---------------------------------------------------------------------------
Write-Host "==> Generating SHA256SUMS..." -ForegroundColor Cyan
$sums = Join-Path $OutDir "SHA256SUMS.txt"
# Start fresh so re-runs don't append stale hashes from a previous build.
Remove-Item $sums -ErrorAction SilentlyContinue
Get-ChildItem $OutDir -File | Where-Object { $_.Name -ne "SHA256SUMS.txt" } | ForEach-Object {
	$hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower()
	"$hash  $($_.Name)" | Out-File $sums -Append -Encoding ascii
}

Write-Host "`nDone. Artifacts in '$OutDir':" -ForegroundColor Green
Get-ChildItem $OutDir -File | Format-Table Name, @{L="MB"; E={"{0:N2}" -f ($_.Length / 1MB)}} -AutoSize
