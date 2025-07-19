#pragma once

#include "resource.h"

// Partition information structure for GUI display
typedef struct _PARTITION_INFO
{
	UINT32 startLBA;
	UINT32 sizeSectors;
	BYTE partitionType;
	WCHAR label[64];
	WCHAR filesystem[32];
	WCHAR size[32];
	BOOL isBootable;
}PARTITION_INFO;

// Global partition information for drawing
extern PARTITION_INFO g_partitions[16];
extern int g_partitionCount;
extern UINT64 g_totalDiskSize;
