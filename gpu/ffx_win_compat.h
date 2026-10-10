// SPDX-License-Identifier: MIT
// Portability shim for AMD FidelityFX sources on Windows with non-MSVC compilers (MinGW / Clang).
// FidelityFX host sources rely on MSVC-specific CRT transitive headers and macros like _countof.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <cmath>
#include <new>
#include <mutex>

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif
