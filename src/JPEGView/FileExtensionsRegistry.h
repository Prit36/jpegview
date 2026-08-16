#pragma once

// Class to register file extensions in modern Windows (10/11).
// Registers JPEGView in HKCU\Software\RegisteredApplications, publishes its capabilities,
// and invokes the system Default Programs application association dialog.
class CFileExtensionsRegistration
{
public:
	CFileExtensionsRegistration();

	// Register JPEGView in HKEY_CURRENT_USER\Software\RegisteredApplications
	// Also publishes the capabilities and creates a ProgId for JPEGView.
	bool RegisterJPEGView();

	// Launches the Windows application association dialog (Default Programs)
	void LaunchApplicationAssociationDialog();

	// Gets the key name of the last failed key to write
	CString GetLastFailedRegistryKey() const { return m_lastFailedRegistryKey; }

private:
	CString m_lastFailedRegistryKey;

	bool WriteStringValue(LPCTSTR path, LPCTSTR keyName, LPCTSTR value);
};