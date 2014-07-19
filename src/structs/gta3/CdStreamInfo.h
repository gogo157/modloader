// Generated by IDAStruct_To_CHeader.py
#pragma once
#include <windows.h>
#include "Queue.h"

#pragma pack(push, 1)
struct CdStream	// sizeof = 0x30
{
	DWORD nSectorOffset;
	DWORD nSectorsToRead;
	LPVOID lpBuffer;
	BYTE field_C;
	BYTE bLocked;
	BYTE bInUse;
	BYTE field_F;
	DWORD status;
	HANDLE semaphore;
	HANDLE hFile;
	OVERLAPPED overlapped;
};
#pragma pack(pop)

static_assert(sizeof(CdStream) == 0x30, "Incorrect struct size: CdStream");

#pragma pack(push, 1)
struct CdStreamInfo	// sizeof = 0x8C0
{
	Queue queue;
	CdStream* pStreams;
	DWORD thread_id;
	HANDLE semaphore;
	HANDLE thread;
	HANDLE streamHandles[32];
	DWORD streamCount;
	DWORD openStreamCount;
	CHAR  streamNames[2048];
	DWORD field_8A8;
	DWORD lastPosn;
	DWORD field_8B0;
	DWORD field_8B4;
	DWORD gtaint_id;
	DWORD gta3_id;
};
#pragma pack(pop)

static_assert(sizeof(CdStreamInfo) == 0x8C0, "Incorrect struct size: CdStreamInfo");