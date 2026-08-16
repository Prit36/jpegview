#include "StdAfx.h"
#include "FileExtensionsRegistry.h"
#include "Helpers.h"
#include "SettingsProvider.h"
#include "NLS.h"
#include "FileList.h"
#include <shlobj.h>

// Gets a string value from the registry, given an open key and the name of the string value to get.
// If the name is NULL, the default value for the key is returned.
static bool GetRegistryStringValue(HKEY key, LPCTSTR name, CString & outValue) {
	DWORD type;
	const int buffsize = 512;
	TCHAR buff[buffsize];
	DWORD size = buffsize * sizeof(TCHAR);
	bool bOk = RegQueryValueEx(key, name, NULL, &type, (LPBYTE)&buff, &size) == ERROR_SUCCESS;
	bOk = bOk && (type == REG_SZ || type == REG_EXPAND_SZ);
	if (bOk) outValue = buff;
	return bOk;
}

// Checks if a registry key relative to HKEY_CURRENT_USER has the specified string value.
// If expectedValue is NULL, it is just checked if the keyName exists.
static bool ExistsAndHasStringValue(LPCTSTR path, LPCTSTR keyName, LPCTSTR expectedValue) {
	HKEY key;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, path, 0, KEY_READ, &key) == ERROR_SUCCESS) {
		CString value;
		bool bOk = GetRegistryStringValue(key, keyName, value);
		bOk = bOk && (expectedValue == NULL || _tcsicmp(value, expectedValue) == 0);
		RegCloseKey(key);
		return bOk;
	}
	return false;
}

// Sets a string value in the registry, given an open key and the name of the string value to set.
// If the name is NULL, the default value for the key is set.
static bool SetRegistryStringValue(HKEY key, LPCTSTR name, LPCTSTR stringValue) {
	return RegSetValueEx(key, name, 0, REG_SZ, (const BYTE *)stringValue, ((int)_tcslen(stringValue) + 1) * sizeof(TCHAR)) == ERROR_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////////
// CFileExtensionsRegistration
///////////////////////////////////////////////////////////////////////////////////

CFileExtensionsRegistration::CFileExtensionsRegistration() {
}

bool CFileExtensionsRegistration::RegisterJPEGView() {
	// ProgId
	CString startupCommand;
	startupCommand.Format(_T("\"%sJPEGView.exe\" \"%%1\""), CSettingsProvider::This().GetEXEPath());
	if (!WriteStringValue(_T("Software\\Classes\\JPEGViewImageFile\\shell\\open\\command"), NULL, startupCommand)) {
		return false;
	}

	// Set as RegisteredApplications and declare capabilities
	if (!WriteStringValue(_T("Software\\RegisteredApplications"), _T("JPEGView"), _T("Software\\JPEGView\\Capabilities"))) {
		return false;
	}
	if (!WriteStringValue(_T("Software\\JPEGView\\Capabilities"), _T("ApplicationDescription"), 
		CNLS::GetString(_T("JPEGView is a lean, fast and highly configurable viewer/editor for JPEG, BMP, PNG, WEBP, TGA, GIF and TIFF images with a minimal GUI.")))) {
		return false;
	}
	if (!WriteStringValue(_T("Software\\JPEGView\\Capabilities"), _T("ApplicationName"), _T("JPEGView"))) {
		return false;
	}
	
	// Declare all supported file endings
	CString fileEndings = CFileList::GetSupportedFileEndings();
	int length = fileEndings.GetLength();
	LPTSTR buffer = fileEndings.GetBuffer(length + 1);
	LPCTSTR fileEnding = buffer;
	for (int i = 0; i <= length; i++) {
		if (buffer[i] == _T(';') || buffer[i] == 0) {
			fileEnding++; // strip the *
			buffer[i] = 0;
			if (!WriteStringValue(_T("Software\\JPEGView\\Capabilities\\FileAssociations"), fileEnding, _T("JPEGViewImageFile"))) {
				return false;
			}
			fileEnding = &(buffer[i + 1]); 
		}
	}
	fileEndings.ReleaseBuffer();

	return true;
}

void CFileExtensionsRegistration::LaunchApplicationAssociationDialog() {
	IApplicationAssociationRegistrationUI *applicationAssociationRegistrationUI = NULL;

	HRESULT hr = ::CoCreateInstance(CLSID_ApplicationAssociationRegistrationUI,
		NULL,
		CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&applicationAssociationRegistrationUI));

	if (SUCCEEDED(hr) && applicationAssociationRegistrationUI) {
		applicationAssociationRegistrationUI->LaunchAdvancedAssociationUI(L"JPEGView");
		applicationAssociationRegistrationUI->Release();
	}
}

bool CFileExtensionsRegistration::WriteStringValue(LPCTSTR path, LPCTSTR keyName, LPCTSTR value) {
	if (!ExistsAndHasStringValue(path, keyName, value)) {
		HKEY key;
		if (RegOpenKeyEx(HKEY_CURRENT_USER, path, 0, KEY_WRITE, &key) != ERROR_SUCCESS) {
			if (RegCreateKeyEx(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_WRITE | KEY_READ, NULL, &key, NULL) != ERROR_SUCCESS) {
				m_lastFailedRegistryKey = path;
				return false;
			}
		}

		bool canWrite = SetRegistryStringValue(key, keyName, value);
		if (!canWrite)
			m_lastFailedRegistryKey = CString(path) + _T('-') + ((keyName == NULL) ? _T("[Default]") : keyName);

		RegCloseKey(key);

		return canWrite;
	}
	// String value already exists and has the correct value
	return true;
}