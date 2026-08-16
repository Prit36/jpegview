// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently

#pragma once

// disable these useless warnings
#pragma warning(disable:4018)
#pragma warning(disable:4800)

// Modern Windows 10/11 Targeting
#ifndef WINVER
#define WINVER          0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT    0x0A00
#endif
#ifndef _WIN32_IE
#define _WIN32_IE       0x0A00
#endif
#define _RICHEDIT_VER   0x0300

#define _CRT_SECURE_NO_DEPRECATE
#define _ATL_NO_DEFAULT_LIBS
#define USE_ATL_THUNK1

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <tchar.h>
#include <dwmapi.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <span>
#include <memory>
#include <string>
#include <string_view>
#include <atlbase.h>
#pragma warning(push)
#pragma warning(disable:4996)
#pragma warning(disable:4838)
#pragma warning(disable:4302)
#include <atlapp.h>
#include <assert.h>

extern CAppModule _Module;

#include <atlwin.h>

#include <atlframe.h>
#include <atlctrls.h>
#include <atldlgs.h>
#include <atlmisc.h>
#include <atlscrl.h>

#pragma warning(pop)

// STL stuff
#include <list>


// own stuff
#include "ImageProcessingTypes.h"

#define VK_PAGE_UP 0x021
#define VK_PAGE_DOWN 0x22
#define VK_PLUS 0x6b
#define VK_MINUS 0x6d

// a type that has enough bits to hold a pointer and allowing arithmetic operations
#define PTR_INTEGRAL_TYPE unsigned long long

#ifndef _UNICODE
#error _UNICODE symbol must be defined
#endif

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")

