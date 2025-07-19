#pragma once

#include "VhdToDisk.h"

class CDiskToVhd
{
	VHD_FOOTER	m_Foot;
	VHD_DYNAMIC m_Dyn;

	HANDLE		m_hVhdFile;
	HANDLE		m_hPhysicalDrive;

public:
	CDiskToVhd(void);
	~CDiskToVhd(void);

	BOOL DumpDiskToVhd(const LPWSTR sDrive, const LPWSTR sVhdPath, const HWND hDlg);

protected:
	BOOL OpenPhysicalDrive(LPWSTR sDrive);
	BOOL ClosePhysicalDrive();

	BOOL CreateVhdFile(LPWSTR sPath);
	BOOL CloseVhdFile();

	BOOL InitializeVhdStructures(UINT64 diskSize);
	BOOL WriteFooter();
	BOOL WriteDynHeader();
	BOOL WriteBlockAllocationTable();
	
	BOOL ReadAndWriteDiskData(HWND hDlg);
	UINT64 GetDiskSize();
	
	BOOL DumpDiskToVhdData(HWND hDlg);
};