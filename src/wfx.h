// Total Commander file system plugin (WFX) interface, written from the
// public interface documentation (fsplugin help, interface version 2.1).
#pragma once
#include <windows.h>

#define FS_FILE_OK 0
#define FS_FILE_EXISTS 1
#define FS_FILE_NOTFOUND 2
#define FS_FILE_READERROR 3
#define FS_FILE_WRITEERROR 4
#define FS_FILE_USERABORT 5
#define FS_FILE_NOTSUPPORTED 6
#define FS_FILE_EXISTSRESUMEALLOWED 7

#define FS_EXEC_OK 0
#define FS_EXEC_ERROR 1
#define FS_EXEC_YOURSELF -1
#define FS_EXEC_SYMLINK -2

#define FS_COPYFLAGS_OVERWRITE 1
#define FS_COPYFLAGS_RESUME 2
#define FS_COPYFLAGS_MOVE 4
#define FS_COPYFLAGS_EXISTS_SAMECASE 8
#define FS_COPYFLAGS_EXISTS_DIFFERENTCASE 16

#define RT_Other 0
#define RT_UserName 1
#define RT_Password 2
#define RT_Account 3
#define RT_TargetDir 6
#define RT_URL 7
#define RT_MsgOK 8
#define RT_MsgYesNo 9
#define RT_MsgOKCancel 10

#define MSGTYPE_CONNECT 1
#define MSGTYPE_DISCONNECT 2
#define MSGTYPE_DETAILS 3
#define MSGTYPE_TRANSFERCOMPLETE 4
#define MSGTYPE_CONNECTCOMPLETE 5
#define MSGTYPE_IMPORTANTERROR 6
#define MSGTYPE_OPERATIONCOMPLETE 7

#define FS_STATUS_START 0
#define FS_STATUS_END 1
#define FS_STATUS_OP_GET_MULTI_THREAD 19
#define FS_STATUS_OP_PUT_MULTI_THREAD 20

#define FS_ICON_USEDEFAULT 0
#define FS_ICON_EXTRACTED 1
#define FS_ICON_EXTRACTED_DESTROY 2
#define FS_ICONFLAG_SMALL 1

#define FS_CRYPT_SAVE_PASSWORD 1
#define FS_CRYPT_LOAD_PASSWORD 2
#define FS_CRYPT_LOAD_PASSWORD_NO_UI 3
#define FS_CRYPT_COPY_PASSWORD 4
#define FS_CRYPT_MOVE_PASSWORD 5
#define FS_CRYPT_DELETE_PASSWORD 6

#define BG_DOWNLOAD 1
#define BG_UPLOAD 2
#define BG_ASK_USER 4

typedef struct {
	DWORD SizeLow, SizeHigh;
	FILETIME LastWriteTime;
	int Attr;
} RemoteInfoStruct;

typedef struct {
	int size;
	DWORD PluginInterfaceVersionLow;
	DWORD PluginInterfaceVersionHi;
	char DefaultIniName[MAX_PATH];
} FsDefaultParamStruct;

typedef int (__stdcall *tProgressProc)(int PluginNr, char* SourceName, char* TargetName, int PercentDone);
typedef int (__stdcall *tProgressProcW)(int PluginNr, WCHAR* SourceName, WCHAR* TargetName, int PercentDone);
typedef void (__stdcall *tLogProc)(int PluginNr, int MsgType, char* LogString);
typedef void (__stdcall *tLogProcW)(int PluginNr, int MsgType, WCHAR* LogString);
typedef BOOL (__stdcall *tRequestProc)(int PluginNr, int RequestType, char* CustomTitle, char* CustomText, char* ReturnedText, int maxlen);
typedef BOOL (__stdcall *tRequestProcW)(int PluginNr, int RequestType, WCHAR* CustomTitle, WCHAR* CustomText, WCHAR* ReturnedText, int maxlen);
typedef int (__stdcall *tCryptProc)(int PluginNr, int CryptoNr, int Mode, char* ConnectionName, char* Password, int maxlen);
typedef int (__stdcall *tCryptProcW)(int PluginNr, int CryptoNr, int Mode, WCHAR* ConnectionName, WCHAR* Password, int maxlen);
