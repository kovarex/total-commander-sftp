// Loads the plugin like Total Commander does and drives it against a test server.
// Usage: harness <plugin.wfx64> <inidir> <connection> <remote dir> <local file>
#include <windows.h>
#include <cstdio>
#include <string>
#include "../src/wfx.h"

static DWORD g_t0 = GetTickCount(); static int g_last = -1;
static int __stdcall progressW(int, WCHAR*, WCHAR*, int p) { if (getenv("TRACE") && p != g_last) { printf("  progress %3d%% at %.2fs\n", p, (GetTickCount() - g_t0) / 1000.0); g_last = p; } return 0; }
static void __stdcall logW(int, int type, WCHAR* s) { wprintf(L"  [log %d] %ls\n", type, s); }
static BOOL __stdcall requestW(int, int type, WCHAR*, WCHAR* text, WCHAR* ret, int maxlen) {
	wprintf(L"  [request %d] %ls\n", type, text ? text : L"");
	if (type == RT_MsgYesNo) {
		// trust the host key, decline saving passwords
		return text && wcsstr(text, L"Trust") ? TRUE : FALSE;
	}
	if (type == RT_MsgOK) {
		if (text && wcsstr(text, L"SAME")) printf("PASS remote file identical to local\n");
		return TRUE;
	}
	return FALSE;
}

typedef int(__stdcall* FsInitW_t)(int, tProgressProcW, tLogProcW, tRequestProcW);
typedef void(__stdcall* FsSetDefaultParams_t)(FsDefaultParamStruct*);
typedef HANDLE(__stdcall* FsFindFirstW_t)(WCHAR*, WIN32_FIND_DATAW*);
typedef BOOL(__stdcall* FsFindNextW_t)(HANDLE, WIN32_FIND_DATAW*);
typedef int(__stdcall* FsFindClose_t)(HANDLE);
typedef int(__stdcall* FsPutFileW_t)(WCHAR*, WCHAR*, int);
typedef int(__stdcall* FsGetFileW_t)(WCHAR*, WCHAR*, int, RemoteInfoStruct*);
typedef int(__stdcall* FsExecuteFileW_t)(HWND, WCHAR*, WCHAR*);
typedef BOOL(__stdcall* FsMkDirW_t)(WCHAR*);
typedef BOOL(__stdcall* FsDeleteFileW_t)(WCHAR*);
typedef int(__stdcall* FsRenMovFileW_t)(WCHAR*, WCHAR*, BOOL, BOOL, RemoteInfoStruct*);
typedef BOOL(__stdcall* FsSetTimeW_t)(WCHAR*, FILETIME*, FILETIME*, FILETIME*);

static int failures = 0;
static void check(bool ok, const char* what) {
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok) failures++;
}

static int listDir(FsFindFirstW_t ff, FsFindNextW_t fn, FsFindClose_t fc, const std::wstring& dir, const wchar_t* want, WIN32_FIND_DATAW* found) {
	WIN32_FIND_DATAW fd;
	std::wstring d = dir;
	HANDLE h = ff(d.data(), &fd);
	if (h == INVALID_HANDLE_VALUE) return -1;
	int n = 0;
	do {
		n++;
		if (want && wcscmp(fd.cFileName, want) == 0 && found) *found = fd;
	} while (fn(h, &fd));
	fc(h);
	return n;
}

int wmain(int argc, wchar_t** argv) {
	if (argc < 7) { printf("usage\n"); return 2; }
	HMODULE m = LoadLibraryW(argv[1]);
	if (!m) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
#define GET(n) auto n = (n##_t)GetProcAddress(m, #n); if (!n) { printf("missing " #n "\n"); return 1; }
	GET(FsInitW) GET(FsSetDefaultParams) GET(FsFindFirstW) GET(FsFindNextW) GET(FsFindClose) GET(FsPutFileW) GET(FsGetFileW)
	GET(FsExecuteFileW) GET(FsMkDirW) GET(FsDeleteFileW) GET(FsRenMovFileW) GET(FsSetTimeW)
	auto FsRemoveDirW = (FsMkDirW_t)GetProcAddress(m, "FsRemoveDirW");
	FsInitW(1, progressW, logW, requestW);
	FsDefaultParamStruct dps = {sizeof(dps), 0, 21, ""};
	WideCharToMultiByte(CP_ACP, 0, (std::wstring(argv[2]) + L"\\wincmd.ini").c_str(), -1, dps.DefaultIniName, MAX_PATH, 0, 0);
	FsSetDefaultParams(&dps);

	std::wstring conn = argv[3], rdir = argv[4], local = argv[5];
	std::wstring base = L"\\" + conn + rdir;

	check(listDir(FsFindFirstW, FsFindNextW, FsFindClose, L"\\", conn.c_str(), nullptr) >= 3, "root lists connections");
	WCHAR target[MAX_PATH] = L"";
	wcscpy(target, (L"\\" + conn).c_str());
	int rc = FsExecuteFileW(0, target, (WCHAR*)L"open");
	wprintf(L"  open -> %d %ls\n", rc, target);
	check(rc == FS_EXEC_SYMLINK, "open connection jumps to home");

	std::wstring rfile = base + L"\\wine-up.bin";
	FsDeleteFileW(rfile.data());
	DWORD t0 = GetTickCount();
	rc = FsPutFileW(local.data(), rfile.data(), 0);
	DWORD t1 = GetTickCount();
	HANDLE lf = CreateFileW(local.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
	LARGE_INTEGER sz; GetFileSizeEx(lf, &sz); CloseHandle(lf);
	printf("  upload %.1f MB in %.2f s = %.2f Mbit/s\n", sz.QuadPart / 1e6, (t1 - t0) / 1000.0, sz.QuadPart * 8.0 / ((t1 - t0) / 1000.0) / 1e6);
	check(rc == FS_FILE_OK, "upload");
	{
		std::wstring cmp = L"quote cmp wine-up.bin " + std::wstring(argv[6]) + L" && echo SAME";
		std::wstring dirPath = base;
		FsExecuteFileW(0, dirPath.data(), cmp.data());
	}
	if (getenv("UPLOAD_ONLY"))
		return failures ? 1 : 0;
	check(FsPutFileW(local.data(), rfile.data(), 0) == FS_FILE_EXISTSRESUMEALLOWED, "upload onto existing file asks");
	check(FsPutFileW(local.data(), rfile.data(), FS_COPYFLAGS_OVERWRITE) == FS_FILE_OK, "upload overwrite");

	{
		// resume: cut the remote file short through the plugin's own server-side command, then resume
		std::wstring cut = L"quote truncate -s 3000001 wine-up.bin";
		std::wstring dirPath = base;
		FsExecuteFileW(0, dirPath.data(), cut.data());
		check(FsPutFileW(local.data(), rfile.data(), FS_COPYFLAGS_RESUME) == FS_FILE_OK, "resume upload");
		std::wstring cmp = L"quote cmp wine-up.bin " + std::wstring(argv[6]) + L" && echo SAME";
		FsExecuteFileW(0, dirPath.data(), cmp.data());
	}
	WIN32_FIND_DATAW fd = {};
	listDir(FsFindFirstW, FsFindNextW, FsFindClose, base, L"wine-up.bin", &fd);
	check(((long long)fd.nFileSizeHigh << 32 | fd.nFileSizeLow) == sz.QuadPart, "listing shows uploaded size");

	std::wstring dl = local + L".dl";
	DeleteFileW(dl.c_str());
	RemoteInfoStruct ri = {fd.nFileSizeLow, fd.nFileSizeHigh, fd.ftLastWriteTime, 0};
	t0 = GetTickCount();
	rc = FsGetFileW(rfile.data(), dl.data(), 0, &ri);
	t1 = GetTickCount();
	printf("  download %.2f s = %.2f Mbit/s\n", (t1 - t0) / 1000.0, sz.QuadPart * 8.0 / ((t1 - t0) / 1000.0) / 1e6);
	check(rc == FS_FILE_OK, "download");
	check(FsGetFileW(rfile.data(), dl.data(), 0, &ri) == FS_FILE_EXISTSRESUMEALLOWED, "download onto existing file asks");

	FILETIME ft; SYSTEMTIME st = {2001, 2, 0, 3, 4, 5, 6, 0}; SystemTimeToFileTime(&st, &ft);
	check(FsSetTimeW(rfile.data(), nullptr, nullptr, &ft), "set time");
	listDir(FsFindFirstW, FsFindNextW, FsFindClose, base, L"wine-up.bin", &fd);
	check(fd.ftLastWriteTime.dwLowDateTime == ft.dwLowDateTime && fd.ftLastWriteTime.dwHighDateTime == ft.dwHighDateTime, "time preserved");

	std::wstring chmod = L"chmod 600";
	check(FsExecuteFileW(0, rfile.data(), chmod.data()) == FS_EXEC_OK, "chmod");
	listDir(FsFindFirstW, FsFindNextW, FsFindClose, base, L"wine-up.bin", &fd);
	check((fd.dwReserved0 & 0777) == 0600, "chmod visible in listing");

	std::wstring dir = base + L"\\wine dir žluťoučký";
	FsRemoveDirW(dir.data());
	check(FsMkDirW(dir.data()), "mkdir with unicode name");
	std::wstring moved = dir + L"\\moved.bin";
	check(FsRenMovFileW(rfile.data(), moved.data(), TRUE, FALSE, &ri) == FS_FILE_OK, "move into dir");
	std::wstring copy = dir + L"\\copy.bin";
	check(FsRenMovFileW(moved.data(), copy.data(), FALSE, FALSE, &ri) == FS_FILE_OK, "server-side copy");
	check(FsRenMovFileW(moved.data(), copy.data(), TRUE, FALSE, &ri) == FS_FILE_EXISTS, "move onto existing asks");
	check(FsRenMovFileW(moved.data(), copy.data(), TRUE, TRUE, &ri) == FS_FILE_OK, "move with overwrite");
	check(listDir(FsFindFirstW, FsFindNextW, FsFindClose, dir, nullptr, nullptr) == 1, "dir has one file");
	check(FsDeleteFileW(copy.data()), "delete");
	check(listDir(FsFindFirstW, FsFindNextW, FsFindClose, dir, nullptr, nullptr) == -1 && GetLastError() == ERROR_NO_MORE_FILES, "empty dir reports no files");
	check(FsRemoveDirW(dir.data()), "rmdir");
	check(listDir(FsFindFirstW, FsFindNextW, FsFindClose, dir, nullptr, nullptr) == -1 && GetLastError() != ERROR_NO_MORE_FILES, "missing dir reports error");

	// many small files: listing speed
	t0 = GetTickCount();
	int n = listDir(FsFindFirstW, FsFindNextW, FsFindClose, base + L"\\many", nullptr, nullptr);
	t1 = GetTickCount();
	printf("  listed %d entries in %.2f s\n", n, (t1 - t0) / 1000.0);

	printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
