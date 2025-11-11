# Building Artemis on Windows

## Prerequisites

1. **CMake** (≥3.20)
   - Download from: https://cmake.org/download/
   - Or use: `winget install Kitware.CMake`

2. **Visual Studio 2022** (Community edition is fine)
   - Download from: https://visualstudio.microsoft.com/
   - Install the **"Desktop development with C++"** workload
   - This includes the MSVC compiler and build tools

3. **Git** (for fetching dependencies)
   - Download from: https://git-scm.com/download/win
   - Or use: `winget install Git.Git`

## Quick Setup

Run the setup script to check prerequisites:

```powershell
.\setup_windows.ps1
```

## Building

### Option 1: Using the build script (Recommended)

```powershell
.\build_windows.ps1
```

### Option 2: Manual build

```powershell
# Create build directory
mkdir build
cd build

# Configure with CMake
cmake .. -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build . --config Release
```

### Option 3: Using Visual Studio

1. Open Visual Studio
2. File → Open → CMake → Select `CMakeLists.txt`
3. Build → Build All

## Running

After building, the executable will be in `build\Release\artemis.exe`:

```powershell
.\build\Release\artemis.exe data\ES_futures_sample.csv 2.5
```

## Running Tests

```powershell
cd build
.\Release\artemis_tests.exe
```

Or using CTest:

```powershell
cd build
ctest -C Release
```

## Troubleshooting

### CMake not found
- Make sure CMake is installed and added to PATH
- Restart PowerShell after installing CMake
- Verify with: `cmake --version`

### Visual Studio not found
- Install Visual Studio 2022 with C++ workload
- Or use Visual Studio Build Tools: https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022

### Build errors
- Make sure you're using a Visual Studio Developer Command Prompt
- Or run: `& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"` before building

### Git not found
- CMake needs Git to fetch spdlog and GoogleTest
- Install Git and add to PATH
- Restart PowerShell after installing

## Alternative: Using MSYS2/MinGW

If you prefer GCC on Windows:

1. Install MSYS2 from https://www.msys2.org/
2. Install toolchain: `pacman -S mingw-w64-x86_64-gcc cmake make`
3. Build from MSYS2 terminal:
   ```bash
   mkdir build && cd build
   cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
   cmake --build .
   ```

