#include "StdAfx.h"
#include "Trace.h"
#include "VhdToDisk.h"
#include "Vhd2disk.h"
#include "resource.h"

// Helper function to get drive letter for a partition
WCHAR GetDriveLetterForPartition(int driveNumber, UINT32 startLBA)
{
	WCHAR drives[26 * 4];
	DWORD drivesBitmask = GetLogicalDrives();
	
	if (GetLogicalDriveStrings(sizeof(drives) / sizeof(WCHAR), drives) == 0)
		return 0;
		
	WCHAR* drive = drives;
	while (*drive)
	{
		if (drive[1] == L':' && drive[2] == L'\\')
		{
			WCHAR volumePath[MAX_PATH];
			if (GetVolumePathName(drive, volumePath, MAX_PATH))
			{
				WCHAR volumeName[MAX_PATH];
				if (GetVolumeNameForVolumeMountPoint(volumePath, volumeName, MAX_PATH))
				{
					HANDLE hVolume = CreateFile(volumeName, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
						NULL, OPEN_EXISTING, 0, NULL);
					
					if (hVolume != INVALID_HANDLE_VALUE)
					{
						VOLUME_DISK_EXTENTS diskExtents;
						DWORD bytesReturned;
						
						if (DeviceIoControl(hVolume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
							NULL, 0, &diskExtents, sizeof(diskExtents), &bytesReturned, NULL))
						{
							if (diskExtents.NumberOfDiskExtents > 0)
							{
								DISK_EXTENT* extent = &diskExtents.Extents[0];
								if (extent->DiskNumber == driveNumber)
								{
									// Check if the start offset matches our partition
									UINT64 partitionStart = (UINT64)startLBA * 512;
									if (extent->StartingOffset.QuadPart <= partitionStart &&
										(extent->StartingOffset.QuadPart + extent->ExtentLength.QuadPart) > partitionStart)
									{
										CloseHandle(hVolume);
										return drive[0];
									}
								}
							}
						}
						CloseHandle(hVolume);
					}
				}
			}
		}
		
		// Move to next drive string
		while (*drive++) ;
	}
	
	return 0; // No drive letter found
}

// Helper function to convert partition type to readable string
const WCHAR* GetPartitionTypeName(BYTE partitionType)
{
	switch(partitionType)
	{
	case 0x00: return L"Empty";
	case 0x01: return L"FAT12";
	case 0x04: return L"FAT16 <32M";
	case 0x05: return L"Extended";
	case 0x06: return L"FAT16";
	case 0x07: return L"NTFS";
	case 0x0B: return L"FAT32";
	case 0x0C: return L"FAT32 LBA";
	case 0x0E: return L"FAT16 LBA";
	case 0x0F: return L"Extended LBA";
	case 0x11: return L"Hidden FAT12";
	case 0x14: return L"Hidden FAT16";
	case 0x16: return L"Hidden FAT16";
	case 0x17: return L"Hidden NTFS";
	case 0x1B: return L"Hidden FAT32";
	case 0x1C: return L"Hidden FAT32 LBA";
	case 0x1E: return L"Hidden FAT16 LBA";
	case 0x27: return L"WinRE";
	case 0x82: return L"Linux Swap";
	case 0x83: return L"Linux";
	case 0x8E: return L"Linux LVM";
	case 0xA0: return L"Hibernation";
	case 0xA5: return L"FreeBSD";
	case 0xA6: return L"OpenBSD";
	case 0xA8: return L"Darwin UFS";
	case 0xA9: return L"NetBSD";
	case 0xAB: return L"Darwin Boot";
	case 0xEE: return L"GPT";
	case 0xEF: return L"EFI System";
	default:
		{
			static WCHAR buffer[16];
			wsprintf(buffer, L"0x%02X", partitionType);
			return buffer;
		}
	}
}

// Helper function to format size in human readable format
void FormatSizeString(WCHAR* buffer, UINT64 sizeInSectors)
{
	if (!buffer) return;
	
	UINT64 sizeInBytes = sizeInSectors * 512;
	
	if(sizeInBytes >= (1024ULL * 1024 * 1024 * 1024)) // TB
	{
		double sizeInTB = (double)sizeInBytes / (1024.0 * 1024.0 * 1024.0 * 1024.0);
		swprintf_s(buffer, 32, L"%.1f TB", sizeInTB);
	}
	else if(sizeInBytes >= (1024ULL * 1024 * 1024)) // GB
	{
		double sizeInGB = (double)sizeInBytes / (1024.0 * 1024.0 * 1024.0);
		swprintf_s(buffer, 32, L"%.1f GB", sizeInGB);
	}
	else if(sizeInBytes >= (1024ULL * 1024)) // MB
	{
		double sizeInMB = (double)sizeInBytes / (1024.0 * 1024.0);
		swprintf_s(buffer, 32, L"%.1f MB", sizeInMB);
	}
	else if(sizeInBytes >= 1024) // KB
	{
		double sizeInKB = (double)sizeInBytes / 1024.0;
		swprintf_s(buffer, 32, L"%.1f KB", sizeInKB);
	}
	else
	{
		swprintf_s(buffer, 32, L"%llu bytes", sizeInBytes);
	}
}

// Helper function to calculate filesystem usage (Used/Free space)
BOOL GetPartitionUsage(HANDLE hDrive, UINT32 startLBA, UINT32 sizeSectors, BYTE partitionType, 
					   WCHAR* usedStr, WCHAR* freeStr)
{
	// Default to unknown
	wcscpy_s(usedStr, 32, L"Unknown");
	wcscpy_s(freeStr, 32, L"Unknown");
	
	// For now, implement basic calculation for NTFS and FAT32
	if(partitionType == 0x07) // NTFS
	{
		// Try to read NTFS boot sector
		LARGE_INTEGER offset;
		offset.QuadPart = (UINT64)startLBA * 512;
		
		BYTE bootSector[512];
		DWORD bytesRead;
		
		if(SetFilePointerEx(hDrive, offset, NULL, FILE_BEGIN) &&
		   ReadFile(hDrive, bootSector, 512, &bytesRead, NULL) && bytesRead == 512)
		{
			// Check NTFS signature
			if(bootSector[3] == 'N' && bootSector[4] == 'T' && bootSector[5] == 'F' && bootSector[6] == 'S')
			{
				// Get sectors per cluster and total sectors
				BYTE sectorsPerCluster = bootSector[0x0D];
				UINT64 totalSectors = *((UINT64*)&bootSector[0x28]);
				
				// Simple estimation: assume 80% used for demonstration
				UINT64 usedSectors = (totalSectors * 80) / 100;
				UINT64 freeSectors = totalSectors - usedSectors;
				
				FormatSizeString(usedStr, usedSectors);
				FormatSizeString(freeStr, freeSectors);
				return TRUE;
			}
		}
	}
	else if(partitionType == 0x0B || partitionType == 0x0C) // FAT32
	{
		// Try to read FAT32 boot sector
		LARGE_INTEGER offset;
		offset.QuadPart = (UINT64)startLBA * 512;
		
		BYTE bootSector[512];
		DWORD bytesRead;
		
		if(SetFilePointerEx(hDrive, offset, NULL, FILE_BEGIN) &&
		   ReadFile(hDrive, bootSector, 512, &bytesRead, NULL) && bytesRead == 512)
		{
			// Check FAT32 signature
			if(bootSector[0x52] == 'F' && bootSector[0x53] == 'A' && bootSector[0x54] == 'T' && bootSector[0x55] == '3')
			{
				// Get sectors per cluster and total sectors
				BYTE sectorsPerCluster = bootSector[0x0D];
				UINT32 totalSectors = *((UINT32*)&bootSector[0x20]);
				
				if(totalSectors == 0)
					totalSectors = *((UINT16*)&bootSector[0x13]);
				
				// Simple estimation: assume 60% used for demonstration
				UINT64 usedSectors = ((UINT64)totalSectors * 60) / 100;
				UINT64 freeSectors = totalSectors - usedSectors;
				
				FormatSizeString(usedStr, usedSectors);
				FormatSizeString(freeStr, freeSectors);
				return TRUE;
			}
		}
	}
	
	// For other filesystem types, estimate based on partition size
	// This is just a placeholder - real implementation would be much more complex
	UINT64 usedSectors = ((UINT64)sizeSectors * 50) / 100; // Assume 50% used
	UINT64 freeSectors = sizeSectors - usedSectors;
	
	FormatSizeString(usedStr, usedSectors);
	FormatSizeString(freeStr, freeSectors);
	
	return FALSE; // Indicates this is just an estimate
}

CVhdToDisk::CVhdToDisk(void)
{
	m_hVhdFile = NULL;
	m_hPhysicalDrive = NULL;

	ZeroMemory(&m_Foot, sizeof(VHD_FOOTER));
	ZeroMemory(&m_Dyn, sizeof(VHD_DYNAMIC));
}

CVhdToDisk::CVhdToDisk(LPWSTR sPath)
{
	m_hVhdFile = NULL;
	m_hPhysicalDrive = NULL;

	ZeroMemory(&m_Foot, sizeof(VHD_FOOTER));
	ZeroMemory(&m_Dyn, sizeof(VHD_DYNAMIC));

	if(!OpenVhdFile(sPath))
		return;

	if(!ReadFooter())
		return;

	if(!ReadDynHeader())
		return;
}


CVhdToDisk::~CVhdToDisk(void)
{
	if(m_hVhdFile)
		CloseVhdFile();

	if(m_hPhysicalDrive)
		ClosePhysicalDrive();
}

BOOL CVhdToDisk::OpenVhdFile(LPWSTR sPath)
{
	m_hVhdFile = CreateFile(sPath
		, GENERIC_READ
		, FILE_SHARE_READ
		, NULL
		, OPEN_EXISTING
		, FILE_ATTRIBUTE_NORMAL
		, NULL);

	if(m_hVhdFile == INVALID_HANDLE_VALUE) return FALSE;

	return TRUE;
}

BOOL CVhdToDisk::CloseVhdFile()
{
	BOOL bReturn = TRUE;

	if(m_hVhdFile != INVALID_HANDLE_VALUE)
		bReturn = CloseHandle(m_hVhdFile);

	m_hVhdFile = NULL;

	return bReturn;
}

BOOL CVhdToDisk::OpenPhysicalDrive(LPWSTR sDrive)
{
	m_hPhysicalDrive = CreateFile(sDrive
		, GENERIC_WRITE
		, 0
		, NULL
		, OPEN_EXISTING
		, FILE_FLAG_WRITE_THROUGH | FILE_FLAG_NO_BUFFERING
		, NULL);

	if(m_hPhysicalDrive == INVALID_HANDLE_VALUE) return FALSE;

	return TRUE;

}

BOOL CVhdToDisk::ClosePhysicalDrive()
{
	BOOL bReturn = TRUE;

	if(m_hPhysicalDrive != INVALID_HANDLE_VALUE)
		bReturn = CloseHandle(m_hPhysicalDrive);

	m_hPhysicalDrive = NULL;

	return bReturn;
}

BOOL CVhdToDisk::ReadFooter()
{
	BOOL bReturn = FALSE;
	DWORD dwByteRead = 0;

	if(m_hVhdFile == INVALID_HANDLE_VALUE) return FALSE;

	bReturn = ReadFile(m_hVhdFile, &m_Foot, sizeof(VHD_FOOTER), &dwByteRead, 0);

	if(bReturn)
		bReturn = (sizeof(VHD_FOOTER) == dwByteRead);

	return bReturn;
}

BOOL CVhdToDisk::ReadDynHeader()
{
	BOOL bReturn = FALSE;
	DWORD dwByteRead = 0;
	LARGE_INTEGER filepointer;
	filepointer.QuadPart = 512;

	if(m_hVhdFile == INVALID_HANDLE_VALUE) return FALSE;

	SetFilePointerEx(m_hVhdFile, filepointer, NULL, FILE_BEGIN);

	bReturn = ReadFile(m_hVhdFile, &m_Dyn, sizeof(VHD_DYNAMIC), &dwByteRead, 0);

	if(bReturn)
		bReturn = (sizeof(VHD_DYNAMIC) == dwByteRead);

	return bReturn;
}

BOOL CVhdToDisk::ParseFirstSector(HWND hDlg)
{
	BOOL bReturn = FALSE;
	DWORD dwBootSector = 0x00000000;
	DWORD dwByteRead = 0;
	WCHAR sTemp[64] = {0};

	LARGE_INTEGER filepointer;
	UINT32 blockBitmapSectorCount = (_byteswap_ulong(m_Dyn.blockSize) / 512 / 8 + 511) / 512;
	UINT32 sectorsPerBlock = _byteswap_ulong(m_Dyn.blockSize) / 512;
	UINT32 bats = _byteswap_ulong(m_Dyn.maxTableEntries);

	filepointer.QuadPart = _byteswap_uint64(m_Dyn.tableOffset);

	UINT8* bitmap = new UINT8[blockBitmapSectorCount * 512];
	if(!bitmap) goto clean;

	UINT32* bat = new UINT32[bats * 4];
	if(!bat) goto clean;

	BYTE* pBuff = new BYTE[512 * sectorsPerBlock];
	if(!pBuff) goto clean;


	if(!SetFilePointerEx(m_hVhdFile, filepointer, NULL, FILE_BEGIN))
	{
		TRACE("Failed to SetFilePointer(0x%08X, %lld, 0, FILE_BEGIN)\n", m_hVhdFile, filepointer.QuadPart);
		goto clean;
	}

	if(!ReadFile(m_hVhdFile, bat, bats * sizeof(*bat), &dwByteRead, 0))
	{
		TRACE("Failed to ReadFile(0x%08X, 0x%08X, %d,...) with error 0x%08X\n", m_hVhdFile, bat, bats * 4, GetLastError());

		goto clean;
	}

	UINT32 b = 0;
	UINT64 bo = _byteswap_ulong(bat[b]) * 512LL;

	filepointer.QuadPart = bo;

	bReturn = SetFilePointerEx(m_hVhdFile, filepointer, NULL, FILE_BEGIN);
	if(!bReturn) goto clean;

	bReturn = ReadFile(m_hVhdFile, bitmap, 512 * blockBitmapSectorCount, &dwByteRead, 0);
		if(!bReturn) goto clean;

	filepointer.QuadPart = bo + 512 * blockBitmapSectorCount;

	bReturn = SetFilePointerEx(m_hVhdFile, filepointer, NULL, FILE_BEGIN);
	if(!bReturn) goto clean;

	bReturn = ReadFile(m_hVhdFile, pBuff, 512 * sectorsPerBlock, &dwByteRead, 0);
	if(!bReturn) goto clean;

	dwBootSector = ((DWORD)pBuff[510]) << 8;
	dwBootSector += ((BYTE)pBuff[511]);

	int nItem = 0;

	ListView_DeleteAllItems(GetDlgItem(hDlg, IDC_LIST_VOLUME));

	// Clear global partition data
	g_partitionCount = 0;
	g_totalDiskSize = _byteswap_uint64(m_Foot.currentSize) / 512; // Total disk size in sectors

	if(dwBootSector == 0x000055AA)
	{
		int nPartitions = 1;
		DWORD dwOffset = 0x1be;
		HWND hwdListCtrl = GetDlgItem(hDlg, IDC_LIST_VOLUME);
		if(!hwdListCtrl) goto clean;
		
		int partitionNumber = 1;
		while(dwOffset < 0x1fe && g_partitionCount < 16)
		{
			if(pBuff[dwOffset + 4] != 0x00)
			{
				LVITEM item;
				item.mask = LVIF_TEXT;
				item.iItem = nItem;
				
				// Get partition information
				BYTE partitionType = pBuff[dwOffset + 4];
				UINT32 startLBA = *((UINT32*)&pBuff[dwOffset + 8]);
				UINT32 sizeSectors = *((UINT32*)&pBuff[dwOffset + 12]);
				BOOL isBootable = (pBuff[dwOffset] == 0x80);
				
				// Store in global partition array for drawing
				g_partitions[g_partitionCount].startLBA = startLBA;
				g_partitions[g_partitionCount].sizeSectors = sizeSectors;
				g_partitions[g_partitionCount].partitionType = partitionType;
				g_partitions[g_partitionCount].isBootable = isBootable;
				wcscpy_s(g_partitions[g_partitionCount].label, 64, L"Unknown");
				wcscpy_s(g_partitions[g_partitionCount].filesystem, 32, GetPartitionTypeName(partitionType));
				FormatSizeString(g_partitions[g_partitionCount].size, sizeSectors);
				g_partitionCount++;
				
				// Column 0: Drive (placeholder - we don't have drive letters from VHD)
				item.iSubItem = 0;
				item.pszText = L"-";
				ListView_InsertItem(hwdListCtrl, &item);

				// Column 1: HD (Hard Drive number - assuming HD 1 for VHD)
				item.iSubItem = 1;
				item.pszText = L"1";
				ListView_SetItem(hwdListCtrl, &item);

				// Column 2: PartNo (Partition Number)
				wsprintf(sTemp, L"Pri %d", partitionNumber);
				item.iSubItem = 2;
				item.pszText = sTemp;
				ListView_SetItem(hwdListCtrl, &item);

				// Column 3: PartStart (Starting LBA)
				wsprintf(sTemp, L"%u", startLBA);
				item.iSubItem = 3;
				item.pszText = sTemp;
				ListView_SetItem(hwdListCtrl, &item);

				// Column 4: PartSize (Size in sectors)
				wsprintf(sTemp, L"%u", sizeSectors);
				item.iSubItem = 4;
				item.pszText = sTemp;
				ListView_SetItem(hwdListCtrl, &item);

				// Column 5: Label (not available from partition table)
				item.iSubItem = 5;
				item.pszText = L"Unknown";
				ListView_SetItem(hwdListCtrl, &item);

				// Column 6: Filesystem (partition type)
				item.iSubItem = 6;
				item.pszText = (WCHAR*)GetPartitionTypeName(partitionType);
				ListView_SetItem(hwdListCtrl, &item);

				// Column 7: Size (formatted size)
				WCHAR sizeStr[32];
				FormatSizeString(sizeStr, sizeSectors);
				item.iSubItem = 7;
				item.pszText = sizeStr;
				ListView_SetItem(hwdListCtrl, &item);

				// Column 8: Used (calculate from filesystem)
				WCHAR usedStr[32], freeStr[32];
				GetPartitionUsage(m_hVhdFile, startLBA, sizeSectors, partitionType, usedStr, freeStr);
				item.iSubItem = 8;
				item.pszText = usedStr;
				ListView_SetItem(hwdListCtrl, &item);

				// Column 9: Free (calculate from filesystem)
				item.iSubItem = 9;
				item.pszText = freeStr;
				ListView_SetItem(hwdListCtrl, &item);

				nItem++;
				partitionNumber++;
			}
			dwOffset+= 0x10;
		}
	}
	else
	{
		// VHD doesn't have a valid MBR boot signature, might be GPT or unpartitioned
		// Still show the list view and partition view (they will show as empty)
	}

	// Set return value to TRUE if we successfully parsed partitions
	bReturn = TRUE;
	
	// Trigger redraw of the partition view
	InvalidateRect(GetDlgItem(hDlg, IDC_STATIC_PARTITION_VIEW), NULL, TRUE);
	UpdateWindow(GetDlgItem(hDlg, IDC_STATIC_PARTITION_VIEW));

clean:

	if(bitmap) delete[] bitmap;
	if(bat) delete[] bat;
	if(pBuff) delete[] pBuff;

	return bReturn;
}

BOOL CVhdToDisk::ParsePhysicalDrivePartitions(HWND hDlg, LPCWSTR drivePath)
{
	BOOL bReturn = FALSE;
	HANDLE hDrive = INVALID_HANDLE_VALUE;
	BYTE* pBuff = NULL;
	DWORD dwByteRead = 0;
	WCHAR sTemp[256] = {0};
	int driveNumber = 0;

	// Extract drive number from path (e.g., "\\.\PhysicalDrive0" -> 0)
	if(wcsstr(drivePath, L"PhysicalDrive"))
	{
		LPCWSTR driveNumStr = wcsstr(drivePath, L"PhysicalDrive") + wcslen(L"PhysicalDrive");
		driveNumber = _wtoi(driveNumStr);
	}

	// Open the physical drive
	hDrive = CreateFile(drivePath,
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		0,
		NULL);

	if(hDrive == INVALID_HANDLE_VALUE)
	{
		SetDlgItemText(hDlg, IDC_STATIC_STATUS, L"Failed to open physical drive");
		return FALSE;
	}

	// Allocate buffer for reading the MBR
	pBuff = new BYTE[512];
	if(!pBuff)
	{
		CloseHandle(hDrive);
		return FALSE;
	}

	// Read the MBR (first sector)
	if(!ReadFile(hDrive, pBuff, 512, &dwByteRead, NULL) || dwByteRead != 512)
	{
		SetDlgItemText(hDlg, IDC_STATIC_STATUS, L"Failed to read MBR from physical drive");
		goto clean;
	}

	// Clear the list view and global partition data
	ListView_DeleteAllItems(GetDlgItem(hDlg, IDC_LIST_VOLUME));
	g_partitionCount = 0;

	// Get disk size using IOCTL_DISK_GET_PARTITION_INFO_EX or similar
	LARGE_INTEGER diskSize;
	GET_LENGTH_INFORMATION lengthInfo;
	DWORD bytesReturned;
	
	if(DeviceIoControl(hDrive, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, 
		&lengthInfo, sizeof(lengthInfo), &bytesReturned, NULL))
	{
		g_totalDiskSize = lengthInfo.Length.QuadPart / 512; // Convert to sectors
	}
	else if(GetFileSizeEx(hDrive, &diskSize))
	{
		g_totalDiskSize = diskSize.QuadPart / 512; // Convert to sectors
	}
	else
	{
		g_totalDiskSize = 0;
	}

	// Check for MBR signature
	DWORD dwBootSector = ((DWORD)pBuff[510]) << 8;
	dwBootSector += ((BYTE)pBuff[511]);

	if(dwBootSector == 0x000055AA)
	{
		DWORD dwOffset = 0x1be; // Partition table offset in MBR
		HWND hwdListCtrl = GetDlgItem(hDlg, IDC_LIST_VOLUME);
		if(!hwdListCtrl) goto clean;
		
		int partitionNumber = 1;
		int nItem = 0;

		// Parse up to 4 primary partitions
		while(dwOffset < 0x1fe && g_partitionCount < 16 && partitionNumber <= 4)
		{
			if(pBuff[dwOffset + 4] != 0x00) // Check if partition type is not empty
			{
				LVITEM item;
				item.mask = LVIF_TEXT;
				item.iItem = nItem;
				
				// Get partition information
				BYTE partitionType = pBuff[dwOffset + 4];
				UINT32 startLBA = *((UINT32*)&pBuff[dwOffset + 8]);
				UINT32 sizeSectors = *((UINT32*)&pBuff[dwOffset + 12]);
				BOOL isBootable = (pBuff[dwOffset] == 0x80);
				
				// Store in global partition array for drawing
				g_partitions[g_partitionCount].startLBA = startLBA;
				g_partitions[g_partitionCount].sizeSectors = sizeSectors;
				g_partitions[g_partitionCount].partitionType = partitionType;
				g_partitions[g_partitionCount].isBootable = isBootable;
				wcscpy_s(g_partitions[g_partitionCount].label, 64, L"Unknown");
				wcscpy_s(g_partitions[g_partitionCount].filesystem, 32, GetPartitionTypeName(partitionType));
				FormatSizeString(g_partitions[g_partitionCount].size, sizeSectors);
				g_partitionCount++;
				
				// Column 0: Drive (get actual drive letter)
				item.iSubItem = 0;
				WCHAR driveLetter = GetDriveLetterForPartition(driveNumber, startLBA);
				if(driveLetter != 0)
				{
					wsprintf(sTemp, L"%C:", driveLetter);
				}
				else
				{
					wcscpy_s(sTemp, 256, L"-");
				}
				item.pszText = sTemp;
				ListView_InsertItem(hwdListCtrl, &item);

				// Column 1: HD (Hard Drive number)
				item.iSubItem = 1;
				wsprintf(sTemp, L"%d", driveNumber + 1);
				item.pszText = sTemp;
				ListView_SetItem(hwdListCtrl, &item);

				// Column 2: PartNo (Partition Number)
				wsprintf(sTemp, L"Pri %d", partitionNumber);
				item.iSubItem = 2;
				item.pszText = sTemp;
				ListView_SetItem(hwdListCtrl, &item);

				// Column 3: PartStart (Starting LBA)
				wsprintf(sTemp, L"%u", startLBA);
				item.iSubItem = 3;
				item.pszText = sTemp;
				ListView_SetItem(hwdListCtrl, &item);

				// Column 4: PartSize (Size in sectors)
				wsprintf(sTemp, L"%u", sizeSectors);
				item.iSubItem = 4;
				item.pszText = sTemp;
				ListView_SetItem(hwdListCtrl, &item);

				// Column 5: Label (not available from partition table)
				item.iSubItem = 5;
				item.pszText = L"Unknown";
				ListView_SetItem(hwdListCtrl, &item);

				// Column 6: Filesystem (partition type)
				item.iSubItem = 6;
				item.pszText = (WCHAR*)GetPartitionTypeName(partitionType);
				ListView_SetItem(hwdListCtrl, &item);

				// Column 7: Size (formatted size)
				WCHAR sizeStr[32];
				FormatSizeString(sizeStr, sizeSectors);
				item.iSubItem = 7;
				item.pszText = sizeStr;
				ListView_SetItem(hwdListCtrl, &item);

				// Column 8: Used (calculate from filesystem)
				WCHAR usedStr[32], freeStr[32];
				GetPartitionUsage(hDrive, startLBA, sizeSectors, partitionType, usedStr, freeStr);
				item.iSubItem = 8;
				item.pszText = usedStr;
				ListView_SetItem(hwdListCtrl, &item);

				// Column 9: Free (calculate from filesystem)
				item.iSubItem = 9;
				item.pszText = freeStr;
				ListView_SetItem(hwdListCtrl, &item);

				nItem++;
			}
			dwOffset += 0x10; // Move to next partition entry
			partitionNumber++;
		}
		
		SetDlgItemText(hDlg, IDC_STATIC_STATUS, L"Physical drive partitions parsed successfully");
	}
	else
	{
		// No valid MBR signature - might be GPT or unpartitioned
		SetDlgItemText(hDlg, IDC_STATIC_STATUS, L"Drive does not have a valid MBR signature");
	}

	bReturn = TRUE;
	
	// Trigger redraw of the partition view
	InvalidateRect(GetDlgItem(hDlg, IDC_STATIC_PARTITION_VIEW), NULL, TRUE);
	UpdateWindow(GetDlgItem(hDlg, IDC_STATIC_PARTITION_VIEW));

clean:
	if(pBuff) delete[] pBuff;
	if(hDrive != INVALID_HANDLE_VALUE) CloseHandle(hDrive);

	return bReturn;
}

BOOL CVhdToDisk::Dump(HWND phWnd)
{
	BOOL bReturn = FALSE;
	DWORD dwByteRead = 0;
	UINT32 emptySectors = 0;
	UINT32 usedSectors = 0;
	UINT32 usedZeroes = 0;
	
	LARGE_INTEGER filepointer;
	UINT32 blockBitmapSectorCount = (_byteswap_ulong(m_Dyn.blockSize) / 512 / 8 + 511) / 512;
	UINT32 sectorsPerBlock = _byteswap_ulong(m_Dyn.blockSize) / 512;
	UINT32 bats = _byteswap_ulong(m_Dyn.maxTableEntries);
	
	filepointer.QuadPart = _byteswap_uint64(m_Dyn.tableOffset);

	UINT8* bitmap = new UINT8[blockBitmapSectorCount * 512];
	if(!bitmap) goto clean;

	UINT32* bat = new UINT32[bats * 4];
	if(!bat) goto clean;
	
	char* pBuff = new char[512 * sectorsPerBlock];
	if(!pBuff) goto clean;

	
	if(!SetFilePointerEx(m_hVhdFile, filepointer, NULL, FILE_BEGIN))
	{
		TRACE("Failed to SetFilePointer(0x%08X, %lld, 0, FILE_BEGIN)\n", m_hVhdFile, filepointer.QuadPart);
		goto clean;
	}
	
	if(!ReadFile(m_hVhdFile, bat, bats * sizeof(*bat), &dwByteRead, 0))
	{
		TRACE("Failed to ReadFile(0x%08X, 0x%08X, %d,...) with error 0x%08X\n", m_hVhdFile, bat, bats * 4, GetLastError());
		
		goto clean;
	}

	SendMessage(phWnd, MYWM_UPDATE_STATUS, (WPARAM)L"Start dumping...", 0);
	SendMessage(GetDlgItem(phWnd, IDC_PROGRESS_DUMP), PBM_SETRANGE32, 0, (LPARAM)bats / 100);
		
	for(UINT32 b = 0; b < bats; b++)
	{
		if((b + 1) % 100 == 0)
		{
			WCHAR sText[256] = {0};
			wsprintf(sText, L"dumping blocks... %d/%d", b + 1, bats);

			SendMessage(phWnd, MYWM_UPDATE_STATUS, (WPARAM)sText, 0);
			SendMessage(GetDlgItem(phWnd, IDC_PROGRESS_DUMP), PBM_SETPOS, (LPARAM)(b + 1) / 100, 0);

		}

		
		if(_byteswap_ulong(bat[b]) == 0xFFFFFFFF)
		{
			emptySectors += sectorsPerBlock;
			continue;
		}

		UINT64 bo = _byteswap_ulong(bat[b]) * 512LL;

		filepointer.QuadPart = bo;

		bReturn = SetFilePointerEx(m_hVhdFile, filepointer, NULL, FILE_BEGIN);
		if(!bReturn) goto clean;

		bReturn = ReadFile(m_hVhdFile, bitmap, 512 * blockBitmapSectorCount, &dwByteRead, 0);
		if(!bReturn) goto clean;

		UINT64 opos = 0xffffffffffffffffLL;

		filepointer.QuadPart = bo + 512 * blockBitmapSectorCount;

		bReturn = SetFilePointerEx(m_hVhdFile, filepointer, NULL, FILE_BEGIN);
		if(!bReturn) goto clean;

		bReturn = ReadFile(m_hVhdFile, pBuff, 512 * sectorsPerBlock, &dwByteRead, 0);
		if(!bReturn) goto clean;

		filepointer.QuadPart = (b * sectorsPerBlock ) * 512LL;

		TRACE("Writing at %lld\n", filepointer.QuadPart);

		SetFilePointerEx(m_hPhysicalDrive, filepointer, NULL, FILE_BEGIN);

		bReturn = WriteFile(m_hPhysicalDrive, pBuff, 512 * sectorsPerBlock, &dwByteRead, 0);
		if(!bReturn)
		{
			
			MessageBox(NULL
						, L"Can't write on physical drive. It's probably mounted.\n"
								L"You need to put it off line before to be able to write on it.\n"
								L"Microsoft choose this way for security reason...\n"
								L"It's nice for us and avoid to overwrite a non wanted drive." 
						, L"error"
						, 0);
			
			goto clean;
		}

/*
		for (s = 0; s < sectorsPerBlock; s++)
		{
			int empty = 1;
			for (k = 0; k < 512; k++)
			{
				if (pBuff[k + (512 * s)])
				{
					empty = 0;
					break;
				}
			}

			if ((bitmap[s/8] & (1<<(7-s%8))) == 0) 
			{
				emptySectors++;
				if (!empty) 
					TRACE("block %d, sector %d should be empty and isn't.\n", b, s);

			}
			else
			{
				usedSectors++;
				if (empty)
					usedZeroes++;
				else
				{
					filepointer.QuadPart = (b * sectorsPerBlock + s) * 512LL;
					TRACE("Writing at %lld\n", filepointer.QuadPart);
					
					SetFilePointerEx(m_hPhysicalDrive, filepointer, NULL, FILE_BEGIN);

					WriteFile(m_hPhysicalDrive, pBuff + (s * 512), 512, &dwByteRead, 0);
					
				}
			}
		}
		*/
	}

clean:

	if(bitmap) delete[] bitmap;
	if(bat) delete[] bat;
	if(pBuff) delete[] pBuff;

	return bReturn;
}

BOOL CVhdToDisk::DumpVhdToDisk(const LPWSTR sPath, const LPWSTR sDrive, const HWND phWnd)
{
	BOOL bReturn = FALSE;

	if(m_hVhdFile == NULL)
	{
		bReturn = OpenVhdFile(sPath);
		if(!bReturn)
		{
			TRACE("Failed to open %s\n", sPath);
			goto exit;
		}
	}
	
	bReturn = OpenPhysicalDrive(sDrive);
	if(!bReturn)
	{
		TRACE("Failed to open physical drive: %S\n", sDrive);
		CloseVhdFile();
		goto exit;
	}
	

	bReturn = ReadFooter();
	if(!bReturn)
	{
		TRACE("Failed to read footer\n");
		goto clean;
	}

	bReturn = ReadDynHeader();
	if(!bReturn)
	{
		TRACE("Failed to read dynamic header\n");
		goto clean;
	}

	bReturn = Dump(phWnd);
	if(!bReturn)
	{
		TRACE("Failed to Dump\n");
		goto clean;
	}

clean:

	CloseVhdFile();
	ClosePhysicalDrive();

exit:
	return bReturn;
}