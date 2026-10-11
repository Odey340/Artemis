# Building Artemis on Windows

Artemis needs two system libraries, **libcurl** and **OpenSSL**, on top of the dependencies CMake downloads for you (spdlog, nlohmann/json, GoogleTest). On Windows the easiest way to get them is [vcpkg](https://github.com/microsoft/vcpkg).

> The Windows build has not been run in CI yet; the Linux build is the reference. If something fails, the MSYS2 route at the bottom is the most Linux-like fallback.

## Prerequisites

1. **Visual Studio 2022** (Community is fine) with the **Desktop development with C++** workload.
2. **CMake ≥ 3.20**: `winget install Kitware.CMake`
3. **Git**: `winget install Git.Git`
4. **vcpkg** + the two libraries:

   ```powershell
   git clone https://github.com/microsoft/vcpkg C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat
   C:\vcpkg\vcpkg install curl:x64-windows openssl:x64-windows
   setx VCPKG_ROOT C:\vcpkg        # then open a new PowerShell
   ```

`.\setup_windows.ps1` checks all of the above.

## Building

### Option 1: build script (recommended)

```powershell
.\build_windows.ps1
```

The script uses `%VCPKG_ROOT%` to find vcpkg's CMake toolchain automatically.

### Option 2: by hand

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build build --config Release
ctest --test-dir build -C Release
```

### Option 3: Visual Studio

Open the folder (File → Open → Folder). Visual Studio picks up `CMakeLists.txt`; set `CMAKE_TOOLCHAIN_FILE` to vcpkg's toolchain in *CMake Settings*, then Build → Build All.

## Running

```powershell
python py\make_sample_data.py
.\build\Release\artemis.exe backtest data\sample_books.csv -c config\backtest.sample.json

$env:KALSHI_API_KEY_ID = "your-key-id"
$env:KALSHI_PRIVATE_KEY_PATH = "C:\Users\you\.kalshi\key.pem"
.\build\Release\artemis.exe balance
.\build\Release\artemis.exe run          # paper trading
```

vcpkg copies `libcurl.dll` and the OpenSSL DLLs next to `artemis.exe` at build time. If you move the executable, take those DLLs with it.

## Troubleshooting

* **`Could NOT find CURL` / `Could NOT find OpenSSL`:** CMake was configured without the vcpkg toolchain. Delete `build\` and configure again with `-DCMAKE_TOOLCHAIN_FILE=...`.
* **TLS errors at runtime (`SSL certificate problem`):** the vcpkg curl uses the Windows certificate store. Make sure the system clock is correct and no proxy is intercepting TLS.
* **CMake can't download dependencies:** CMake needs Git and internet access for spdlog / json / GoogleTest the first time.

## Alternative: MSYS2 / MinGW

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-curl \
          mingw-w64-ucrt-x86_64-openssl make
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```
