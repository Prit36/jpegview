// Minimal ATL base module definition so ParallelJPEG.cpp's stdafx.h includes link.
#define _ATL_NO_DEFAULT_LIBS
#include <atlbase.h>
#include <atlapp.h>

namespace ATL {
	CAtlBaseModule::CAtlBaseModule() throw() {
		cbSize = sizeof(_ATL_BASE_MODULE);
		m_hInst = ::GetModuleHandle(NULL);
		m_hInstResource = m_hInst;
		dwAtlBuildVer = _ATL_VER;
		pguidVer = &GUID_NULL;
		m_csResource.Init();
	}
	CAtlBaseModule::~CAtlBaseModule() throw() {
		m_csResource.Term();
	}
	CAtlBaseModule _AtlBaseModule;
}
