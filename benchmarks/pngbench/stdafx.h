// Minimal stand-in for stdafx.h so we can compile PNGWrapper.cpp standalone
// (the real stdafx.h pulls in ATL/WTL which we don't need for benchmarking).
#pragma once

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_DEPRECATE

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <new>
#include <stdexcept>
#include <cstdint>
#include <cstddef>
