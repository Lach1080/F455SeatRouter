# deploy.ps1
# Packages F455SeatRouter into a self-contained deployable folder.
#
# Usage (run from repo root after building Release in VS2022):
#   powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1
#
# Output: dist\F455SeatRouter\  (zip this folder and copy to target machine)

param(
    [string]$BuildConfig = "Release"
)

$ErrorActionPreference = "Stop"
$RepoRoot  = Split-Path $PSScriptRoot -Parent
$BuildDir  = Join-Path $RepoRoot "x64\$BuildConfig"
$DistDir   = Join-Path $RepoRoot "dist\F455SeatRouter"
$SdkBinDir = Join-Path $RepoRoot "sdk\bin\x64\Release"

Write-Host "=== F455SeatRouter Deploy ===" -ForegroundColor Cyan
Write-Host "Build dir : $BuildDir"
Write-Host "SDK bin   : $SdkBinDir"
Write-Host "Output    : $DistDir"
Write-Host ""

# --- Validate EXE exists ---
$ExePath = Join-Path $BuildDir "F455SeatRouter.exe"
if (-not (Test-Path $ExePath)) {
    Write-Error "EXE not found at $ExePath. Build the Release configuration in Visual Studio first."
    exit 1
}

# --- Clean and recreate dist folder ---
if (Test-Path $DistDir) { Remove-Item $DistDir -Recurse -Force }
New-Item -ItemType Directory -Path $DistDir | Out-Null

# --- Copy EXE ---
Copy-Item $ExePath $DistDir
Write-Host "[OK] Copied F455SeatRouter.exe"

# --- Copy config.json ---
# Prefer the runtime config (x64\Release\config.json) as it holds the actual
# site settings (mode, CMS host/port, paths). Fall back to repo root if absent.
$ConfigSrc = Join-Path $BuildDir "config.json"
if (-not (Test-Path $ConfigSrc)) {
    Write-Warning "Runtime config.json not found at $ConfigSrc - falling back to repo root copy."
    $ConfigSrc = Join-Path $RepoRoot "config.json"
}
if (Test-Path $ConfigSrc) {
    Copy-Item $ConfigSrc $DistDir
    Write-Host "[OK] Copied config.json (from $ConfigSrc)"
} else {
    Write-Warning "config.json not found - you must copy it to the dist folder manually."
}

# --- Copy SDK DLLs ---
if (Test-Path $SdkBinDir) {
    $Dlls = Get-ChildItem $SdkBinDir -Filter "*.dll"
    if ($Dlls.Count -eq 0) {
        Write-Warning "No DLLs found in $SdkBinDir"
    }
    foreach ($dll in $Dlls) {
        Copy-Item $dll.FullName $DistDir
        Write-Host "[OK] Copied $($dll.Name)"
    }
} else {
    Write-Warning "SDK bin directory not found: $SdkBinDir"
    Write-Warning "Copy rsid.dll and any required runtime DLLs into the dist folder manually."
}

# --- Copy any Visual C++ runtime DLLs present in build dir ---
$RuntimeDlls = @("msvcp140.dll","vcruntime140.dll","vcruntime140_1.dll")
foreach ($rt in $RuntimeDlls) {
    $rtPath = Join-Path $BuildDir $rt
    if (Test-Path $rtPath) {
        Copy-Item $rtPath $DistDir
        Write-Host "[OK] Copied runtime $rt"
    }
}

# --- Create a zip ---
$ZipPath = Join-Path (Split-Path $DistDir -Parent) "F455SeatRouter.zip"
if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
Compress-Archive -Path $DistDir -DestinationPath $ZipPath
Write-Host ""
Write-Host "=== Done ===" -ForegroundColor Green
Write-Host "Deployable folder : $DistDir"
Write-Host "Deployable zip    : $ZipPath"
Write-Host ""
Write-Host "On the target machine:" -ForegroundColor Yellow
Write-Host "  1. Extract F455SeatRouter.zip to any folder"
Write-Host "  2. Edit config.json (set port, CMS host, seats, routes)"
Write-Host "  3. Run F455SeatRouter.exe"
