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
		DWORD error = GetLastError();
		m_hPhysicalDrive = NULL;
		
		// Try with backup privileges for better access
		m_hPhysicalDrive = CreateFile(sDrive
			, GENERIC_READ
			, FILE_SHARE_READ | FILE_SHARE_WRITE
			, NULL
			, OPEN_EXISTING
			, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN
			, NULL);
			
		if(m_hPhysicalDrive == INVALID_HANDLE_VALUE)
		{
			m_hPhysicalDrive = NULL;
			return FALSE;
		}
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
	
	// Set disk geometry (handle large disks properly)
	UINT64 totalSectors = diskSize / 512;
	UINT32 sectorsPerTrack = 63;
	UINT32 heads = 16;
	UINT32 cylindersNeeded = (UINT32)(totalSectors / (heads * sectorsPerTrack));
	
	// VHD format uses 16-bit cylinders, so cap at maximum value
	if(cylindersNeeded > 65535)
	{
		// For very large disks, use maximum CHS values
		m_Foot.diskGeometry.cylinders = _byteswap_ushort(65535);
		m_Foot.diskGeometry.heads = 16;
		m_Foot.diskGeometry.sectors = 63;
	}
	else
	{
		m_Foot.diskGeometry.cylinders = _byteswap_ushort((UINT16)cylindersNeeded);
		m_Foot.diskGeometry.heads = (UCHAR)heads;
		m_Foot.diskGeometry.sectors = (UCHAR)sectorsPerTrack;
	}
	
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
	{
		SendMessage(hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"Failed to open physical drive. Administrator privileges may be required.", 1);
		return FALSE;
	}

	UINT64 diskSize = GetDiskSize();
	if(diskSize == 0)
	{
		ClosePhysicalDrive();
		SendMessage(hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"Failed to determine disk size.", 1);
		return FALSE;
	}

	if(!CreateVhdFile((LPWSTR)sVhdPath))
	{
		ClosePhysicalDrive();
		SendMessage(hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"Failed to create VHD file. Check path and permissions.", 1);
		return FALSE;
	}

	if(!InitializeVhdStructures(diskSize))
	{
		CloseVhdFile();
		ClosePhysicalDrive();
		SendMessage(hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"Failed to initialize VHD structures.", 1);
		return FALSE;
	}

	// Write VHD footer first
	if(!WriteFooter())
	{
		CloseVhdFile();
		ClosePhysicalDrive();
		SendMessage(hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"Failed to write VHD footer.", 1);
		return FALSE;
	}

	// Write dynamic header
	if(!WriteDynHeader())
	{
		CloseVhdFile();
		ClosePhysicalDrive();
		SendMessage(hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"Failed to write VHD dynamic header.", 1);
		return FALSE;
	}

	// Write block allocation table
	if(!WriteBlockAllocationTable())
	{
		CloseVhdFile();
		ClosePhysicalDrive();
		SendMessage(hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"Failed to write VHD block allocation table.", 1);
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
	SendMessage(hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"Initializing disk to VHD conversion...", 0);
	
	UINT64 diskSize = GetDiskSize();
	UINT32 blockSize = _byteswap_ulong(m_Dyn.blockSize);
	UINT32 totalBlocks = (UINT32)((diskSize + blockSize - 1) / blockSize);
	UINT32 sectorsPerBlock = blockSize / 512;
	UINT32 bitmapSize = (sectorsPerBlock / 8 + 511) & ~511; // Align to 512 bytes
	
	// Initialize timing for progress estimation
	DWORD startTime = GetTickCount();
	DWORD lastStatusUpdate = startTime;
	UINT64 totalDataProcessed = 0;
	
	// Allocate buffers for reading disk data and block bitmap
	BYTE* diskBuffer = new BYTE[blockSize];
	BYTE* bitmapBuffer = new BYTE[bitmapSize];
	UINT32* bat = new UINT32[totalBlocks];
	
	if(!diskBuffer || !bitmapBuffer || !bat)
	{
		delete[] diskBuffer;
		delete[] bitmapBuffer;
		delete[] bat;
		return FALSE;
	}
	
	// Initialize BAT with all entries as unused
	for(UINT32 i = 0; i < totalBlocks; i++)
		bat[i] = 0xFFFFFFFF;
	
	// Calculate starting offset for data blocks (after headers and BAT)
	UINT64 dataStartOffset = 1536 + ((UINT64)totalBlocks * 4);
	dataStartOffset = (dataStartOffset + 511) & ~511; // Align to 512 bytes
	UINT64 currentDataOffset = dataStartOffset;
	
	LARGE_INTEGER diskPos, vhdPos;
	DWORD bytesRead, bytesWritten;
	
	// Process each block
	for(UINT32 blockIndex = 0; blockIndex < totalBlocks; blockIndex++)
	{
		// Update progress (time-based throttling to reduce flicker)
		DWORD currentTime = GetTickCount();
		if(currentTime - lastStatusUpdate >= 500 || blockIndex == 0 || blockIndex == totalBlocks - 1) // Update every 500ms, first block, or last block
		{
			lastStatusUpdate = currentTime;
			// Calculate progress and timing information
			int progressPercent = (blockIndex * 100) / totalBlocks;
			DWORD elapsedTime = GetTickCount() - startTime;
			
			WCHAR statusMsg[512];
			WCHAR timeRemaining[128] = L"";
			
			// Calculate remaining time if we have meaningful progress
			if(progressPercent > 0 && elapsedTime > 1000) // At least 1 second elapsed
			{
				DWORD estimatedTotalTime = (elapsedTime * 100) / progressPercent;
				DWORD remainingTime = estimatedTotalTime - elapsedTime;
				
				// Convert to hours, minutes, seconds
				DWORD hours = remainingTime / (1000 * 60 * 60);
				DWORD minutes = (remainingTime % (1000 * 60 * 60)) / (1000 * 60);
				DWORD seconds = (remainingTime % (1000 * 60)) / 1000;
				
				if(hours > 0)
					wsprintf(timeRemaining, L", %d:%02d:%02d remaining", hours, minutes, seconds);
				else if(minutes > 0)
					wsprintf(timeRemaining, L", %d:%02d remaining", minutes, seconds);
				else
					wsprintf(timeRemaining, L", %d seconds remaining", seconds);
			}
			
			// Format user-friendly message with data processed
			UINT64 processedMB = totalDataProcessed / (1024 * 1024);
			UINT64 totalMB = diskSize / (1024 * 1024);
			
			if(totalMB > 1024)
			{
				// Show in GB for large drives
				UINT64 processedGB = processedMB / 1024;
				UINT64 totalGB = totalMB / 1024;
				wsprintf(statusMsg, L"Converting disk data... %d%% complete (%I64u GB of %I64u GB processed%s)", 
					progressPercent, processedGB, totalGB, timeRemaining);
			}
			else
			{
				// Show in MB for smaller drives
				wsprintf(statusMsg, L"Converting disk data... %d%% complete (%I64u MB of %I64u MB processed%s)", 
					progressPercent, processedMB, totalMB, timeRemaining);
			}
			
			SendMessage(hDlg, MYWM_UPDATE_STATUS, (WPARAM)statusMsg, 0);
			
			// Update progress bar
			SendMessage(hDlg, MYWM_UPDATE_PROGRESSBAR, progressPercent, 0);
		}
		
		// Calculate disk position for this block
		diskPos.QuadPart = (UINT64)blockIndex * blockSize;
		
		// Read block from disk
		if(!SetFilePointerEx(m_hPhysicalDrive, diskPos, NULL, FILE_BEGIN))
			continue; // Skip this block if seek fails
		
		UINT32 bytesToRead = blockSize;
		if(diskPos.QuadPart + blockSize > diskSize)
			bytesToRead = (UINT32)(diskSize - diskPos.QuadPart);
		
		if(!ReadFile(m_hPhysicalDrive, diskBuffer, bytesToRead, &bytesRead, NULL) || bytesRead == 0)
			continue; // Skip this block if read fails
		
		// Track total data processed for progress reporting
		totalDataProcessed += bytesRead;
		
		// Check if block contains any non-zero data
		BOOL isEmptyBlock = TRUE;
		for(UINT32 i = 0; i < bytesRead && isEmptyBlock; i++)
		{
			if(diskBuffer[i] != 0)
				isEmptyBlock = FALSE;
		}
		
		// Skip empty blocks to save space (sparse VHD)
		if(isEmptyBlock)
			continue;
		
		// Create block bitmap - mark sectors as used
		memset(bitmapBuffer, 0, bitmapSize);
		UINT32 usedSectors = (bytesRead + 511) / 512;
		for(UINT32 sector = 0; sector < usedSectors; sector++)
		{
			UINT32 byteIndex = sector / 8;
			UINT32 bitIndex = 7 - (sector % 8);
			if(byteIndex < bitmapSize)
				bitmapBuffer[byteIndex] |= (1 << bitIndex);
		}
		
		// Write block to VHD file
		vhdPos.QuadPart = currentDataOffset;
		if(SetFilePointerEx(m_hVhdFile, vhdPos, NULL, FILE_BEGIN))
		{
			// Write bitmap first
			if(WriteFile(m_hVhdFile, bitmapBuffer, bitmapSize, &bytesWritten, NULL) && bytesWritten == bitmapSize)
			{
				// Pad partial blocks to sector boundary
				UINT32 paddedSize = (bytesRead + 511) & ~511;
				if(paddedSize > bytesRead)
					memset(diskBuffer + bytesRead, 0, paddedSize - bytesRead);
				
				// Write block data
				if(WriteFile(m_hVhdFile, diskBuffer, paddedSize, &bytesWritten, NULL) && bytesWritten == paddedSize)
				{
					// Update BAT entry (ensure it fits in UINT32 for VHD format)
					UINT64 sectorOffset = currentDataOffset / 512;
					if(sectorOffset > 0xFFFFFFFF)
					{
						// VHD format limitation reached
						delete[] diskBuffer;
						delete[] bitmapBuffer;
						delete[] bat;
						SendMessage(hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"VHD file size limit exceeded (2TB maximum for dynamic VHDs).", 1);
						return FALSE;
					}
					
					bat[blockIndex] = _byteswap_ulong((UINT32)sectorOffset);
					
					// Advance data offset for next block
					currentDataOffset += bitmapSize + paddedSize;
				}
			}
		}
	}
	
	SendMessage(hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"Updating file allocation table...", 0);
	
	// Write updated BAT to VHD file
	vhdPos.QuadPart = 1536; // BAT location
	if(!SetFilePointerEx(m_hVhdFile, vhdPos, NULL, FILE_BEGIN) ||
	   !WriteFile(m_hVhdFile, bat, totalBlocks * sizeof(UINT32), &bytesWritten, NULL) ||
	   bytesWritten != totalBlocks * sizeof(UINT32))
	{
		delete[] diskBuffer;
		delete[] bitmapBuffer;
		delete[] bat;
		return FALSE;
	}
	
	SendMessage(hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"Finalizing VHD file structure...", 0);
	SendMessage(hDlg, MYWM_UPDATE_PROGRESSBAR, 100, 0);
	
	// Write final footer at end of file
	vhdPos.QuadPart = 0;
	SetFilePointerEx(m_hVhdFile, vhdPos, NULL, FILE_END);
	BOOL result = WriteFooter();
	
	// Cleanup
	delete[] diskBuffer;
	delete[] bitmapBuffer;
	delete[] bat;
	
	return result;
}