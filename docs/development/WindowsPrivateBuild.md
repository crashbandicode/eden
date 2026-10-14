# Private Windows build

The GIF release uses Windows x64, Visual Studio 2022 Build Tools 17.14 (MSVC
19.44), CMake 4.4, Ninja, Python 3.12, Vulkan SDK 1.4.341.1, and the complete
official **Qt 6.8.3 MSVC2022 x64 shared** distribution, including Qt Charts.
The fork's control server requires Qt Network with local-server support; the
bundled static Qt archive used by this revision does not provide that feature.

From PowerShell 7, load the Visual Studio developer environment through
`Common7/Tools/Launch-VsDevShell.ps1 -Arch amd64 -HostArch amd64` in the installed
Build Tools directory. Put CMake, Ninja, Python, Git, and the Vulkan SDK's `Bin`
directory on `PATH`, and set `VULKAN_SDK` to that SDK directory. Then, from the
source root (replace the Qt path):

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DYUZU_TESTS=OFF -DENABLE_QT=ON -DYUZU_USE_BUNDLED_QT=OFF -DQT_DIR=C:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6 -DQt6_DIR=C:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6 -DQT_HOST_PATH=C:/Qt/6.8.3/msvc2022_64 -DQT_TARGET_PATH=C:/Qt/6.8.3/msvc2022_64 -DYUZU_USE_BUNDLED_SDL3=ON -DYUZU_USE_BUNDLED_FFMPEG=ON -DYUZU_USE_BUNDLED_OPENSSL=ON -DYUZU_USE_BUNDLED_SIRIT=ON -DENABLE_LIBUSB=ON -DENABLE_OPENGL=ON -DYUZU_CMD=ON
cmake --build build --parallel 4
```

The executables, Qt DLLs, and plugin directories are under `build/bin`. Keep
them together and add `qt.conf` containing:

```ini
[Paths]
Plugins=plugins
```

The target machine needs the Microsoft Visual C++ 2022 x64 runtime. Launch
`eden.exe --control-help` to check dependency loading before opening the GUI.
Games, firmware, keys, saves, and user settings are external to this build and
are not part of the release archive.

The standalone GIF encoder test project is `tools/tests/gif_writer`. Configure
it with `CMAKE_PREFIX_PATH` pointing to the same Qt distribution, build it, then
run CTest. Its independent decoding checks use Python and Pillow.

The service-dispatch regression test is `tools/tests/service_dispatch`; configure
it with `FROZEN_INCLUDE_DIR` pointing to the fetched Frozen `include` directory.
MSVC 19.44 miscompiles constant-evaluated tables containing null member-function
pointers. This fork initializes immutable dispatch tables at runtime on MSVC,
while GCC/Clang retain constant evaluation. Defining `EDEN_REPRO_BROKEN` in this
standalone test restores the defective path as a negative control (expected to
fail on MSVC 19.44). Both the normal MSVC and GCC test paths must pass.
