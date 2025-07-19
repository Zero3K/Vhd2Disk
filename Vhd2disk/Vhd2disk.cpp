// Vhd2disk.cpp�: d�finit le point d'entr�e pour l'application.
//

#include "stdafx.h"
#include "Vhd2disk.h"
#include "VhdToDisk.h"
#include "DiskToVhd.h"
#include "URLCtrl.h"


typedef struct _DUMPTHRDSTRUCT
{
	HWND hDlg;
	WCHAR sVhdPath[MAX_PATH];
	WCHAR sDrive[64];
	BOOL bVhdToDisk; // TRUE for VHD->Disk, FALSE for Disk->VHD
}DUMPTHRDSTRUCT;

// Global partition information for drawing
PARTITION_INFO g_partitions[16];
int g_partitionCount = 0;
UINT64 g_totalDiskSize = 0;

LRESULT CALLBACK MainDlgProc( HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam );

HINSTANCE hInst;
HWND hWnd;
HWND m_hDlg = NULL;
HICON hIcon = NULL;
HANDLE hDumpThread = NULL;
CVhdToDisk* pVhd2disk = NULL;
CDiskToVhd* pDisk2vhd = NULL;
DUMPTHRDSTRUCT dmpstruct;
static WCHAR g_lastStatusText[512] = {0}; // Buffer to prevent redundant status updates


UINT APIENTRY OFNHookProc(HWND hdlg, UINT uiMsg, WPARAM wParam, LPARAM lParam) 
{ 
	if (WM_INITDIALOG==uiMsg) 
	{ 
		HWND hOK=GetDlgItem(GetParent(hdlg), IDOK); 
		SetWindowText(hOK, L"Open VHD"); 
		return 1; 
	} 
	return 0; 
} 

BOOL DoFileDialog(LPWSTR lpszFilename, LPWSTR lpzFilter, LPWSTR lpzExtension) 
{ 
	OPENFILENAME ofn; 

	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn); 
	ofn.lpstrFile = lpszFilename; 
	ofn.nMaxFile = MAX_PATH; 
	ofn.lpstrFilter = lpzFilter;
	ofn.lpstrDefExt = lpzExtension;
	ofn.Flags = OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_ENABLEHOOK|OFN_EXPLORER|OFN_ENABLEHOOK;; 
	ofn.lpfnHook = (LPOFNHOOKPROC)OFNHookProc; 

	return GetOpenFileName(&ofn); 
} 

BOOL DoSaveFileDialog(LPWSTR lpszFilename, LPWSTR lpzFilter, LPWSTR lpzExtension) 
{ 
	OPENFILENAME ofn; 

	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn); 
	ofn.lpstrFile = lpszFilename; 
	ofn.nMaxFile = MAX_PATH; 
	ofn.lpstrFilter = lpzFilter;
	ofn.lpstrDefExt = lpzExtension;
	ofn.Flags = OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_ENABLEHOOK|OFN_EXPLORER; 
	ofn.lpfnHook = (LPOFNHOOKPROC)OFNHookProc; 

	return GetSaveFileName(&ofn); 
} 

// Function to draw the partition visualization
void DrawPartitionView(HWND hWnd, HDC hdc, RECT* pRect)
{
	if(g_partitionCount == 0 || g_totalDiskSize == 0)
	{
		// Draw "No partitions" message
		SetTextColor(hdc, RGB(128, 128, 128));
		SetBkMode(hdc, TRANSPARENT);
		DrawText(hdc, L"No partition information available", -1, pRect, 
			DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		return;
	}

	// Colors for different partition types
	COLORREF colors[] = {
		RGB(0, 102, 204),    // Blue for NTFS
		RGB(255, 153, 0),    // Orange for FAT32
		RGB(0, 153, 76),     // Green for Linux
		RGB(153, 51, 255),   // Purple for Extended
		RGB(255, 51, 51),    // Red for System
		RGB(102, 204, 255),  // Light Blue for Data
		RGB(255, 204, 153),  // Light Orange for Others
		RGB(204, 204, 204)   // Gray for Unknown
	};

	// Clear background
	FillRect(hdc, pRect, (HBRUSH)GetStockObject(WHITE_BRUSH));

	// Draw drive label
	SetTextColor(hdc, RGB(0, 0, 0));
	SetBkMode(hdc, TRANSPARENT);
	RECT labelRect = *pRect;
	labelRect.bottom = labelRect.top + 15;
	DrawText(hdc, L"HD1:", -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	// Adjust drawing area
	RECT drawRect = *pRect;
	drawRect.top += 20;
	drawRect.left += 30;
	drawRect.right -= 10;
	drawRect.bottom -= 5;

	int totalWidth = drawRect.right - drawRect.left;
	int currentX = drawRect.left;

	// Draw each partition
	for(int i = 0; i < g_partitionCount; i++)
	{
		PARTITION_INFO* pPart = &g_partitions[i];
		
		// Calculate partition width based on size
		double ratio = (double)pPart->sizeSectors / (double)g_totalDiskSize;
		int partWidth = (int)(totalWidth * ratio);
		if(partWidth < 10) partWidth = 10; // Minimum width for visibility

		// Select color based on partition type
		COLORREF partColor = colors[i % (sizeof(colors) / sizeof(colors[0]))];
		if(pPart->partitionType == 0x07) partColor = RGB(0, 102, 204);      // NTFS - Blue
		else if(pPart->partitionType == 0x0B || pPart->partitionType == 0x0C) partColor = RGB(255, 153, 0); // FAT32 - Orange
		else if(pPart->partitionType == 0x83) partColor = RGB(0, 153, 76);   // Linux - Green
		else if(pPart->partitionType == 0x05 || pPart->partitionType == 0x0F) partColor = RGB(153, 51, 255); // Extended - Purple

		// Create brush and draw partition rectangle
		HBRUSH hBrush = CreateSolidBrush(partColor);
		HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hBrush);
		
		RECT partRect = {currentX, drawRect.top, currentX + partWidth, drawRect.bottom};
		FillRect(hdc, &partRect, hBrush);
		
		// Draw border
		HPEN hPen = CreatePen(PS_SOLID, 1, RGB(64, 64, 64));
		HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
		Rectangle(hdc, partRect.left, partRect.top, partRect.right, partRect.bottom);
		
		// Draw partition label if there's space
		if(partWidth > 40)
		{
			SetTextColor(hdc, RGB(255, 255, 255));
			SetBkMode(hdc, TRANSPARENT);
			WCHAR labelText[64];
			wsprintf(labelText, L"P%d", i + 1);
			DrawText(hdc, labelText, -1, &partRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		}

		SelectObject(hdc, hOldPen);
		SelectObject(hdc, hOldBrush);
		DeleteObject(hPen);
		DeleteObject(hBrush);

		currentX += partWidth + 2; // Small gap between partitions
	}

	// Draw size information
	WCHAR sizeText[256];
	wsprintf(sizeText, L"Total Size: %.1f GB", (double)(g_totalDiskSize * 512) / (1024.0 * 1024.0 * 1024.0));
	RECT sizeRect = *pRect;
	sizeRect.left = drawRect.right - 150;
	sizeRect.top = pRect->top;
	sizeRect.bottom = pRect->top + 15;
	SetTextColor(hdc, RGB(0, 0, 0));
	DrawText(hdc, sizeText, -1, &sizeRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void PopulatePhysicalDriveComboBox(HWND hDlg)
{
	HANDLE hFile = NULL;
	WCHAR sPhysicalDrive[64] = {0};
	HWND hCombo = GetDlgItem(hDlg, IDC_COMBO1);
	
	SendMessage(hCombo, CB_RESETCONTENT, 0, 0);

	for(int i = 0; i < 99; i++)
	{
		wsprintf(sPhysicalDrive, L"\\\\.\\PhysicalDrive%d", i);
		hFile = CreateFile(sPhysicalDrive
			, GENERIC_READ
			, FILE_SHARE_READ | FILE_SHARE_WRITE
			, NULL
			, OPEN_EXISTING
			, 0
			, NULL);

		if(hFile != INVALID_HANDLE_VALUE)
		{
			CloseHandle(hFile);
			SendMessage(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(sPhysicalDrive));
		}
	}
}

void UpdateUIMode(HWND hDlg, BOOL bVhdToDisk)
{
	// Show/hide controls based on operation mode
	if(bVhdToDisk)
	{
		// VHD to Disk mode
		ShowWindow(GetDlgItem(hDlg, IDC_EDIT_VHD_FILE), SW_SHOW);
		ShowWindow(GetDlgItem(hDlg, IDC_BUTTON_BROWSE_VHD), SW_SHOW);
		ShowWindow(GetDlgItem(hDlg, IDC_STATIC_VHD_LOAD), SW_SHOW);
		
		ShowWindow(GetDlgItem(hDlg, IDC_EDIT_VHD_SAVE_FILE), SW_HIDE);
		ShowWindow(GetDlgItem(hDlg, IDC_BUTTON_BROWSE_VHD_SAVE), SW_HIDE);
		ShowWindow(GetDlgItem(hDlg, IDC_STATIC_VHD_SAVE), SW_HIDE);
	}
	else
	{
		// Disk to VHD mode
		ShowWindow(GetDlgItem(hDlg, IDC_EDIT_VHD_FILE), SW_HIDE);
		ShowWindow(GetDlgItem(hDlg, IDC_BUTTON_BROWSE_VHD), SW_HIDE);
		ShowWindow(GetDlgItem(hDlg, IDC_STATIC_VHD_LOAD), SW_HIDE);
		
		ShowWindow(GetDlgItem(hDlg, IDC_EDIT_VHD_SAVE_FILE), SW_SHOW);
		ShowWindow(GetDlgItem(hDlg, IDC_BUTTON_BROWSE_VHD_SAVE), SW_SHOW);
		ShowWindow(GetDlgItem(hDlg, IDC_STATIC_VHD_SAVE), SW_SHOW);
	}
}

void AddListHeader(HWND hDlg)
{
	LVCOLUMN col;

	HWND hListCtrl = GetDlgItem(hDlg, IDC_LIST_VOLUME);
	if(!hListCtrl) return;

	ListView_SetExtendedListViewStyle 
		(hListCtrl, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_HEADERDRAGDROP);

	col.mask = LVCF_TEXT | LVCF_WIDTH;
	col.pszText = L"Drive";
	col.fmt = LVCFMT_LEFT;
	col.cx = 40;
	ListView_InsertColumn(hListCtrl, 0, &col);

	col.mask = LVCF_TEXT | LVCF_WIDTH;
	col.pszText = L"HD";
	col.fmt = LVCFMT_LEFT;
	col.cx = 30;
	ListView_InsertColumn(hListCtrl, 1, &col);

	col.mask = LVCF_TEXT | LVCF_WIDTH;
	col.pszText = L"PartNo";
	col.fmt = LVCFMT_LEFT;
	col.cx = 50;
	ListView_InsertColumn(hListCtrl, 2, &col);

	col.mask = LVCF_TEXT | LVCF_WIDTH;
	col.pszText = L"PartStart";
	col.fmt = LVCFMT_LEFT;
	col.cx = 70;
	ListView_InsertColumn(hListCtrl, 3, &col);

	col.mask = LVCF_TEXT | LVCF_WIDTH;
	col.pszText = L"PartSize";
	col.fmt = LVCFMT_LEFT;
	col.cx = 70;
	ListView_InsertColumn(hListCtrl, 4, &col);

	col.mask = LVCF_TEXT | LVCF_WIDTH;
	col.pszText = L"Label";
	col.fmt = LVCFMT_LEFT;
	col.cx = 80;
	ListView_InsertColumn(hListCtrl, 5, &col);

	col.mask = LVCF_TEXT | LVCF_WIDTH;
	col.pszText = L"Filesystem";
	col.fmt = LVCFMT_LEFT;
	col.cx = 80;
	ListView_InsertColumn(hListCtrl, 6, &col);

	col.mask = LVCF_TEXT | LVCF_WIDTH;
	col.pszText = L"Size";
	col.fmt = LVCFMT_LEFT;
	col.cx = 70;
	ListView_InsertColumn(hListCtrl, 7, &col);

	col.mask = LVCF_TEXT | LVCF_WIDTH;
	col.pszText = L"Used";
	col.fmt = LVCFMT_LEFT;
	col.cx = 70;
	ListView_InsertColumn(hListCtrl, 8, &col);

	col.mask = LVCF_TEXT | LVCF_WIDTH;
	col.pszText = L"Free";
	col.fmt = LVCFMT_LEFT;
	col.cx = 70;
	ListView_InsertColumn(hListCtrl, 9, &col);
}

int WINAPI WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd )
{
	hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON_V2D));
	DialogBox( hInstance, MAKEINTRESOURCE(IDD_MAIN_DIAG), hWnd, (DLGPROC)MainDlgProc );
	return 0;
}

DWORD WINAPI DumpThread(LPVOID lpVoid)
{
	DUMPTHRDSTRUCT* pDumpStruct = (DUMPTHRDSTRUCT*)lpVoid;
	
	if(pDumpStruct->bVhdToDisk)
	{
		// VHD to Disk conversion
		if(pVhd2disk->DumpVhdToDisk(pDumpStruct->sVhdPath, pDumpStruct->sDrive, pDumpStruct->hDlg))
			SendMessage(pDumpStruct->hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"VHD dumped to drive successfully!", 1);
		else
			SendMessage(pDumpStruct->hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"Failed to dump the VHD to drive!", 1);
	}
	else
	{
		// Disk to VHD conversion
		if(!pDisk2vhd)
			pDisk2vhd = new CDiskToVhd();
			
		if(pDisk2vhd && pDisk2vhd->DumpDiskToVhd(pDumpStruct->sDrive, pDumpStruct->sVhdPath, pDumpStruct->hDlg))
			SendMessage(pDumpStruct->hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"Disk converted to VHD successfully!", 1);
		else
			SendMessage(pDumpStruct->hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"Failed to convert disk to VHD!", 1);
	}

	return 0;
}

LRESULT CALLBACK MainDlgProc( HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam )
{
	WCHAR sVhdPath[MAX_PATH] = {0};
	WCHAR* sPhysicalDrive = 0;
	int nLen = 0;
	DWORD dwThrdID = 0;
	int nComboIndex = 0;

	COLORREF unvisited = RGB(0,102,204);
	COLORREF visited = RGB(128,0,128);
	HFONT hFont;
	LOGFONT lf;


	switch(Msg)
	{
	case WM_INITDIALOG:

		SendMessage(hDlg, WM_SETICON, ICON_BIG, (LPARAM)hIcon);

		PopulatePhysicalDriveComboBox(hDlg);

		AddListHeader(hDlg);
		
		// Set default mode to VHD to Disk
		CheckDlgButton(hDlg, IDC_RADIO_VHD_TO_DISK, BST_CHECKED);
		UpdateUIMode(hDlg, TRUE);
		
		hFont = (HFONT)SendMessage(GetDlgItem(hDlg, IDC_STATIC_NAME), WM_GETFONT, 0, 0);
		GetObject(hFont, sizeof(LOGFONT), &lf);
		lf.lfWeight = FW_BOLD;
		
		SendDlgItemMessage(hDlg, IDC_STATIC_NAME, WM_SETFONT, (WPARAM)CreateFontIndirect(&lf), (LPARAM)TRUE);

		urlctrl_set(GetDlgItem(hDlg, IDC_STATIC_URL), L"http://www.wooxo.com", &unvisited, &visited, UCF_KBD | UCF_FIT);

		SetDlgItemText(hDlg, IDC_STATIC_STATUS, L"IDLE");
		
		return TRUE;

	case WM_COMMAND:
		switch(LOWORD(wParam)) 
		{
		case IDC_RADIO_VHD_TO_DISK:
			UpdateUIMode(hDlg, TRUE);
			ListView_DeleteAllItems(GetDlgItem(hDlg, IDC_LIST_VOLUME));
			return TRUE;

		case IDC_RADIO_DISK_TO_VHD:
			UpdateUIMode(hDlg, FALSE);
			ListView_DeleteAllItems(GetDlgItem(hDlg, IDC_LIST_VOLUME));
			return TRUE;

		case IDC_BUTTON_BROWSE_VHD_SAVE:
			sVhdPath[0] = L'\0';
			if (DoSaveFileDialog(sVhdPath, L"VHD Files (*.vhd)\0*.vhd\0All Files (*.*)\0*.*\0", L"vhd") == IDOK)
			{
				SetDlgItemText(hDlg, IDC_EDIT_VHD_SAVE_FILE, sVhdPath);
			}
			return TRUE;

		case IDCANCEL:
			DWORD dwExit;
			if(hDumpThread)
			{
				GetExitCodeThread(hDumpThread, &dwExit);
				if(dwExit != STILL_ACTIVE)
				{
					TerminateThread(hDumpThread, -1);
				}
			}
			
			// Cleanup objects
			if(pVhd2disk)
			{
				delete pVhd2disk;
				pVhd2disk = NULL;
			}
			if(pDisk2vhd)
			{
				delete pDisk2vhd;
				pDisk2vhd = NULL;
			}
			
			EndDialog(hDlg, LOWORD( wParam ));
			hDlg = NULL;
			return TRUE;

		case IDC_BUTTON_BROWSE_VHD:
			if (DoFileDialog(sVhdPath, L"VHD Files (*.vhd)\0*.vhd\0All Files (*.*)\0*.*\0", L"vhd") == IDOK)
			{
				SetDlgItemText(hDlg, IDC_EDIT_VHD_FILE, sVhdPath);

				if(pVhd2disk)
					delete pVhd2disk;

				ListView_DeleteAllItems(GetDlgItem(hDlg, IDC_LIST_VOLUME));

				pVhd2disk = new CVhdToDisk(sVhdPath);
				if(pVhd2disk)
					pVhd2disk->ParseFirstSector(hDlg);
			}
			return TRUE;

		case IDC_BUTTON_START:
			
			ZeroMemory(&dmpstruct, sizeof(DUMPTHRDSTRUCT));
			dmpstruct.hDlg = hDlg;
			
			// Determine operation mode
			dmpstruct.bVhdToDisk = IsDlgButtonChecked(hDlg, IDC_RADIO_VHD_TO_DISK) == BST_CHECKED;
			
			if(dmpstruct.bVhdToDisk)
			{
				// VHD to Disk: get VHD file path
				GetDlgItemText(hDlg, IDC_EDIT_VHD_FILE, dmpstruct.sVhdPath, MAX_PATH);
			}
			else
			{
				// Disk to VHD: get save VHD path
				GetDlgItemText(hDlg, IDC_EDIT_VHD_SAVE_FILE, dmpstruct.sVhdPath, MAX_PATH);
			}
			
			if(wcslen(dmpstruct.sVhdPath) < 3) return TRUE;

			nComboIndex = SendMessage(GetDlgItem(hDlg, IDC_COMBO1), CB_GETCURSEL, 0, 0);
			if(nComboIndex == CB_ERR) return TRUE;
			nLen = SendMessage(GetDlgItem(hDlg, IDC_COMBO1), CB_GETLBTEXTLEN, nComboIndex, 0);

			if(nLen < 8 || nLen > 64) return TRUE;
			SendMessage(GetDlgItem(hDlg, IDC_COMBO1), CB_GETLBTEXT, nComboIndex, (LPARAM)dmpstruct.sDrive);
			
			LPCWSTR warningMsg = dmpstruct.bVhdToDisk ? 
				L"Are you sure to proceed? This operation will destroy all data present on the target drive" :
				L"Are you sure to proceed? This operation will create a VHD file from the selected drive";
				
			if(MessageBox(hDlg, warningMsg, L"Warning!", MB_OKCANCEL) == IDOK)
			{
				hDumpThread = CreateThread(NULL, 0, DumpThread, &dmpstruct, 0, &dwThrdID);
				if(hDumpThread != INVALID_HANDLE_VALUE)
				{
					EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_START), FALSE);
					EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_BROWSE_VHD), FALSE);
					EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_BROWSE_VHD_SAVE), FALSE);
					EnableWindow(GetDlgItem(hDlg, IDC_EDIT_VHD_FILE), FALSE);
					EnableWindow(GetDlgItem(hDlg, IDC_EDIT_VHD_SAVE_FILE), FALSE);
					EnableWindow(GetDlgItem(hDlg, IDC_COMBO1), FALSE);
					EnableWindow(GetDlgItem(hDlg, IDC_RADIO_VHD_TO_DISK), FALSE);
					EnableWindow(GetDlgItem(hDlg, IDC_RADIO_DISK_TO_VHD), FALSE);

					ShowWindow(GetDlgItem(hDlg, IDC_PROGRESS_DUMP), SW_SHOW);
					
					// Initialize progress bar
					HWND hProgress = GetDlgItem(hDlg, IDC_PROGRESS_DUMP);
					if(hProgress)
					{
						SendMessage(hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
						SendMessage(hProgress, PBM_SETPOS, 0, 0);
					}
				}
				else
				{
					SetDlgItemText(hDlg, IDC_STATIC_STATUS, L"Failed to start the dump's thread");
				}
			}

			return TRUE;
		}
		break;
	case WM_SIZING:

		return TRUE;

	case WM_DRAWITEM:
		{
			DRAWITEMSTRUCT* pDIS = (DRAWITEMSTRUCT*)lParam;
			if(pDIS && pDIS->CtlID == IDC_STATIC_PARTITION_VIEW)
			{
				DrawPartitionView(pDIS->hwndItem, pDIS->hDC, &pDIS->rcItem);
				return TRUE;
			}
		}
		break;

	case MYWM_UPDATE_STATUS:
		{
			// Only update if the text has actually changed to reduce flicker
			LPCWSTR newStatusText = (LPCWSTR)wParam;
			if(wcscmp(g_lastStatusText, newStatusText) != 0)
			{
				wcscpy_s(g_lastStatusText, 512, newStatusText);
				SetDlgItemText(hDlg, IDC_STATIC_STATUS, newStatusText);
			}

			if(LOWORD(lParam) == 1)
			{
				EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_START), TRUE);
				EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_BROWSE_VHD), TRUE);
				EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_BROWSE_VHD_SAVE), TRUE);
				EnableWindow(GetDlgItem(hDlg, IDC_EDIT_VHD_FILE), TRUE);
				EnableWindow(GetDlgItem(hDlg, IDC_EDIT_VHD_SAVE_FILE), TRUE);
				EnableWindow(GetDlgItem(hDlg, IDC_COMBO1), TRUE);
				EnableWindow(GetDlgItem(hDlg, IDC_RADIO_VHD_TO_DISK), TRUE);
				EnableWindow(GetDlgItem(hDlg, IDC_RADIO_DISK_TO_VHD), TRUE);
				
				// Hide progress bar when operation completes
				ShowWindow(GetDlgItem(hDlg, IDC_PROGRESS_DUMP), SW_HIDE);
			}
		}
		return TRUE;

	case MYWM_UPDATE_PROGRESSBAR:
		
		// Update progress bar
		HWND hProgress = GetDlgItem(hDlg, IDC_PROGRESS_DUMP);
		if(hProgress)
		{
			SendMessage(hProgress, PBM_SETPOS, wParam, 0);
		}
		
		return TRUE;
	
	}

	return FALSE;
}