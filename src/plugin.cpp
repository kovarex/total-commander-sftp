// Total Commander entry points.
#include "common.h"
#include <shellapi.h>
#include <cstdio>

static const wchar_t* NEW_CONNECTION = L"<New connection>";
static const wchar_t* QUICK_CONNECTION = L"<Quick connection>";
static const wchar_t* QUICK_NAME = L"Quick connection";

// ---------------------------------------------------------------- paths

// "\name\dir\file" -> name="name", remote="/dir/file"
struct PluginPath {
	std::wstring name;
	std::string remote;  // UTF-8, always absolute
	bool root = false;   // the plugin root "\"
	bool connRoot = false;  // "\name" itself
};

static PluginPath splitPath(const WCHAR* path) {
	PluginPath p;
	std::wstring s = path ? path : L"\\";
	while (!s.empty() && s.front() == L'\\')
		s.erase(0, 1);
	while (!s.empty() && s.back() == L'\\')
		s.pop_back();
	if (s.empty()) {
		p.root = true;
		return p;
	}
	size_t sep = s.find(L'\\');
	p.name = s.substr(0, sep);
	std::wstring rest = sep == std::wstring::npos ? L"" : s.substr(sep);
	for (auto& c : rest)
		if (c == L'\\')
			c = L'/';
	p.connRoot = rest.empty();
	p.remote = rest.empty() ? "/" : toUtf8(rest);
	return p;
}

static std::wstring remoteToPluginPath(const std::wstring& name, const std::string& remote) {
	std::wstring r = fromUtf8(remote);
	for (auto& c : r)
		if (c == L'/')
			c = L'\\';
	if (r.empty() || r == L"\\")
		return L"\\" + name + L"\\";
	return L"\\" + name + r;
}

static std::string joinRemote(const std::string& dir, const std::string& name) {
	if (dir.empty() || dir.back() == '/')
		return dir + name;
	return dir + "/" + name;
}

// ---------------------------------------------------------------- helpers

// Runs op(session) and, if the connection turned out to be dead, reconnects once and retries.
template <class F>
static auto withSession(const std::wstring& name, F op, decltype(op(std::declval<Session&>())) failValue) -> decltype(op(std::declval<Session&>())) {
	for (int attempt = 0; attempt < 2; attempt++) {
		std::shared_ptr<Session> s = getSession(name);
		if (!s)
			return failValue;
		std::unique_lock<std::mutex> g(s->lock);
		if (!s->alive()) {
			if (!s->connect(true))
				return failValue;
		}
		auto result = op(*s);
		if (result != failValue || !s->connectionLost() || attempt == 1)
			return result;
		logMsg(MSGTYPE_DETAILS, L"Connection lost, reconnecting");
		s->close();
	}
	return failValue;
}

static bool remoteStat(Session& s, const std::string& path, LIBSSH2_SFTP_ATTRIBUTES& attrs, bool followLinks = true) {
	return libssh2_sftp_stat_ex(s.sftp, path.c_str(), (unsigned)path.size(), followLinks ? LIBSSH2_SFTP_STAT : LIBSSH2_SFTP_LSTAT, &attrs) == 0;
}

static bool progress(const std::wstring& src, const std::wstring& dst, int percent) {
	if (!g_host.progress)
		return false;
	std::wstring a = src, b = dst;
	return g_host.progress(g_host.pluginNr, a.data(), b.data(), percent) != 0;
}

static int percentOf(long long done, long long total) {
	if (total <= 0)
		return 0;
	long long p = done * 100 / total;
	return (int)(p > 100 ? 100 : p);
}

static size_t transferWindow() {
	int kb = iniInt(L"WindowKB", 2048);
	if (kb < 64)
		kb = 64;
	if (kb > 65536)
		kb = 65536;
	return (size_t)kb * 1024;
}

// ---------------------------------------------------------------- listing

struct FindState {
	std::vector<WIN32_FIND_DATAW> items;
	size_t next = 0;
};

static void addRootItem(FindState* st, const std::wstring& name) {
	WIN32_FIND_DATAW fd = {};
	copyW(fd.cFileName, name, MAX_PATH);
	fd.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
	fd.ftLastWriteTime.dwHighDateTime = 0xFFFFFFFF;  // no date
	fd.ftLastWriteTime.dwLowDateTime = 0xFFFFFFFE;
	st->items.push_back(fd);
}

static void fillFindData(WIN32_FIND_DATAW& fd, const std::wstring& name, const LIBSSH2_SFTP_ATTRIBUTES& a, bool isDir) {
	memset(&fd, 0, sizeof(fd));
	copyW(fd.cFileName, name, MAX_PATH);
	fd.dwFileAttributes = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
	if (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
		fd.dwFileAttributes |= 0x80000000;
		fd.dwReserved0 = a.permissions & 0xFFFF;
	}
	if (!isDir && (a.flags & LIBSSH2_SFTP_ATTR_SIZE)) {
		fd.nFileSizeLow = (DWORD)a.filesize;
		fd.nFileSizeHigh = (DWORD)(a.filesize >> 32);
	}
	if (a.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) {
		fd.ftLastWriteTime = unixToFileTime(a.mtime);
		fd.ftLastAccessTime = unixToFileTime(a.atime);
	} else {
		fd.ftLastWriteTime.dwHighDateTime = 0xFFFFFFFF;
		fd.ftLastWriteTime.dwLowDateTime = 0xFFFFFFFE;
	}
}

static bool listRemote(Session& s, const std::string& dir, FindState* st) {
	st->items.clear();
	LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_opendir(s.sftp, dir.c_str());
	if (!h)
		return false;
	char name[1024];
	LIBSSH2_SFTP_ATTRIBUTES attrs;
	std::vector<std::string> links;
	int rc;
	while ((rc = libssh2_sftp_readdir_ex(h, name, sizeof(name), nullptr, 0, &attrs)) > 0) {
		std::string n(name, rc);
		if (n == "." || n == "..")
			continue;
		bool isLink = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISLNK(attrs.permissions);
		bool isDir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
		WIN32_FIND_DATAW fd;
		fillFindData(fd, fromUtf8(n), attrs, isDir);
		if (isLink)
			links.push_back(n), fd.dwReserved1 = (DWORD)links.size();  // resolved below
		st->items.push_back(fd);
	}
	libssh2_sftp_closedir(h);
	if (rc < 0 && s.connectionLost())
		return false;
	// Symbolic links: show them as what they point to (one round trip each).
	if (!links.empty()) {
		for (auto& fd : st->items) {
			if (!fd.dwReserved1)
				continue;
			const std::string& n = links[fd.dwReserved1 - 1];
			fd.dwReserved1 = 0;
			LIBSSH2_SFTP_ATTRIBUTES target;
			if (remoteStat(s, joinRemote(dir, n), target)) {
				bool isDir = (target.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(target.permissions);
				DWORD mode = fd.dwReserved0;
				fillFindData(fd, fromUtf8(n), target, isDir);
				fd.dwReserved0 = mode;  // keep the link's own mode so TC shows it as a link
			}
		}
	}
	return true;
}

static HANDLE findFirst(const WCHAR* path, WIN32_FIND_DATAW* fd) {
	PluginPath p = splitPath(path);
	auto* st = new FindState;
	if (p.root) {
		addRootItem(st, QUICK_CONNECTION);
		addRootItem(st, NEW_CONNECTION);
		for (auto& c : listConnections())
			addRootItem(st, c);
	} else {
		bool ok = withSession(p.name, [&](Session& s) { return listRemote(s, p.remote, st); }, false);
		if (!ok) {
			delete st;
			SetLastError(ERROR_PATH_NOT_FOUND);
			return INVALID_HANDLE_VALUE;
		}
	}
	if (st->items.empty()) {
		delete st;
		SetLastError(ERROR_NO_MORE_FILES);
		return INVALID_HANDLE_VALUE;
	}
	*fd = st->items[0];
	st->next = 1;
	return (HANDLE)st;
}

// ---------------------------------------------------------------- connections

static bool editConnection(ConnConfig& c, bool isNew) {
	std::wstring title = isNew ? L"New SFTP connection" : L"Edit connection " + c.name;
	if (isNew) {
		if (!ask(RT_Other, title, L"Connection name:", c.name, 200) || c.name.empty())
			return false;
		if (c.name.find_first_of(L"\\/[]") != std::wstring::npos || c.name[0] == L'<') {
			message(title, L"The name can't contain \\ / [ ] or start with <.");
			return false;
		}
	}
	std::wstring url = c.host;
	if (!c.user.empty())
		url = c.user + L"@" + url;
	if (c.port != 22 && !url.empty())
		url += L":" + std::to_wstring(c.port);
	if (!ask(RT_URL, title, L"Server (user@host:port):", url, 512))
		return false;
	ConnConfig parsed = c;
	parsed.user.clear();
	parsed.port = 22;
	std::wstring savedDir = c.initialDir;
	if (!parseUrl(url, parsed)) {
		message(title, L"Invalid server address.");
		return false;
	}
	if (parsed.initialDir.empty())
		parsed.initialDir = savedDir;
	if (!ask(RT_Other, title, L"Private key file (leave empty to use Pageant, the OpenSSH agent, ~/.ssh keys or a password):", parsed.keyFile, MAX_PATH))
		return false;
	if (!ask(RT_Other, title, L"Start directory on the server (leave empty for the home directory):", parsed.initialDir, 1024))
		return false;
	c = parsed;
	return true;
}

static int openConnection(const std::wstring& name, WCHAR* remoteName) {
	std::shared_ptr<Session> s = getSession(name);
	if (!s)
		return FS_EXEC_ERROR;
	std::string start = s->cfg.initialDir.empty() ? s->home : toUtf8(s->cfg.initialDir);
	copyW(remoteName, remoteToPluginPath(name, start), MAX_PATH);
	return FS_EXEC_SYMLINK;
}

static int executeRoot(HWND, WCHAR* remoteName, const std::wstring& item, const std::wstring& verb) {
	bool open = lstrcmpiW(verb.c_str(), L"open") == 0;
	bool props = lstrcmpiW(verb.c_str(), L"properties") == 0;
	if (item.empty()) {
		if (props)
			message(PLUGIN_NAME, PLUGIN_NAME L" " PLUGIN_VERSION L"\nOpen-source SFTP plugin with pipelined transfers.\nSettings: " + iniPath());
		return FS_EXEC_OK;
	}
	if (item == NEW_CONNECTION) {
		if (!open)
			return FS_EXEC_OK;
		ConnConfig c;
		if (!editConnection(c, true))
			return FS_EXEC_OK;
		ConnConfig existing;
		if (loadConnection(c.name, existing) && !confirm(PLUGIN_NAME, L"Connection " + c.name + L" already exists. Replace it?"))
			return FS_EXEC_OK;
		saveConnection(c);
		return openConnection(c.name, remoteName);
	}
	if (item == QUICK_CONNECTION) {
		if (!open)
			return FS_EXEC_OK;
		std::wstring url;
		if (!ask(RT_URL, L"Quick connection", L"Server (user@host:port/path):", url, 512))
			return FS_EXEC_OK;
		ConnConfig c;
		c.name = QUICK_NAME;
		if (!parseUrl(url, c)) {
			message(PLUGIN_NAME, L"Invalid server address.");
			return FS_EXEC_OK;
		}
		dropSession(QUICK_NAME);
		setQuickConnection(c);
		return openConnection(QUICK_NAME, remoteName);
	}
	ConnConfig c;
	if (!loadConnection(item, c))
		return FS_EXEC_ERROR;
	if (props) {
		if (editConnection(c, false)) {
			saveConnection(c);
			dropSession(item);
		}
		return FS_EXEC_OK;
	}
	return openConnection(item, remoteName);
}

// ---------------------------------------------------------------- transfers

// Set when a transfer failed after data started flowing, so reconnecting and resuming makes sense.
static thread_local bool t_resumable = false;

static void logTransferError(Session& s, const wchar_t* what, const std::wstring& name, long long done, int code) {
	unsigned long status = s.sftp ? libssh2_sftp_last_error(s.sftp) : 0;
	logMsg(MSGTYPE_IMPORTANTERROR, std::wstring(what) + L" of " + name + L" failed after " + std::to_wstring(done) +
		L" bytes: " + lastErrorText(s.ssh, s.sftp) + L" (code " + std::to_wstring(code) + L", sftp status " +
		std::to_wstring(status) + L")");
	// Server-side refusals won't go away by reconnecting.
	t_resumable = !(code == LIBSSH2_ERROR_SFTP_PROTOCOL &&
		(status == LIBSSH2_FX_PERMISSION_DENIED || status == LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM ||
			status == LIBSSH2_FX_QUOTA_EXCEEDED || status == LIBSSH2_FX_WRITE_PROTECT));
}

// Runs a transfer and, when the connection drops in the middle, reconnects
// and continues from where the data ended instead of failing the copy.
template <class F>
static int transferWithResume(const std::wstring& name, int flags, int connectError, F transfer) {
	std::shared_ptr<Session> s = getSession(name);
	if (!s)
		return connectError;
	std::lock_guard<std::mutex> g(s->lock);
	const int maxRetries = iniInt(L"TransferRetries", 5);
	for (int attempt = 0;; attempt++) {
		if (!s->alive() && !s->connect(true)) {
			logMsg(MSGTYPE_IMPORTANTERROR, L"Reconnect to " + s->cfg.host + L" failed");
			return connectError;
		}
		t_resumable = false;
		int rc = transfer(*s, flags);
		if (rc == FS_FILE_OK || rc == FS_FILE_USERABORT || rc == FS_FILE_EXISTSRESUMEALLOWED)
			return rc;
		bool retry = t_resumable || s->connectionLost();
		// The session may be half-broken after a failed write; start the next attempt on a fresh one.
		s->close();
		if (!retry || attempt >= maxRetries) {
			logMsg(MSGTYPE_IMPORTANTERROR, L"Giving up on " + std::wstring(retry ? L"transfer after retries" : L"transfer") +
				L", result " + std::to_wstring(rc));
			return rc;
		}
		logMsg(MSGTYPE_IMPORTANTERROR, L"Reconnecting and resuming (attempt " + std::to_wstring(attempt + 1) + L")");
		Sleep(2000);
		flags = (flags & FS_COPYFLAGS_MOVE) | FS_COPYFLAGS_RESUME;
	}
}

static int downloadFile(Session& s, const std::string& remote, const std::wstring& local, int flags, RemoteInfoStruct* ri,
	const std::wstring& remoteW) {
	bool resume = (flags & FS_COPYFLAGS_RESUME) != 0;
	bool overwrite = (flags & FS_COPYFLAGS_OVERWRITE) != 0;
	if (!resume && !overwrite && GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES)
		return FS_FILE_EXISTSRESUMEALLOWED;

	LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open(s.sftp, remote.c_str(), LIBSSH2_FXF_READ, 0);
	if (!h)
		return s.connectionLost() ? FS_FILE_READERROR : FS_FILE_NOTFOUND;
	long long total = -1;
	if (ri && ri->SizeHigh != 0xFFFFFFFF)
		total = ((long long)ri->SizeHigh << 32) | ri->SizeLow;
	HANDLE f = CreateFileW(local.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, resume ? OPEN_ALWAYS : CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		libssh2_sftp_close(h);
		return FS_FILE_WRITEERROR;
	}
	long long done = 0;
	if (resume) {
		LARGE_INTEGER size;
		GetFileSizeEx(f, &size);
		done = size.QuadPart;
		LARGE_INTEGER zero = {};
		SetFilePointerEx(f, zero, nullptr, FILE_END);
		libssh2_sftp_seek64(h, (libssh2_uint64_t)done);
	}
	std::vector<char> buf(transferWindow());
	int result = FS_FILE_OK;
	if (progress(remoteW, local, percentOf(done, total)))
		result = FS_FILE_USERABORT;
	while (result == FS_FILE_OK) {
		ssize_t n = libssh2_sftp_read(h, buf.data(), buf.size());
		if (n == 0)
			break;
		if (n < 0) {
			logTransferError(s, L"Download", remoteW, done, (int)n);
			result = FS_FILE_READERROR;
			break;
		}
		DWORD written = 0;
		if (!WriteFile(f, buf.data(), (DWORD)n, &written, nullptr) || written != (DWORD)n) {
			result = FS_FILE_WRITEERROR;
			break;
		}
		done += n;
		if (progress(remoteW, local, percentOf(done, total)))
			result = FS_FILE_USERABORT;
	}
	libssh2_sftp_close(h);
	if (result == FS_FILE_OK && ri && (ri->LastWriteTime.dwHighDateTime != 0xFFFFFFFF))
		SetFileTime(f, nullptr, nullptr, &ri->LastWriteTime);
	CloseHandle(f);
	if (result == FS_FILE_OK) {
		progress(remoteW, local, 100);
		if (flags & FS_COPYFLAGS_MOVE)
			libssh2_sftp_unlink(s.sftp, remote.c_str());
	}
	return result;
}

// Upload keeping up to transferWindow() bytes in flight: libssh2 sends the
// whole buffer as pipelined write requests and returns as soon as the first
// ones are acknowledged, so we slide the window forward and top it up instead
// of waiting for every block to drain.
static int uploadFile(Session& s, const std::wstring& local, const std::string& remote, int flags, const std::wstring& remoteW) {
	HANDLE f = CreateFileW(local.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return FS_FILE_NOTFOUND;
	LARGE_INTEGER size;
	GetFileSizeEx(f, &size);
	long long total = size.QuadPart;
	bool resume = (flags & FS_COPYFLAGS_RESUME) != 0;
	bool overwrite = (flags & FS_COPYFLAGS_OVERWRITE) != 0;
	long mode = iniOctal(L"FileMode", 0644);

	unsigned long openFlags = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT;
	if (resume)
		openFlags = LIBSSH2_FXF_WRITE;
	else if (overwrite)
		openFlags |= LIBSSH2_FXF_TRUNC;
	else
		openFlags |= LIBSSH2_FXF_EXCL;  // fails if the file exists, saving a separate stat
	LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open(s.sftp, remote.c_str(), openFlags, mode);
	if (!h) {
		CloseHandle(f);
		if (!resume && !overwrite && !s.connectionLost()) {
			LIBSSH2_SFTP_ATTRIBUTES a;
			if (remoteStat(s, remote, a))
				return FS_FILE_EXISTSRESUMEALLOWED;
		}
		return FS_FILE_WRITEERROR;
	}
	long long done = 0;
	if (resume) {
		LIBSSH2_SFTP_ATTRIBUTES a;
		if (libssh2_sftp_fstat(h, &a) == 0 && (a.flags & LIBSSH2_SFTP_ATTR_SIZE))
			done = (long long)a.filesize;
		if (done > total)
			done = 0;
		LARGE_INTEGER pos;
		pos.QuadPart = done;
		SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
		libssh2_sftp_seek64(h, (libssh2_uint64_t)done);
	}
	size_t window = transferWindow();
	std::vector<char> buf(window);
	size_t head = 0, fill = 0;
	bool eof = false;
	int result = FS_FILE_OK;
	if (progress(local, remoteW, percentOf(done, total)))
		result = FS_FILE_USERABORT;
	while (result == FS_FILE_OK) {
		if (!eof && fill < window) {
			if (head > 0) {
				memmove(buf.data(), buf.data() + head, fill);
				head = 0;
			}
			DWORD got = 0;
			if (!ReadFile(f, buf.data() + fill, (DWORD)(window - fill), &got, nullptr)) {
				logMsg(MSGTYPE_IMPORTANTERROR, L"Reading local file " + local + L" failed, Windows error " + std::to_wstring(GetLastError()));
				result = FS_FILE_READERROR;
				break;
			}
			if (got == 0)
				eof = true;
			fill += got;
		}
		if (fill == 0)
			break;
		ssize_t n = libssh2_sftp_write(h, buf.data() + head, fill);
		if (n < 0) {
			logTransferError(s, L"Upload", remoteW, done, (int)n);
			result = FS_FILE_WRITEERROR;
			break;
		}
		head += (size_t)n;
		fill -= (size_t)n;
		done += n;
		if (progress(local, remoteW, percentOf(done, total)))
			result = FS_FILE_USERABORT;
	}
	CloseHandle(f);
	if (libssh2_sftp_close(h) != 0 && result == FS_FILE_OK) {
		logTransferError(s, L"Closing upload", remoteW, done, libssh2_session_last_errno(s.ssh));
		result = FS_FILE_WRITEERROR;
	}
	if (result == FS_FILE_OK) {
		progress(local, remoteW, 100);
		if (flags & FS_COPYFLAGS_MOVE)
			DeleteFileW(local.c_str());
	}
	return result;
}

static std::string runCommand(Session& s, const std::string& command, int* exitCode) {
	std::string out;
	LIBSSH2_CHANNEL* ch = libssh2_channel_open_session(s.ssh);
	if (!ch) {
		if (exitCode)
			*exitCode = -1;
		return "Cannot open a shell channel: " + toUtf8(lastErrorText(s.ssh, nullptr));
	}
	// stderr is merged into stdout by the shell, so a single blocking read loop can't stall on a full stderr window
	std::string full = "( " + command + " ) 2>&1";
	if (libssh2_channel_exec(ch, full.c_str()) == 0) {
		char buf[4096];
		ssize_t n;
		while ((n = libssh2_channel_read(ch, buf, sizeof(buf))) > 0)
			out.append(buf, n);
	}
	libssh2_channel_close(ch);
	if (exitCode)
		*exitCode = libssh2_channel_get_exit_status(ch);
	libssh2_channel_free(ch);
	return out;
}

// ================================================================ exports

extern "C" {

__declspec(dllexport) int __stdcall FsInitW(int nr, tProgressProcW progress, tLogProcW log, tRequestProcW request) {
	g_host.pluginNr = nr;
	g_host.progress = progress;
	g_host.log = log;
	g_host.request = request;
	return 0;
}

// The ANSI entry points exist only so that Total Commander loads the plugin; it calls the W versions.
__declspec(dllexport) int __stdcall FsInit(int nr, tProgressProc, tLogProc, tRequestProc) {
	g_host.pluginNr = nr;
	return 0;
}

__declspec(dllexport) void __stdcall FsSetCryptCallbackW(tCryptProcW proc, int cryptoNr, int) {
	g_host.crypt = proc;
	g_host.cryptoNr = cryptoNr;
}

__declspec(dllexport) void __stdcall FsSetDefaultParams(FsDefaultParamStruct* dps) {
	std::wstring ini = fromAnsi(dps->DefaultIniName);
	size_t pos = ini.find_last_of(L"\\/");
	g_host.iniDir = pos == std::wstring::npos ? L"" : ini.substr(0, pos + 1);
}

__declspec(dllexport) void __stdcall FsGetDefRootName(char* name, int maxlen) {
	lstrcpynA(name, "tc-sftp", maxlen);
}

__declspec(dllexport) HANDLE __stdcall FsFindFirstW(WCHAR* path, WIN32_FIND_DATAW* fd) {
	return findFirst(path, fd);
}

__declspec(dllexport) BOOL __stdcall FsFindNextW(HANDLE h, WIN32_FIND_DATAW* fd) {
	auto* st = (FindState*)h;
	if (!st || st->next >= st->items.size())
		return FALSE;
	*fd = st->items[st->next++];
	return TRUE;
}

__declspec(dllexport) HANDLE __stdcall FsFindFirst(char* path, WIN32_FIND_DATAA* fd) {
	WIN32_FIND_DATAW w;
	std::wstring p = fromAnsi(path);
	HANDLE h = findFirst(p.c_str(), &w);
	if (h != INVALID_HANDLE_VALUE) {
		memcpy(fd, &w, offsetof(WIN32_FIND_DATAA, cFileName));
		toAnsi(w.cFileName, fd->cFileName, MAX_PATH);
		fd->cAlternateFileName[0] = 0;
	}
	return h;
}

__declspec(dllexport) BOOL __stdcall FsFindNext(HANDLE h, WIN32_FIND_DATAA* fd) {
	WIN32_FIND_DATAW w;
	if (!FsFindNextW(h, &w))
		return FALSE;
	memcpy(fd, &w, offsetof(WIN32_FIND_DATAA, cFileName));
	toAnsi(w.cFileName, fd->cFileName, MAX_PATH);
	fd->cAlternateFileName[0] = 0;
	return TRUE;
}

__declspec(dllexport) int __stdcall FsFindClose(HANDLE h) {
	delete (FindState*)h;
	return 0;
}

__declspec(dllexport) BOOL __stdcall FsMkDirW(WCHAR* path) {
	PluginPath p = splitPath(path);
	if (p.root || p.connRoot)
		return FALSE;
	long mode = iniOctal(L"DirMode", 0755);
	return withSession(p.name, [&](Session& s) { return libssh2_sftp_mkdir(s.sftp, p.remote.c_str(), mode) == 0; }, false);
}

__declspec(dllexport) BOOL __stdcall FsRemoveDirW(WCHAR* path) {
	PluginPath p = splitPath(path);
	if (p.root || p.connRoot)
		return FALSE;
	return withSession(p.name, [&](Session& s) { return libssh2_sftp_rmdir(s.sftp, p.remote.c_str()) == 0; }, false);
}

__declspec(dllexport) BOOL __stdcall FsDeleteFileW(WCHAR* path) {
	PluginPath p = splitPath(path);
	if (p.root)
		return FALSE;
	if (p.connRoot) {  // deleting a saved connection
		if (p.name == NEW_CONNECTION || p.name == QUICK_CONNECTION)
			return FALSE;
		dropSession(p.name);
		std::wstring n = p.name;
		if (g_host.crypt)
			g_host.crypt(g_host.pluginNr, g_host.cryptoNr, FS_CRYPT_DELETE_PASSWORD, n.data(), nullptr, 0);
		deleteConnection(p.name);
		return TRUE;
	}
	return withSession(p.name, [&](Session& s) { return libssh2_sftp_unlink(s.sftp, p.remote.c_str()) == 0; }, false);
}

__declspec(dllexport) int __stdcall FsRenMovFileW(WCHAR* oldName, WCHAR* newName, BOOL move, BOOL overwrite, RemoteInfoStruct*) {
	PluginPath a = splitPath(oldName), b = splitPath(newName);
	if (a.root || b.root)
		return FS_FILE_NOTSUPPORTED;
	if (a.connRoot && b.connRoot) {  // renaming or copying a saved connection
		ConnConfig c;
		if (!loadConnection(a.name, c))
			return FS_FILE_NOTFOUND;
		ConnConfig existing;
		if (!overwrite && loadConnection(b.name, existing))
			return FS_FILE_EXISTS;
		c.name = b.name;
		saveConnection(c);
		if (g_host.crypt) {
			std::wstring from = a.name, to = b.name;
			g_host.crypt(g_host.pluginNr, g_host.cryptoNr, move ? FS_CRYPT_MOVE_PASSWORD : FS_CRYPT_COPY_PASSWORD, from.data(), to.data(), 0);
		}
		if (move) {
			dropSession(a.name);
			deleteConnection(a.name);
		}
		return FS_FILE_OK;
	}
	if (a.name != b.name || a.connRoot || b.connRoot)
		return FS_FILE_NOTSUPPORTED;
	std::wstring src = oldName, dst = newName;
	progress(src, dst, 0);
	int rc = withSession(a.name, [&](Session& s) -> int {
		LIBSSH2_SFTP_ATTRIBUTES attrs;
		if (!overwrite && remoteStat(s, b.remote, attrs, false))
			return FS_FILE_EXISTS;
		if (move) {
			if (libssh2_sftp_rename_ex(s.sftp, a.remote.c_str(), (unsigned)a.remote.size(), b.remote.c_str(), (unsigned)b.remote.size(),
					LIBSSH2_SFTP_RENAME_OVERWRITE | LIBSSH2_SFTP_RENAME_ATOMIC | LIBSSH2_SFTP_RENAME_NATIVE) == 0)
				return FS_FILE_OK;
			if (s.connectionLost())
				return FS_FILE_WRITEERROR;
			// SFTP v3 rename refuses to replace an existing file; try the OpenSSH extension, then delete first.
			if (overwrite) {
				if (libssh2_sftp_posix_rename_ex(s.sftp, a.remote.c_str(), a.remote.size(), b.remote.c_str(), b.remote.size()) == 0)
					return FS_FILE_OK;
				libssh2_sftp_unlink(s.sftp, b.remote.c_str());
				if (libssh2_sftp_rename(s.sftp, a.remote.c_str(), b.remote.c_str()) == 0)
					return FS_FILE_OK;
			}
			return remoteStat(s, a.remote, attrs, false) ? FS_FILE_WRITEERROR : FS_FILE_NOTFOUND;
		}
		// Copy on the server with cp, which avoids sending the data through this PC.
		int exitCode = -1;
		std::string out = runCommand(s, "cp -p -- " + shellQuote(a.remote) + " " + shellQuote(b.remote), &exitCode);
		if (exitCode != 0) {
			logMsg(MSGTYPE_IMPORTANTERROR, L"Remote copy failed: " + fromUtf8(out));
			return FS_FILE_WRITEERROR;
		}
		return FS_FILE_OK;
	}, FS_FILE_WRITEERROR);
	if (rc == FS_FILE_OK)
		progress(src, dst, 100);
	return rc;
}

__declspec(dllexport) int __stdcall FsGetFileW(WCHAR* remoteName, WCHAR* localName, int flags, RemoteInfoStruct* ri) {
	PluginPath p = splitPath(remoteName);
	if (p.root || p.connRoot)
		return FS_FILE_NOTSUPPORTED;
	std::wstring local = localName, remoteW = remoteName;
	return transferWithResume(p.name, flags, FS_FILE_READERROR,
		[&](Session& s, int f) { return downloadFile(s, p.remote, local, f, ri, remoteW); });
}

__declspec(dllexport) int __stdcall FsPutFileW(WCHAR* localName, WCHAR* remoteName, int flags) {
	PluginPath p = splitPath(remoteName);
	if (p.root || p.connRoot)
		return FS_FILE_NOTSUPPORTED;
	std::wstring local = localName, remoteW = remoteName;
	return transferWithResume(p.name, flags, FS_FILE_WRITEERROR,
		[&](Session& s, int f) { return uploadFile(s, local, p.remote, f, remoteW); });
}

__declspec(dllexport) BOOL __stdcall FsSetTimeW(WCHAR* path, FILETIME* creation, FILETIME* access, FILETIME* write) {
	PluginPath p = splitPath(path);
	if (p.root || p.connRoot || !write)
		return FALSE;
	return withSession(p.name, [&](Session& s) {
		LIBSSH2_SFTP_ATTRIBUTES a = {};
		a.flags = LIBSSH2_SFTP_ATTR_ACMODTIME;
		a.mtime = fileTimeToUnix(*write);
		a.atime = access ? fileTimeToUnix(*access) : a.mtime;
		return libssh2_sftp_setstat(s.sftp, p.remote.c_str(), &a) == 0;
	}, false);
	(void)creation;
}

__declspec(dllexport) BOOL __stdcall FsSetAttrW(WCHAR*, int) {
	return FALSE;  // Windows attributes don't map to SFTP; use chmod instead
}

__declspec(dllexport) int __stdcall FsExecuteFileW(HWND mainWin, WCHAR* remoteName, WCHAR* verb) {
	PluginPath p = splitPath(remoteName);
	std::wstring v = verb;
	if (p.root || p.connRoot)
		return executeRoot(mainWin, remoteName, p.name, v);

	if (lstrcmpiW(v.c_str(), L"open") == 0)
		return FS_EXEC_YOURSELF;

	if (_wcsnicmp(v.c_str(), L"chmod ", 6) == 0) {
		long mode = wcstol(v.c_str() + 6, nullptr, 8);
		bool ok = withSession(p.name, [&](Session& s) {
			LIBSSH2_SFTP_ATTRIBUTES a = {};
			a.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
			a.permissions = (unsigned long)mode;
			return libssh2_sftp_setstat(s.sftp, p.remote.c_str(), &a) == 0;
		}, false);
		return ok ? FS_EXEC_OK : FS_EXEC_ERROR;
	}

	if (_wcsnicmp(v.c_str(), L"quote ", 6) == 0) {
		std::string cmd = toUtf8(v.substr(6));
		std::string out;
		int exitCode = -1;
		bool ok = withSession(p.name, [&](Session& s) {
			out = runCommand(s, "cd " + shellQuote(p.remote) + " && " + cmd, &exitCode);
			return exitCode >= 0;
		}, false);
		if (!ok)
			return FS_EXEC_ERROR;
		std::wstring text = fromUtf8(out);
		if (text.size() > 3000)
			text = text.substr(0, 3000) + L"\n...";
		logMsg(MSGTYPE_DETAILS, fromUtf8(cmd) + L"\n" + text);
		message(L"Command output (exit code " + std::to_wstring(exitCode) + L")", text.empty() ? L"(no output)" : text);
		return FS_EXEC_OK;
	}

	if (lstrcmpiW(v.c_str(), L"properties") == 0) {
		LIBSSH2_SFTP_ATTRIBUTES a = {};
		bool ok = withSession(p.name, [&](Session& s) { return remoteStat(s, p.remote, a, false); }, false);
		if (!ok)
			return FS_EXEC_ERROR;
		wchar_t text[512];
		swprintf(text, 512, L"%ls\n\nSize: %llu bytes\nPermissions: %lo\nOwner/group: %lu/%lu", fromUtf8(p.remote).c_str(),
			(unsigned long long)a.filesize, a.permissions & 07777, a.uid, a.gid);
		message(PLUGIN_NAME, text);
		return FS_EXEC_OK;
	}
	return FS_EXEC_ERROR;
}

__declspec(dllexport) BOOL __stdcall FsDisconnectW(WCHAR* root) {
	PluginPath p = splitPath(root);
	if (p.name.empty())
		return TRUE;
	dropSession(p.name);
	logMsg(MSGTYPE_DISCONNECT, L"DISCONNECT \\" + p.name);
	return TRUE;
}

__declspec(dllexport) void __stdcall FsStatusInfoW(WCHAR*, int startEnd, int operation) {
	if (operation == FS_STATUS_OP_GET_MULTI_THREAD || operation == FS_STATUS_OP_PUT_MULTI_THREAD) {
		if (startEnd == FS_STATUS_START) {
			t_background = true;
		} else {
			dropThreadSessions();
			t_background = false;
		}
	}
}

__declspec(dllexport) int __stdcall FsGetBackgroundFlags(void) {
	return BG_DOWNLOAD | BG_UPLOAD | BG_ASK_USER;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
		libssh2_init(0);
	}
	return TRUE;
}
