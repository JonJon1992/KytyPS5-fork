@echo off
rem Release build to share: Release, LTO, for any x86-64-v3 CPU (AVX2: Intel Haswell 2013+ or AMD Zen).
rem build-release.bat tunes for the building CPU (-march=native) and can crash on others. Output: _Build\windows-portable\install
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Program Files\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%
rem Qt and glslangValidator locations for the first configure; set KYTY_QT_DIR / KYTY_GLSLANG to override.
if not defined KYTY_QT_DIR set KYTY_QT_DIR=C:/Qt/6.10.3/msvc2022_64
if not defined KYTY_GLSLANG set KYTY_GLSLANG=%USERPROFILE%\BuildTools\glslang\bin\glslang.exe
if not exist _Build\windows-portable\CMakeCache.txt (
  cmake -S . -B _Build\windows-portable -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
    "-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /clang:-march=x86-64-v3" "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /GR /EHsc /clang:-march=x86-64-v3" -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON ^
    "-DCMAKE_PREFIX_PATH=%KYTY_QT_DIR%" "-DKYTY_GLSLANG_VALIDATOR=%KYTY_GLSLANG%" || exit /b 1
)
cmake --build _Build\windows-portable --target launcher || exit /b 1
cmake --install _Build\windows-portable --prefix _Build\windows-portable\install || exit /b 1
echo BUILD OK
