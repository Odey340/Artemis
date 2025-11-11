# Windows Build Script for Artemis
# This script builds the project on Windows

Write-Host "=== Building Artemis on Windows ===" -ForegroundColor Green
Write-Host ""

# Check if CMake is available
try {
    $null = cmake --version 2>&1
} catch {
    Write-Host "Error: CMake not found. Please install CMake first." -ForegroundColor Red
    Write-Host "Run setup_windows.ps1 for installation instructions." -ForegroundColor Yellow
    exit 1
}

# Create build directory
if (-not (Test-Path "build")) {
    New-Item -ItemType Directory -Path "build" | Out-Null
    Write-Host "Created build directory" -ForegroundColor Green
}

# Change to build directory
Set-Location build

# Configure with CMake
Write-Host "Configuring project with CMake..." -ForegroundColor Yellow
try {
    # Try to find Visual Studio generator
    $generators = @(
        "Visual Studio 17 2022",
        "Visual Studio 16 2019",
        "Visual Studio 15 2017",
        "Visual Studio 14 2015"
    )
    
    $generatorFound = $false
    foreach ($gen in $generators) {
        try {
            cmake .. -G $gen -DCMAKE_BUILD_TYPE=Release 2>&1 | Out-Null
            if ($LASTEXITCODE -eq 0) {
                Write-Host "✓ Configured with $gen" -ForegroundColor Green
                $generatorFound = $true
                break
            }
        } catch {
            continue
        }
    }
    
    if (-not $generatorFound) {
        # Try default generator
        cmake .. -DCMAKE_BUILD_TYPE=Release
        if ($LASTEXITCODE -ne 0) {
            throw "CMake configuration failed"
        }
        Write-Host "✓ Configured with default generator" -ForegroundColor Green
    }
} catch {
    Write-Host "Error: CMake configuration failed" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    Set-Location ..
    exit 1
}

# Build the project
Write-Host ""
Write-Host "Building project..." -ForegroundColor Yellow
try {
    cmake --build . --config Release
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed"
    }
    Write-Host "✓ Build successful!" -ForegroundColor Green
} catch {
    Write-Host "Error: Build failed" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    Set-Location ..
    exit 1
}

# Return to root directory
Set-Location ..

# Find the executable
$exePath = ""
$possiblePaths = @(
    "build\Release\artemis.exe",
    "build\Debug\artemis.exe",
    "build\artemis.exe"
)

foreach ($path in $possiblePaths) {
    if (Test-Path $path) {
        $exePath = $path
        break
    }
}

if ($exePath) {
    Write-Host ""
    Write-Host "=== Build Complete ===" -ForegroundColor Green
    Write-Host "Executable: $exePath" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "To run the backtest:" -ForegroundColor Yellow
    Write-Host "  .\$exePath data\ES_futures_sample.csv 2.5" -ForegroundColor White
} else {
    Write-Host ""
    Write-Host "Build completed, but executable not found in expected locations." -ForegroundColor Yellow
    Write-Host "Please check the build directory manually." -ForegroundColor Yellow
}

