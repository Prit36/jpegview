// stdafx.cpp : source file that includes just the standard includes
//	JPEGView.pch will be the pre-compiled header
//	stdafx.obj will contain the pre-compiled type information

#include "stdafx.h"

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

	bool CAtlBaseModule::AddResourceInstance(HINSTANCE hInst) throw() {
		CComCritSecLock<CComCriticalSection> lock(m_csResource);
		return m_rgResourceInstance.Add(hInst);
	}

	bool CAtlBaseModule::RemoveResourceInstance(HINSTANCE hInst) throw() {
		CComCritSecLock<CComCriticalSection> lock(m_csResource);
		return m_rgResourceInstance.Remove(hInst);
	}

	HINSTANCE CAtlBaseModule::GetHInstanceAt(int i) throw() {
		if (i == 0) return m_hInstResource;
		CComCritSecLock<CComCriticalSection> lock(m_csResource);
		if (i > 0 && i <= m_rgResourceInstance.GetSize()) {
			return m_rgResourceInstance[i - 1];
		}
		return NULL;
	}

	CAtlBaseModule _AtlBaseModule;

#if defined(_M_IX86)
	void* __stdcall __AllocStdCallThunk(void) {
		return ::VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	}

	void __stdcall __FreeStdCallThunk(void* p) {
		if (p) ::VirtualFree(p, 0, MEM_RELEASE);
	}
#else
	void* __cdecl __AllocStdCallThunk(void) {
		return ::VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	}

	void __cdecl __FreeStdCallThunk(void* p) {
		if (p) ::VirtualFree(p, 0, MEM_RELEASE);
	}
#endif
}