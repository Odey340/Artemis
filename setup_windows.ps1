# Windows Setup Script for Artemis
Write-Host "=== Artemis Windows Setup ===" -ForegroundColor Green
Write-Host ""

# Check for CMake
$cmakeFound = $false
$cmakeCheck = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCheck) {
    Write-Host "CMake found" -ForegroundColor Green
    $cmakeFound = $true
} else {
    Write-Host "CMake not found" -ForegroundColor Red
}

# Check for Visual Studio
$vsFound = $false
if (Test-Path "C:\Program Files\Microsoft Visual Studio") {
    $vsVersions = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Directory -ErrorAction SilentlyContinue
    if ($vsVersions -and $vsVersions.Count -gt 0) {
        Write-Host "Visual Studio found" -ForegroundColor Green
        $vsFound = $true
    }
}
if (-not $vsFound) {
    Write-Host "Visual Studio not found" -ForegroundColor Red
}

# Check for Git
$gitFound = $false
$gitCheck = Get-Command git -ErrorAction SilentlyContinue
if ($gitCheck) {
    Write-Host "Git found" -ForegroundColor Green
    $gitFound = $true
} else {
    Write-Host "Git not found" -ForegroundColor Red
}

# Check for vcpkg (provides libcurl + OpenSSL)
$vcpkgFound = $false
if ($env:VCPKG_ROOT -and (Test-Path "$env:VCPKG_ROOT\vcpkg.exe")) {
    Write-Host "vcpkg found at $env:VCPKG_ROOT" -ForegroundColor Green
    $vcpkgFound = $true
} else {
    Write-Host "vcpkg not found (VCPKG_ROOT not set)" -ForegroundColor Red
}

Write-Host ""
Write-Host "=== Installation Instructions ===" -ForegroundColor Yellow
Write-Host ""

if (-not $cmakeFound) {
    Write-Host "1. Install CMake (3.20 or later):" -ForegroundColor Cyan
    Write-Host "   Download from: https://cmake.org/download/" -ForegroundColor White
    Write-Host "   Or use: winget install Kitware.CMake" -ForegroundColor White
    Write-Host ""
}

if (-not $vsFound) {
    Write-Host "2. Install Visual Studio 2022:" -ForegroundColor Cyan
    Write-Host "   Download from: https://visualstudio.microsoft.com/" -ForegroundColor White
    Write-Host "   Install Desktop development with C++ workload" -ForegroundColor White
    Write-Host ""
}

if (-not $gitFound) {
    Write-Host "3. Install Git:" -ForegroundColor Cyan
    Write-Host "   Download from: https://git-scm.com/download/win" -ForegroundColor White
    Write-Host "   Or use: winget install Git.Git" -ForegroundColor White
    Write-Host ""
}

if (-not $vcpkgFound) {
    Write-Host "4. Install vcpkg + libraries:" -ForegroundColor Cyan
    Write-Host "   git clone https://github.com/microsoft/vcpkg C:\vcpkg" -ForegroundColor White
    Write-Host "   C:\vcpkg\bootstrap-vcpkg.bat" -ForegroundColor White
    Write-Host "   C:\vcpkg\vcpkg install curl:x64-windows openssl:x64-windows" -ForegroundColor White
    Write-Host "   setx VCPKG_ROOT C:\vcpkg   (then reopen PowerShell)" -ForegroundColor White
    Write-Host ""
}

if ($cmakeFound -and $vsFound -and $gitFound -and $vcpkgFound) {
    Write-Host "=== All prerequisites met! ===" -ForegroundColor Green
    Write-Host ""
    Write-Host "To build, run: .\build_windows.ps1" -ForegroundColor Yellow
} else {
    Write-Host "=== Please install missing prerequisites ===" -ForegroundColor Red
}
