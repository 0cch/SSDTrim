// SSDTrim.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <windows.h>
#include <vector>
#include <assert.h>
#include "color_print.h"

using std::vector;

typedef struct _FREE_ENTRY {
	ULONGLONG Address;
	ULONGLONG Length;
} FREE_ENTRY, *PFREE_ENTRY;

typedef struct _LBA_ENTRY {
	ULONGLONG LBAValue : 48;
	ULONGLONG Length : 16;
} LBA_ENTRY, *PLBA_ENTRY;

#define MAX_LBA_ENTRY_SIZE 0xffff
#define IOCTL_VOLUME_LOGICAL_TO_PHYSICAL CTL_CODE(IOCTL_VOLUME_BASE, 8, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _VOLUME_LOGICAL_OFFSET {
	LONGLONG LogicalOffset;
} VOLUME_LOGICAL_OFFSET, *PVOLUME_LOGICAL_OFFSET;

typedef struct _VOLUME_PHYSICAL_OFFSET {
	ULONG    DiskNumber;
	LONGLONG Offset;
} VOLUME_PHYSICAL_OFFSET, *PVOLUME_PHYSICAL_OFFSET;

typedef struct _VOLUME_PHYSICAL_OFFSETS {
	ULONG                  NumberOfPhysicalOffsets;
	VOLUME_PHYSICAL_OFFSET PhysicalOffset[ANYSIZE_ARRAY];
} VOLUME_PHYSICAL_OFFSETS, *PVOLUME_PHYSICAL_OFFSETS;

#pragma pack(push, 1)

typedef struct _ATA_PASS_THROUGH_EX {
	USHORT    Length;
	USHORT    AtaFlags;
	UCHAR     PathId;
	UCHAR     TargetId;
	UCHAR     Lun;
	UCHAR     ReservedAsUchar;
	ULONG     DataTransferLength;
	ULONG     TimeOutValue;
	ULONG     ReservedAsUlong;
	ULONG_PTR DataBufferOffset;
	UCHAR     PreviousTaskFile[8];
	UCHAR     CurrentTaskFile[8];
} ATA_PASS_THROUGH_EX, *PATA_PASS_THROUGH_EX;

#pragma pack(pop)

#define ATA_FLAGS_DRDY_REQUIRED 0x01
#define ATA_FLAGS_DATA_IN (1 << 1)
#define ATA_FLAGS_DATA_OUT (1 << 2)
#define ATA_FLAGS_48BIT_COMMAND (1 << 3)
#define ATA_FLAGS_USE_DMA (1 <<4 )

#define IOCTL_SCSI_BASE                 FILE_DEVICE_CONTROLLER
#define IOCTL_ATA_PASS_THROUGH          CTL_CODE(IOCTL_SCSI_BASE, 0x040b, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_ATA_PASS_THROUGH_DIRECT   CTL_CODE(IOCTL_SCSI_BASE, 0x040c, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define TRIM_CMD 6

void CloseFileHandleList(vector<HANDLE> &file_handle_list)
{
	for (size_t i = 0; i < file_handle_list.size(); i++) {
		CloseHandle(file_handle_list[i]);
	}

	file_handle_list.clear();
}



BOOL CreateLargeFile(WCHAR volume_letter, vector<HANDLE> &file_handle_list)
{
	const WCHAR file_guid_name[] = L"%c:\\{D4E01F7A-E88C-416d-AFD2-79C0F12FC74E}_%u";
	const LARGE_INTEGER file_size = {0x80000000, 0};
	WCHAR tmp_file_path[MAX_PATH];
	BOOL retval = FALSE;

	for (int i = 0; ; i++) {
		swprintf_s(tmp_file_path, MAX_PATH, file_guid_name, volume_letter, i);
		HANDLE h = CreateFileW(tmp_file_path, 
			GENERIC_WRITE, 
			0, 
			NULL, 
			CREATE_ALWAYS, 
			FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, 
			0);

		if (h == INVALID_HANDLE_VALUE) {
			CloseFileHandleList(file_handle_list);
			break;
		}

		if (!SetFilePointerEx(h, file_size, NULL, FILE_BEGIN)) {
			CloseFileHandleList(file_handle_list);
			break;
		}

		if (!SetEndOfFile(h)) {
			if (GetLastError() == ERROR_DISK_FULL) {
				retval = TRUE;
			}
			break;
		}

		file_handle_list.push_back(h);
	}

	return retval;
}

BOOL GetFileLCNList(HANDLE file_handle, vector<FREE_ENTRY> &free_list)
{
	STARTING_VCN_INPUT_BUFFER input_buffer = {0};
	RETRIEVAL_POINTERS_BUFFER output_buffer = {0};
	FREE_ENTRY free_entry = {0};
	BOOL retval = FALSE;
	ULONG last_error = NO_ERROR;
	ULONG bytes_returned = 0;
	do {
		retval = DeviceIoControl(file_handle, 
			FSCTL_GET_RETRIEVAL_POINTERS,
			&input_buffer, 
			sizeof(input_buffer),
			&output_buffer, 
			sizeof(output_buffer),
			&bytes_returned,
			NULL);
		last_error = GetLastError();
		if (!retval && last_error != ERROR_MORE_DATA) {
			break;
		}
		
		input_buffer.StartingVcn.QuadPart = output_buffer.Extents[0].NextVcn.QuadPart;

		free_entry.Address = output_buffer.Extents[0].Lcn.QuadPart;
		free_entry.Length = output_buffer.Extents[0].NextVcn.QuadPart - output_buffer.StartingVcn.QuadPart;
		free_list.push_back(free_entry);

	} while (!retval && last_error == ERROR_MORE_DATA);

	return retval;
}

BOOL GetFileListLCNList(vector<HANDLE> &file_list_handle, vector<FREE_ENTRY> &free_list)
{
	BOOL retval = TRUE;
	vector<FREE_ENTRY> tmp_list;

	for (size_t i = 0; i < file_list_handle.size(); i++) {

		tmp_list.clear();
		if (!GetFileLCNList(file_list_handle[i], tmp_list)) {
			retval = FALSE;
			break;
		}

		// printf("file=%u block_number=%u\n", i, tmp_list.size());

		free_list.insert(free_list.end(), tmp_list.begin(), tmp_list.end());
	}

	return retval;
}

BOOL ConvertClusterListToSectorList(WCHAR volume_letter, vector<FREE_ENTRY> &free_cluster_list)
{
	WCHAR volume_name[] = L"C:\\";
	volume_name[0] = volume_letter;

	ULONG sectors_per_cluster = 0;
	if (!GetDiskFreeSpaceW(volume_name, &sectors_per_cluster, NULL, NULL, NULL)) {
		return FALSE;
	}


	for (size_t i = 0; i < free_cluster_list.size(); i++) {
		free_cluster_list[i].Address *= sectors_per_cluster;
		free_cluster_list[i].Length *= sectors_per_cluster;
	}

	return TRUE;
}

BOOL ConvertSectorAddrFromVolumeToDisk(WCHAR volume_letter, vector<FREE_ENTRY> &free_list)
{
	WCHAR volume_name[] = L"C:\\";
	volume_name[0] = volume_letter;
	ULONG bytes_per_sector = 0;
	if (!GetDiskFreeSpaceW(volume_name, NULL, &bytes_per_sector, NULL, NULL)) {
		return FALSE;
	}

	WCHAR perfix_volume_name[] = L"\\\\.\\C:";
	perfix_volume_name[4] = volume_letter;
	HANDLE volume_handle = CreateFileW(perfix_volume_name, 
		GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, 0);

	if (volume_handle == INVALID_HANDLE_VALUE) {
		return FALSE;
	}

	ULONG bytes_returned = 0;
	VOLUME_LOGICAL_OFFSET volume_offset = {0};
	VOLUME_PHYSICAL_OFFSETS physical_offsets = {0};

	BOOL retval = FALSE;

	for (size_t i = 0; i < free_list.size(); i++) {

		volume_offset.LogicalOffset = free_list[i].Address * bytes_per_sector;

		retval = DeviceIoControl(volume_handle,
			IOCTL_VOLUME_LOGICAL_TO_PHYSICAL, 
			&volume_offset,
			sizeof(volume_offset),
			&physical_offsets,
			sizeof(physical_offsets),
			&bytes_returned,
			NULL);

		if (!retval) {
			break;
		}

		free_list[i].Address = physical_offsets.PhysicalOffset[0].Offset / bytes_per_sector;
// 		printf("%u v=%I64x p=%I64x l=%I64x\n", i, 
// 			volume_offset.LogicalOffset, 
// 			physical_offsets.PhysicalOffset[0].Offset, 
// 			free_list[i].Length * bytes_per_sector);

	}

	CloseHandle(volume_handle);

	return retval;
}

void SplitsLargeFreeEntryToLBAList(ULONGLONG entry_address, ULONGLONG entry_length, vector<LBA_ENTRY> &lba_list)
{
	// 	static int i = 0;
	// 	printf("%d entry=%I64x length=%I64x\n", i++, entry_address, entry_length);

	ULONGLONG left_length = entry_length;
	ULONGLONG current_address = entry_address;
	LBA_ENTRY lba_entry = {0};
	ULONGLONG step_size = 0;

	while (left_length > 0) {
		if (left_length > MAX_LBA_ENTRY_SIZE) {
			step_size = MAX_LBA_ENTRY_SIZE;
		}
		else {
			step_size = left_length;
		}

		lba_entry.Length = step_size;
		lba_entry.LBAValue = current_address;
		lba_list.push_back(lba_entry);

		//  printf("  - lba_addr=%I64x lba_length=%I64x\n", lba_entry.LBAValue, lba_entry.Length);

		left_length -= step_size;
		current_address += step_size;
	}
}

void ConvertFreeListToLBAList(vector<FREE_ENTRY> &free_list, vector<LBA_ENTRY> &lba_list)
{
	LBA_ENTRY lba_entry = {0};
	for (size_t i = 0; i < free_list.size(); i++) {
		if (free_list[i].Length > MAX_LBA_ENTRY_SIZE) {
			SplitsLargeFreeEntryToLBAList(free_list[i].Address, free_list[i].Length, lba_list);
		}
		else {
			lba_entry.LBAValue = free_list[i].Address;
			lba_entry.Length = free_list[i].Length;
			lba_list.push_back(lba_entry);
		}
	}

}

BOOL IsWindowsXpSp3()
{
	OSVERSIONINFOEXW os_ver = {0};
	os_ver.dwOSVersionInfoSize = sizeof(os_ver);
	BOOL retval = GetVersionExW((LPOSVERSIONINFOW)&os_ver);
	assert(retval);
	if (retval && 
		os_ver.dwMajorVersion == 5 &&
		os_ver.dwMinorVersion == 1 &&
		os_ver.wServicePackMajor == 3) {
			return TRUE;		
	}

	return FALSE;
}

BOOL IsNTFS(WCHAR volume_letter)
{
	WCHAR volume_name[] = L"C:\\";
	volume_name[0] = volume_letter;
	WCHAR file_system_buffer[MAX_PATH];
	BOOL retval = GetVolumeInformationW(volume_name, NULL, 0, NULL, 0, NULL, file_system_buffer, MAX_PATH);
	assert(retval);
	if (retval && wcscmp(file_system_buffer, L"NTFS") == 0) {
		return TRUE;
	}

	return FALSE;
}

BOOL GetPhysicalDriveFromVolumeLetter(WCHAR volume_letter, ULONG &physical_drive_index)
{
	STORAGE_DEVICE_NUMBER number = {0};

	WCHAR volume_name[] = L"\\\\.\\C:";
	volume_name[4] = volume_letter;

	HANDLE volume_handle = CreateFileW(volume_name,
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		0,
		0);

	if (volume_handle == INVALID_HANDLE_VALUE) {
		return FALSE;
	}

	ULONG bytes_returned = 0;
	BOOL retval = DeviceIoControl(volume_handle,
		IOCTL_STORAGE_GET_DEVICE_NUMBER,
		NULL,
		0,
		&number,
		sizeof(number),
		&bytes_returned,
		NULL);

	CloseHandle(volume_handle);

	physical_drive_index = number.DeviceNumber;

	return retval;
}


BOOL IsSSDAndSupportTrim(ULONG physical_drive_index, BOOL &is_ssd, BOOL &support_trim)
{
	WCHAR drive_name[32] = {0};
	swprintf_s(drive_name, _countof(drive_name), L"\\\\.\\PHYSICALDRIVE%d", physical_drive_index);
	HANDLE drive_handle = CreateFileW(drive_name, 
		GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, 0);

	if (drive_handle == INVALID_HANDLE_VALUE) {
		return FALSE;
	}

	UCHAR raw_buffer[1024] = {0};
	PATA_PASS_THROUGH_EX ata_pass_through = (PATA_PASS_THROUGH_EX)raw_buffer;
	ULONG input_length = sizeof(ATA_PASS_THROUGH_EX) + 512;

	ata_pass_through->Length = sizeof(ATA_PASS_THROUGH_EX);
	ata_pass_through->AtaFlags = ATA_FLAGS_DATA_IN;
	ata_pass_through->DataTransferLength = 512;
	ata_pass_through->TimeOutValue = 10;
	ata_pass_through->DataBufferOffset = sizeof(ATA_PASS_THROUGH_EX);
	ata_pass_through->CurrentTaskFile[6] = ID_CMD;

	ULONG bytes_returned = 0;
	BOOL retval = DeviceIoControl(drive_handle, IOCTL_ATA_PASS_THROUGH, ata_pass_through, 
		sizeof(ATA_PASS_THROUGH_EX), ata_pass_through, input_length, &bytes_returned, NULL);

	//
	// WORD 217 : IS SSD = 1 SSD
	// WORD 169 : SUPPORT TRIM = 1 SUPPORT
	//

	USHORT *device_identify = (USHORT *)(raw_buffer + ata_pass_through->DataBufferOffset);
	is_ssd = (*(device_identify + 217)) == 1;
	support_trim = (*(device_identify + 169)) & 1;

	CloseHandle(drive_handle);
	return retval;
}


BOOL SendTrimCommand(ULONG physical_drive_index, vector<LBA_ENTRY> &lba_list)
{
	WCHAR drive_name[32] = {0};
	swprintf_s(drive_name, _countof(drive_name), L"\\\\.\\PHYSICALDRIVE%d", physical_drive_index);
	HANDLE drive_handle = CreateFileW(drive_name, 
		GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, 0);

	if (drive_handle == INVALID_HANDLE_VALUE) {
		return FALSE;
	}

	UCHAR raw_buffer[1024] = {0};
	PATA_PASS_THROUGH_EX ata_pass_through = (PATA_PASS_THROUGH_EX)raw_buffer;
	ULONG input_length = sizeof(ATA_PASS_THROUGH_EX) + 512;
	PLBA_ENTRY lba_entry = (PLBA_ENTRY)(raw_buffer + sizeof(ATA_PASS_THROUGH_EX));
	ULONG bytes_returned = 0;
	BOOL retval = FALSE;

	for (size_t i = 0; i < lba_list.size(); i++) {

		ZeroMemory(raw_buffer, sizeof(raw_buffer));
		ata_pass_through->Length = sizeof(ATA_PASS_THROUGH_EX);
		ata_pass_through->AtaFlags = ATA_FLAGS_DATA_OUT | ATA_FLAGS_USE_DMA | ATA_FLAGS_DRDY_REQUIRED | ATA_FLAGS_48BIT_COMMAND;
		ata_pass_through->DataTransferLength = 512;
		ata_pass_through->TimeOutValue = 10;
		ata_pass_through->DataBufferOffset = sizeof(ATA_PASS_THROUGH_EX);
		ata_pass_through->CurrentTaskFile[0] = 1;				// Features register = TRIM
		ata_pass_through->CurrentTaskFile[1] = 1;				// Sector count register = Number of 512-byte blocks of LBA Range Entries to be transferred.
		ata_pass_through->CurrentTaskFile[6] = TRIM_CMD;		// Command register = DATA SET MANAGEMENT, DMA
		lba_entry->LBAValue = lba_list[i].LBAValue;
		lba_entry->Length = lba_list[i].Length;

		//
		// See d2015r3-ATAATAPI_Command_Set_-_2_ACS-2 -> chapter 7.10
		//

		retval = DeviceIoControl(drive_handle, IOCTL_ATA_PASS_THROUGH, ata_pass_through, 
			input_length, ata_pass_through, input_length, &bytes_returned, NULL);
		if (!retval) {
			break;
		}

		if (ata_pass_through->CurrentTaskFile[6] & 1) {
			retval = FALSE;
			break;
		}
	}

	CloseHandle(drive_handle);
	return retval;
}

class AutoCloseHandleList {
public:
	AutoCloseHandleList(vector<HANDLE> *handle_list) : handle_list_(handle_list) {};
	~AutoCloseHandleList() {CloseFileHandleList(*handle_list_);}

private:
	vector<HANDLE> *handle_list_;
	AutoCloseHandleList(const AutoCloseHandleList&);                 // Prevent copy-construction
	AutoCloseHandleList& operator=(const AutoCloseHandleList&);      // Prevent assignment

};


int _tmain(int argc, _TCHAR* argv[])
{
	WCHAR volume_letter = L'D';
	if (argc == 2 && wcslen(argv[1]) == 1 && isalpha(argv[1][0])) {
		volume_letter = argv[1][0];
	}
	else {
		PRINT_ERR("SSDTrim.exe <A-Z>\n");
		return 1;
	}

	//
	// Check OS environment.
	//

	ULONG physical_drive_index = (ULONG)-1;
	if (!GetPhysicalDriveFromVolumeLetter(volume_letter, physical_drive_index)) {
		PRINT_ERR("Failed to get physical drive number.\n");
		return 1;
	}

	if (!IsWindowsXpSp3()) {
		PRINT_ERR("This application requires Windows XP SP3.\n");
		return 1;
	}
	
	if (!IsNTFS(volume_letter)) {
		PRINT_ERR("Volume %C requires NTFS file system.\n", volume_letter);
		return 1;
	}

	BOOL is_ssd = FALSE, support_trim = FALSE;
	if (!IsSSDAndSupportTrim(physical_drive_index, is_ssd, support_trim)) {
		PRINT_ERR("Failed to get SSD information.\n");
		return 1;
	}
	
	if (!is_ssd) {
		PRINT_ERR("Target drive is NOT SSD.\n");
		return 1;
	}

	if (!support_trim) {
		PRINT_ERR("Target drive is NOT support Trim.\n");
		return 1;
	}
	
	PRINT_INFO("OS environment has been verified.\n");

	//
	// Create large file to fill the volume.
	//

	vector<HANDLE> file_handle_list;
	if (!CreateLargeFile(volume_letter, file_handle_list)) {
		PRINT_ERR("Failed to create 2GB files.\n");
		return 1;
	}
	
	AutoCloseHandleList auto_close_handle_list(&file_handle_list);

	PRINT_INFO("Create 2GB files = %u.\n", file_handle_list.size());
	// system("pause");
	
	//
	// Get file clusters in the file list.
	//

	vector<FREE_ENTRY> free_list;
	if (!GetFileListLCNList(file_handle_list, free_list)) {
		PRINT_ERR("Failed to get file clusters\n.");
		return 1;
	}

	PRINT_INFO("Number of clusters = %u\n", free_list.size());

	if (!ConvertClusterListToSectorList(volume_letter, free_list)) {
		PRINT_ERR("Failed to convert clusters to sectors\n.");
		return 1;
	}

	if (!ConvertSectorAddrFromVolumeToDisk(volume_letter, free_list)) {
		PRINT_ERR("Failed to convert volume offsets to disk\n.");
		return 1;
	}

// 	for (size_t i = 0; i < free_list.size(); i++) {
// 		printf("%u lba_start=%I64u lba_length=%I64u\n", i, free_list[i].Address, free_list[i].Length);
// 	}

	vector<LBA_ENTRY> lba_list;
	ConvertFreeListToLBAList(free_list, lba_list);
	PRINT_INFO("Number of LBA = %u\n", lba_list.size());

	if (!SendTrimCommand(physical_drive_index, lba_list)) {
		PRINT_ERR("Failed to send trim command.\n");
	}

	PRINT_INFO("TRIM SSD SUCCEED!\n");

	return 0;
}

