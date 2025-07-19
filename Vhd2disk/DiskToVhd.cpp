#include "StdAfx.h"
#include "Trace.h"
#include "DiskToVhd.h"
#include "resource.h"
#include <time.h>
#include <winioctl.h>

CDiskToVhd::CDiskToVhd(void)
{
	m_hVhdFile = NULL;
	m_hPhysicalDrive = NULL;

	ZeroMemory(&m_Foot, sizeof(VHD_FOOTER));
	ZeroMemory(&m_Dyn, sizeof(VHD_DYNAMIC));
}

CDiskToVhd::~CDiskToVhd(void)
{
	if(m_hVhdFile)
		CloseVhdFile();

	if(m_hPhysicalDrive)
		ClosePhysicalDrive();
}

BOOL CDiskToVhd::OpenPhysicalDrive(LPWSTR sDrive)
{
	m_hPhysicalDrive = CreateFile(sDrive
		, GENERIC_READ
		, FILE_SHARE_READ | FILE_SHARE_WRITE
		, NULL
		, OPEN_EXISTING
		, FILE_FLAG_SEQUENTIAL_SCAN
		, NULL);

	if(m_hPhysicalDrive == INVALID_HANDLE_VALUE)
	{
		m_hPhysicalDrive = NULL;
		return FALSE;
	}

	return TRUE;
}

BOOL CDiskToVhd::ClosePhysicalDrive()
{
	if(m_hPhysicalDrive)
	{
		CloseHandle(m_hPhysicalDrive);
		m_hPhysicalDrive = NULL;
	}

	return TRUE;
}

BOOL CDiskToVhd::CreateVhdFile(LPWSTR sPath)
{
	m_hVhdFile = CreateFile(sPath
		, GENERIC_WRITE
		, 0
		, NULL
		, CREATE_ALWAYS
		, FILE_ATTRIBUTE_NORMAL
		, NULL);

	if(m_hVhdFile == INVALID_HANDLE_VALUE)
	{
		m_hVhdFile = NULL;
		return FALSE;
	}

	return TRUE;
}

BOOL CDiskToVhd::CloseVhdFile()
{
	if(m_hVhdFile)
	{
		CloseHandle(m_hVhdFile);
		m_hVhdFile = NULL;
	}

	return TRUE;
}

UINT64 CDiskToVhd::GetDiskSize()
{
	if(!m_hPhysicalDrive)
		return 0;

	LARGE_INTEGER diskSize;
	if(!GetFileSizeEx(m_hPhysicalDrive, &diskSize))
	{
		// Try alternative method for physical drives
		DISK_GEOMETRY_EX geometry;
		DWORD bytesReturned;
		
		if(DeviceIoControl(m_hPhysicalDrive, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
			NULL, 0, &geometry, sizeof(geometry), &bytesReturned, NULL))
		{
			return geometry.DiskSize.QuadPart;
		}
		return 0;
	}

	return diskSize.QuadPart;
}

BOOL CDiskToVhd::InitializeVhdStructures(UINT64 diskSize)
{
	// Initialize VHD footer
	memcpy(m_Foot.cookie, "conectix", 8);
	m_Foot.features = _byteswap_ulong(0x00000002); // No features enabled
	m_Foot.version = _byteswap_ulong(0x00010000); // Version 1.0
	m_Foot.dataOffset = _byteswap_uint64(512); // Header starts at offset 512
	
	// Set timestamp (seconds since Jan 1, 2000 00:00:00 UTC)
	time_t currentTime;
	time(&currentTime);
	UINT32 vhdTimestamp = (UINT32)(currentTime - 946684800); // Subtract Jan 1, 2000
	m_Foot.timeStamp = _byteswap_ulong(vhdTimestamp);
	
	memcpy(m_Foot.creatorApplication, "vhd2", 4);
	m_Foot.creatorVersion = _byteswap_ulong(0x00010000);
	memcpy(m_Foot.creatorOS, "Wi2k", 4);
	
	m_Foot.originalSize = _byteswap_uint64(diskSize);
	m_Foot.currentSize = _byteswap_uint64(diskSize);
	
	// Set disk geometry (simplified)
	UINT32 totalSectors = (UINT32)(diskSize / 512);
	m_Foot.diskGeometry.cylinders = _byteswap_ushort((UINT16)(totalSectors / (16 * 63)));
	m_Foot.diskGeometry.heads = 16;
	m_Foot.diskGeometry.sectors = 63;
	
	m_Foot.diskType = _byteswap_ulong(3); // Dynamic disk
	
	// Generate UUID (simplified)
	srand((unsigned int)time(NULL));
	for(int i = 0; i < 16; i++)
		m_Foot.uniqueId[i] = (UCHAR)(rand() % 256);
	
	// Initialize dynamic header
	memcpy(m_Dyn.cookie, "cxsparse", 8);
	m_Dyn.dataOffset = _byteswap_uint64(0xFFFFFFFFFFFFFFFFULL); // No parent
	m_Dyn.tableOffset = _byteswap_uint64(1536); // BAT starts after header
	m_Dyn.headerVersion = _byteswap_ulong(0x00010000);
	
	UINT32 blockSize = 2 * 1024 * 1024; // 2MB blocks
	UINT32 maxTableEntries = (UINT32)((diskSize + blockSize - 1) / blockSize);
	
	m_Dyn.maxTableEntries = _byteswap_ulong(maxTableEntries);
	m_Dyn.blockSize = _byteswap_ulong(blockSize);
	
	// Copy parent UUID from footer
	memcpy(m_Dyn.parentUniqueId, m_Foot.uniqueId, 16);
	m_Dyn.parentTimeStamp = m_Foot.timeStamp;

	return TRUE;
}

BOOL CDiskToVhd::WriteFooter()
{
	if(!m_hVhdFile)
		return FALSE;

	// Calculate checksum
	UINT32 checksum = 0;
	UCHAR* footerBytes = (UCHAR*)&m_Foot;
	for(int i = 0; i < sizeof(VHD_FOOTER); i++)
	{
		if(i < 64 || i >= 68) // Skip checksum field
			checksum += footerBytes[i];
	}
	m_Foot.checksum = _byteswap_ulong(~checksum);

	DWORD bytesWritten;
	return WriteFile(m_hVhdFile, &m_Foot, sizeof(VHD_FOOTER), &bytesWritten, NULL) &&
		   bytesWritten == sizeof(VHD_FOOTER);
}

BOOL CDiskToVhd::WriteDynHeader()
{
	if(!m_hVhdFile)
		return FALSE;

	// Calculate checksum
	UINT32 checksum = 0;
	UCHAR* headerBytes = (UCHAR*)&m_Dyn;
	for(int i = 0; i < sizeof(VHD_DYNAMIC); i++)
	{
		if(i < 36 || i >= 40) // Skip checksum field
			checksum += headerBytes[i];
	}
	m_Dyn.checksum = _byteswap_ulong(~checksum);

	DWORD bytesWritten;
	return WriteFile(m_hVhdFile, &m_Dyn, sizeof(VHD_DYNAMIC), &bytesWritten, NULL) &&
		   bytesWritten == sizeof(VHD_DYNAMIC);
}

BOOL CDiskToVhd::WriteBlockAllocationTable()
{
	if(!m_hVhdFile)
		return FALSE;

	UINT32 maxEntries = _byteswap_ulong(m_Dyn.maxTableEntries);
	UINT32* bat = new UINT32[maxEntries];
	if(!bat)
		return FALSE;

	// Initialize all entries to 0xFFFFFFFF (unused)
	for(UINT32 i = 0; i < maxEntries; i++)
		bat[i] = 0xFFFFFFFF;

	DWORD bytesWritten;
	BOOL result = WriteFile(m_hVhdFile, bat, maxEntries * sizeof(UINT32), &bytesWritten, NULL) &&
				  bytesWritten == maxEntries * sizeof(UINT32);

	delete[] bat;
	return result;
}

BOOL CDiskToVhd::DumpDiskToVhd(const LPWSTR sDrive, const LPWSTR sVhdPath, const HWND hDlg)
{
	if(!OpenPhysicalDrive((LPWSTR)sDrive))
		return FALSE;

	UINT64 diskSize = GetDiskSize();
	if(diskSize == 0)
	{
		ClosePhysicalDrive();
		return FALSE;
	}

	if(!CreateVhdFile((LPWSTR)sVhdPath))
	{
		ClosePhysicalDrive();
		return FALSE;
	}

	if(!InitializeVhdStructures(diskSize))
	{
		CloseVhdFile();
		ClosePhysicalDrive();
		return FALSE;
	}

	// Write VHD footer first
	if(!WriteFooter())
	{
		CloseVhdFile();
		ClosePhysicalDrive();
		return FALSE;
	}

	// Write dynamic header
	if(!WriteDynHeader())
	{
		CloseVhdFile();
		ClosePhysicalDrive();
		return FALSE;
	}

	// Write block allocation table
	if(!WriteBlockAllocationTable())
	{
		CloseVhdFile();
		ClosePhysicalDrive();
		return FALSE;
	}

	// Read disk data and write to VHD
	BOOL result = DumpDiskToVhdData(hDlg);

	CloseVhdFile();
	ClosePhysicalDrive();

	return result;
}

BOOL CDiskToVhd::DumpDiskToVhdData(HWND hDlg)
{
	// This is a simplified implementation
	// In a full implementation, this would read the physical disk
	// and write blocks to the VHD format with proper bitmap management
	
	SendMessage(hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"Creating VHD file structure...", 0);
	
	// For now, just create the basic VHD structure
	// A complete implementation would need to:
	// 1. Read disk blocks
	// 2. Check for empty/zero blocks
	// 3. Write non-empty blocks to VHD with bitmaps
	// 4. Update the Block Allocation Table
	// 5. Write final footer at end of file
	
	// Write final footer at end of file
	LARGE_INTEGER filePos;
	filePos.QuadPart = 0;
	SetFilePointerEx(m_hVhdFile, filePos, NULL, FILE_END);
	
	return WriteFooter();
}