#pragma once

// The loader sources include <Windows.h> (valid on Windows, where the
// filesystem is case-insensitive). mingw-w64 ships only windows.h, so a Linux
// cross build needs this shim; it is added to the include path by
// scripts/build-cross-linux.sh and scripts/toolchains/mingw-w64-x86_64.cmake.
#include <windows.h>
