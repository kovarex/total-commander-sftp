#include "common.h"
#include <algorithm>
#include <cstdio>

Host g_host;
thread_local bool t_background = false;

// ---------------------------------------------------------------- strings

std::string toUtf8(const std::wstring& s) {
	if (s.empty())
		return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
	std::string out(n, 0);
	WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
	return out;
}

std::wstring fromUtf8(const std::string& s) {
	if (s.empty())
		return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring out(n, 0);
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
	return out;
}

std::wstring fromAnsi(const char* s) {
	if (!s || !*s)
		return {};
	int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
	std::wstring out(n, 0);
	MultiByteToWideChar(CP_ACP, 0, s, -1, out.data(), n);
	out.resize(n - 1);
	return out;
}

void toAnsi(const std::wstring& s, char* out, int maxlen) {
	int n = WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, out, maxlen, nullptr, nullptr);
	if (n == 0 && maxlen > 0)
		out[0] = 0;
	out[maxlen - 1] = 0;
}

void copyW(WCHAR* out, const std::wstring& s, int maxlen) {
	lstrcpynW(out, s.c_str(), maxlen);
}

// Unix seconds <-> FILETIME (100 ns ticks since 1601)
static const unsigned long long EPOCH_DIFF = 116444736000000000ULL;

FILETIME unixToFileTime(unsigned long t) {
	unsigned long long v = (unsigned long long)t * 10000000ULL + EPOCH_DIFF;
	FILETIME ft;
	ft.dwLowDateTime = (DWORD)v;
	ft.dwHighDateTime = (DWORD)(v >> 32);
	return ft;
}

unsigned long fileTimeToUnix(const FILETIME& ft) {
	unsigned long long v = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	if (v < EPOCH_DIFF)
		return 0;
	return (unsigned long)((v - EPOCH_DIFF) / 10000000ULL);
}

std::string shellQuote(const std::string& s) {
	std::string out = "'";
	for (char c : s) {
		if (c == '\'')
			out += "'\\''";
		else
			out += c;
	}
	return out + "'";
}

static const wchar_t* sftpStatusText(unsigned long code) {
	switch (code) {
	case LIBSSH2_FX_EOF: return L"end of file";
	case LIBSSH2_FX_NO_SUCH_FILE: return L"no such file";
	case LIBSSH2_FX_PERMISSION_DENIED: return L"permission denied";
	case LIBSSH2_FX_FAILURE: return L"failure";
	case LIBSSH2_FX_BAD_MESSAGE: return L"bad message";
	case LIBSSH2_FX_NO_CONNECTION: return L"no connection";
	case LIBSSH2_FX_CONNECTION_LOST: return L"connection lost";
	case LIBSSH2_FX_OP_UNSUPPORTED: return L"operation not supported";
	case LIBSSH2_FX_FILE_ALREADY_EXISTS: return L"file already exists";
	case LIBSSH2_FX_WRITE_PROTECT: return L"write protected";
	case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM: return L"no space left";
	case LIBSSH2_FX_QUOTA_EXCEEDED: return L"quota exceeded";
	case LIBSSH2_FX_DIR_NOT_EMPTY: return L"directory not empty";
	case LIBSSH2_FX_NOT_A_DIRECTORY: return L"not a directory";
	default: return L"error";
	}
}

std::wstring lastErrorText(LIBSSH2_SESSION* ssh, LIBSSH2_SFTP* sftp) {
	if (!ssh)
		return L"not connected";
	char* msg = nullptr;
	int err = libssh2_session_last_error(ssh, &msg, nullptr, 0);
	if (err == LIBSSH2_ERROR_SFTP_PROTOCOL && sftp)
		return sftpStatusText(libssh2_sftp_last_error(sftp));
	return fromUtf8(msg ? msg : "unknown error");
}

// ---------------------------------------------------------------- dialogs

// Everything except routine details also goes to tcsftp.log next to the ini,
// because Total Commander's own log window is easy to miss and gone after disconnect.
void logMsg(int type, const std::wstring& text) {
	if (g_host.log) {
		std::wstring t = text;
		g_host.log(g_host.pluginNr, type, t.data());
	}
	if (g_host.iniDir.empty())
		return;
	static std::mutex fileLock;
	std::lock_guard<std::mutex> g(fileLock);
	std::wstring path = g_host.iniDir + L"tcsftp.log";
	WIN32_FILE_ATTRIBUTE_DATA info;
	if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeLow > 1024 * 1024) {
		std::wstring old = path + L".old";
		MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
	}
	if (FILE* f = _wfopen(path.c_str(), L"ab")) {
		SYSTEMTIME st;
		GetLocalTime(&st);
		char stamp[64];
		snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d [%lu] ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
			st.wSecond, GetCurrentThreadId());
		std::string line = stamp + toUtf8(text) + "\r\n";
		fwrite(line.data(), 1, line.size(), f);
		fclose(f);
	}
}

bool ask(int type, const std::wstring& title, const std::wstring& text, std::wstring& value, int maxlen) {
	if (!g_host.request)
		return false;
	std::vector<WCHAR> buf(maxlen + 1, 0);
	lstrcpynW(buf.data(), value.c_str(), maxlen);
	std::wstring t = title, x = text;
	BOOL ok = g_host.request(g_host.pluginNr, type, t.data(), x.empty() ? nullptr : x.data(), buf.data(), maxlen);
	if (ok)
		value = buf.data();
	return ok != FALSE;
}

void message(const std::wstring& title, const std::wstring& text) {
	std::wstring dummy;
	ask(RT_MsgOK, title, text, dummy, 2);
}

bool confirm(const std::wstring& title, const std::wstring& text) {
	std::wstring dummy;
	return ask(RT_MsgYesNo, title, text, dummy, 2);
}

// ---------------------------------------------------------------- config

std::wstring iniPath() { return g_host.iniDir + L"tcsftp.ini"; }

static std::wstring iniGet(const std::wstring& section, const wchar_t* key, const wchar_t* def = L"") {
	WCHAR buf[2048];
	GetPrivateProfileStringW(section.c_str(), key, def, buf, 2048, iniPath().c_str());
	return buf;
}

int iniInt(const wchar_t* key, int def) {
	return GetPrivateProfileIntW(L"General", key, def, iniPath().c_str());
}

long iniOctal(const wchar_t* key, long def) {
	std::wstring v = iniGet(L"General", key);
	return v.empty() ? def : wcstol(v.c_str(), nullptr, 8);
}

std::vector<std::wstring> listConnections() {
	std::vector<WCHAR> buf(65536, 0);
	DWORD n = GetPrivateProfileSectionNamesW(buf.data(), (DWORD)buf.size(), iniPath().c_str());
	std::vector<std::wstring> out;
	for (DWORD i = 0; i < n;) {
		std::wstring name = &buf[i];
		i += (DWORD)name.size() + 1;
		if (!name.empty() && lstrcmpiW(name.c_str(), L"General") != 0)
			out.push_back(name);
	}
	return out;
}

bool loadConnection(const std::wstring& name, ConnConfig& c) {
	c = ConnConfig();
	c.name = name;
	c.host = iniGet(name, L"host");
	if (c.host.empty())
		return false;
	c.port = _wtoi(iniGet(name, L"port", L"22").c_str());
	if (c.port <= 0)
		c.port = 22;
	c.user = iniGet(name, L"user");
	c.keyFile = iniGet(name, L"keyfile");
	c.initialDir = iniGet(name, L"initialdir");
	c.savePassword = iniGet(name, L"savepassword", L"0") == L"1";
	return true;
}

void saveConnection(const ConnConfig& c) {
	std::wstring path = iniPath();
	const wchar_t* ini = path.c_str();
	WritePrivateProfileStringW(c.name.c_str(), L"host", c.host.c_str(), ini);
	WritePrivateProfileStringW(c.name.c_str(), L"port", std::to_wstring(c.port).c_str(), ini);
	WritePrivateProfileStringW(c.name.c_str(), L"user", c.user.c_str(), ini);
	WritePrivateProfileStringW(c.name.c_str(), L"keyfile", c.keyFile.c_str(), ini);
	WritePrivateProfileStringW(c.name.c_str(), L"initialdir", c.initialDir.c_str(), ini);
	WritePrivateProfileStringW(c.name.c_str(), L"savepassword", c.savePassword ? L"1" : L"0", ini);
}

void deleteConnection(const std::wstring& name) {
	WritePrivateProfileStringW(name.c_str(), nullptr, nullptr, iniPath().c_str());
}

// Accepts "user@host:port/path", "sftp://user@host:port/path" or just "host".
bool parseUrl(const std::wstring& urlIn, ConnConfig& c) {
	std::wstring url = urlIn;
	while (!url.empty() && url.front() == L' ')
		url.erase(0, 1);
	while (!url.empty() && url.back() == L' ')
		url.pop_back();
	if (url.compare(0, 7, L"sftp://") == 0)
		url = url.substr(7);
	else if (url.compare(0, 6, L"ssh://") == 0)
		url = url.substr(6);
	size_t slash = url.find(L'/');
	if (slash != std::wstring::npos) {
		c.initialDir = url.substr(slash);
		url = url.substr(0, slash);
	}
	size_t at = url.rfind(L'@');
	if (at != std::wstring::npos) {
		c.user = url.substr(0, at);
		url = url.substr(at + 1);
	}
	if (!url.empty() && url.front() == L'[') {  // [ipv6]:port
		size_t close = url.find(L']');
		if (close == std::wstring::npos)
			return false;
		if (close + 1 < url.size() && url[close + 1] == L':')
			c.port = _wtoi(url.substr(close + 2).c_str());
		url = url.substr(1, close - 1);
	} else if (std::count(url.begin(), url.end(), L':') == 1) {
		size_t colon = url.find(L':');
		c.port = _wtoi(url.substr(colon + 1).c_str());
		url = url.substr(0, colon);
	}
	if (c.port <= 0)
		c.port = 22;
	c.host = url;
	return !c.host.empty();
}

// ---------------------------------------------------------------- sessions

static std::mutex g_sessionsLock;
static std::map<std::wstring, std::shared_ptr<Session>> g_sessions;
static ConnConfig g_quick;
static bool g_haveQuick = false;

void setQuickConnection(const ConnConfig& c) {
	std::lock_guard<std::mutex> g(g_sessionsLock);
	g_quick = c;
	g_quick.transient = true;
	g_haveQuick = true;
}

static std::wstring sessionKey(const std::wstring& name) {
	if (!t_background)
		return name;
	return name + L"#" + std::to_wstring(GetCurrentThreadId());
}

Session::~Session() { close(); }

void Session::close() {
	if (sftp) {
		libssh2_sftp_shutdown(sftp);
		sftp = nullptr;
	}
	if (ssh) {
		libssh2_session_disconnect(ssh, "bye");
		libssh2_session_free(ssh);
		ssh = nullptr;
	}
	if (sock != INVALID_SOCKET) {
		closesocket(sock);
		sock = INVALID_SOCKET;
	}
}

int Session::lastSftpError() const {
	if (!ssh)
		return LIBSSH2_ERROR_SOCKET_DISCONNECT;
	int err = libssh2_session_last_errno(ssh);
	if (err == LIBSSH2_ERROR_SFTP_PROTOCOL && sftp)
		return (int)libssh2_sftp_last_error(sftp);
	return err;
}

// True when the last failure means the connection itself is gone.
bool Session::connectionLost() const {
	if (!ssh)
		return true;
	int err = libssh2_session_last_errno(ssh);
	switch (err) {
	case LIBSSH2_ERROR_SOCKET_SEND:
	case LIBSSH2_ERROR_SOCKET_RECV:
	case LIBSSH2_ERROR_SOCKET_DISCONNECT:
	case LIBSSH2_ERROR_SOCKET_TIMEOUT:
	case LIBSSH2_ERROR_TIMEOUT:
	case LIBSSH2_ERROR_CHANNEL_CLOSED:
	case LIBSSH2_ERROR_CHANNEL_EOF_SENT:
	case LIBSSH2_ERROR_BAD_SOCKET:
		return true;
	case LIBSSH2_ERROR_SFTP_PROTOCOL: {
		unsigned long st = libssh2_sftp_last_error(sftp);
		return st == LIBSSH2_FX_NO_CONNECTION || st == LIBSSH2_FX_CONNECTION_LOST;
	}
	default:
		return false;
	}
}

static SOCKET connectSocket(const std::wstring& host, int port, std::wstring& error) {
	ADDRINFOW hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	ADDRINFOW* res = nullptr;
	std::wstring portStr = std::to_wstring(port);
	if (GetAddrInfoW(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
		error = L"Cannot resolve host name " + host;
		return INVALID_SOCKET;
	}
	SOCKET s = INVALID_SOCKET;
	for (ADDRINFOW* a = res; a; a = a->ai_next) {
		s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (s == INVALID_SOCKET)
			continue;
		u_long nb = 1;
		ioctlsocket(s, FIONBIO, &nb);
		connect(s, a->ai_addr, (int)a->ai_addrlen);
		fd_set w, e;
		FD_ZERO(&w);
		FD_ZERO(&e);
		FD_SET(s, &w);
		FD_SET(s, &e);
		timeval tv = {15, 0};
		bool ok = select(0, nullptr, &w, &e, &tv) == 1 && FD_ISSET(s, &w);
		nb = 0;
		ioctlsocket(s, FIONBIO, &nb);
		if (ok) {
			BOOL one = TRUE;
			setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
			setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char*)&one, sizeof(one));
			break;
		}
		closesocket(s);
		s = INVALID_SOCKET;
	}
	FreeAddrInfoW(res);
	if (s == INVALID_SOCKET)
		error = L"Cannot connect to " + host + L" port " + std::to_wstring(port);
	return s;
}

static std::string base64(const unsigned char* data, size_t len) {
	static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	for (size_t i = 0; i < len; i += 3) {
		unsigned v = data[i] << 16;
		if (i + 1 < len) v |= data[i + 1] << 8;
		if (i + 2 < len) v |= data[i + 2];
		out += tbl[(v >> 18) & 63];
		out += tbl[(v >> 12) & 63];
		if (i + 1 < len) out += tbl[(v >> 6) & 63];
		if (i + 2 < len) out += tbl[v & 63];
	}
	return out;
}

static int knownHostKeyType(int hostkeyType) {
	switch (hostkeyType) {
	case LIBSSH2_HOSTKEY_TYPE_RSA: return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
	case LIBSSH2_HOSTKEY_TYPE_DSS: return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
	case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
	case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
	case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
	case LIBSSH2_HOSTKEY_TYPE_ED25519: return LIBSSH2_KNOWNHOST_KEY_ED25519;
	default: return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
	}
}

// Checks the server key against tcsftp_known_hosts (OpenSSH format) and asks on new or changed keys.
static bool verifyHostKey(Session& s, bool interactive) {
	size_t keyLen = 0;
	int keyType = 0;
	const char* key = libssh2_session_hostkey(s.ssh, &keyLen, &keyType);
	if (!key)
		return false;
	const char* sha = libssh2_hostkey_hash(s.ssh, LIBSSH2_HOSTKEY_HASH_SHA256);
	std::wstring fp = sha ? L"SHA256:" + fromUtf8(base64((const unsigned char*)sha, 32)) : L"(unknown)";

	std::wstring fileW = g_host.iniDir + L"tcsftp_known_hosts";
	LIBSSH2_KNOWNHOSTS* kh = libssh2_knownhost_init(s.ssh);
	if (!kh)
		return false;
	// libssh2 opens the file with fopen(), so read it ourselves to support non-ASCII paths.
	std::string content;
	if (FILE* f = _wfopen(fileW.c_str(), L"rb")) {
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			content.append(buf, n);
		fclose(f);
	}
	size_t pos = 0;
	while (pos < content.size()) {
		size_t end = content.find('\n', pos);
		if (end == std::string::npos)
			end = content.size();
		std::string line = content.substr(pos, end - pos);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (!line.empty() && line[0] != '#')
			libssh2_knownhost_readline(kh, line.c_str(), line.size(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
		pos = end + 1;
	}

	std::string host = toUtf8(s.cfg.host);
	struct libssh2_knownhost* found = nullptr;
	int typeMask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | knownHostKeyType(keyType);
	int check = libssh2_knownhost_checkp(kh, host.c_str(), s.cfg.port, key, keyLen, typeMask, &found);
	bool ok = false;
	if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH) {
		ok = true;
	} else if (interactive && (check == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND || check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH)) {
		std::wstring text;
		if (check == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND)
			text = L"First connection to " + s.cfg.host + L".\nServer key fingerprint:\n" + fp + L"\n\nTrust this server and remember its key?";
		else
			text = L"WARNING: the server key of " + s.cfg.host + L" has CHANGED!\nThis can mean someone is intercepting the connection.\n\nNew fingerprint:\n" + fp + L"\n\nAccept the new key anyway?";
		if (confirm(PLUGIN_NAME L" - host key", text)) {
			if (found)
				libssh2_knownhost_del(kh, found);
			std::string entryHost = s.cfg.port == 22 ? host : "[" + host + "]:" + std::to_string(s.cfg.port);
			libssh2_knownhost_addc(kh, entryHost.c_str(), nullptr, key, keyLen, nullptr, 0,
				LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | knownHostKeyType(keyType), nullptr);
			// write the whole list back
			std::string out;
			struct libssh2_knownhost* it = nullptr;
			while (libssh2_knownhost_get(kh, &it, it) == 0) {
				char line[8192];
				size_t outLen = 0;
				if (libssh2_knownhost_writeline(kh, it, line, sizeof(line), &outLen, LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0)
					out.append(line, outLen);
			}
			if (FILE* f = _wfopen(fileW.c_str(), L"wb")) {
				fwrite(out.data(), 1, out.size(), f);
				fclose(f);
			}
			ok = true;
		}
	}
	libssh2_knownhost_free(kh);
	return ok;
}

static std::string readFileBytes(const std::wstring& path) {
	std::string data;
	if (FILE* f = _wfopen(path.c_str(), L"rb")) {
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			data.append(buf, n);
		fclose(f);
	}
	return data;
}

static bool tryKeyFile(Session& s, const std::string& user, const std::wstring& path, bool interactive, bool askPassphrase) {
	std::string priv = readFileBytes(path);
	if (priv.empty())
		return false;
	int rc = libssh2_userauth_publickey_frommemory(s.ssh, user.c_str(), user.size(), nullptr, 0, priv.data(), priv.size(), "");
	if (rc == 0)
		return true;
	if (!interactive || !askPassphrase)
		return false;
	for (int attempt = 0; attempt < 3; attempt++) {
		std::wstring pass;
		if (!ask(RT_Password, PLUGIN_NAME, L"Passphrase for key " + path + L":", pass, 512))
			return false;
		std::string p = toUtf8(pass);
		rc = libssh2_userauth_publickey_frommemory(s.ssh, user.c_str(), user.size(), nullptr, 0, priv.data(), priv.size(), p.c_str());
		SecureZeroMemory(p.data(), p.size());
		if (rc == 0)
			return true;
	}
	return false;
}

static bool tryAgent(Session& s, const std::string& user) {
	LIBSSH2_AGENT* agent = libssh2_agent_init(s.ssh);
	if (!agent)
		return false;
	bool ok = false;
	// Pageant first, then the Windows OpenSSH agent
	const char* paths[] = {nullptr, "\\\\.\\pipe\\openssh-ssh-agent"};
	for (const char* p : paths) {
		if (p)
			libssh2_agent_set_identity_path(agent, p);
		if (libssh2_agent_connect(agent) != 0)
			continue;
		if (libssh2_agent_list_identities(agent) == 0) {
			struct libssh2_agent_publickey* id = nullptr;
			while (libssh2_agent_get_identity(agent, &id, id) == 0) {
				if (libssh2_agent_userauth(agent, user.c_str(), id) == 0) {
					ok = true;
					break;
				}
			}
		}
		libssh2_agent_disconnect(agent);
		if (ok)
			break;
	}
	libssh2_agent_free(agent);
	return ok;
}

static thread_local const std::string* t_kbdPassword = nullptr;

static void kbdCallback(const char*, int, const char*, int, int numPrompts, const LIBSSH2_USERAUTH_KBDINT_PROMPT*,
	LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses, void**) {
	for (int i = 0; i < numPrompts; i++) {
		if (t_kbdPassword) {
			responses[i].text = (char*)malloc(t_kbdPassword->size() + 1);
			memcpy(responses[i].text, t_kbdPassword->c_str(), t_kbdPassword->size() + 1);
			responses[i].length = (unsigned int)t_kbdPassword->size();
		}
	}
}

static bool tryPassword(Session& s, const std::string& user, const std::string& methods, const std::wstring& password) {
	std::string p = toUtf8(password);
	bool ok = false;
	if (methods.find("password") != std::string::npos)
		ok = libssh2_userauth_password(s.ssh, user.c_str(), p.c_str()) == 0;
	if (!ok && methods.find("keyboard-interactive") != std::string::npos) {
		t_kbdPassword = &p;
		ok = libssh2_userauth_keyboard_interactive(s.ssh, user.c_str(), kbdCallback) == 0;
		t_kbdPassword = nullptr;
	}
	SecureZeroMemory(p.data(), p.size());
	return ok;
}

static bool authenticate(Session& s, bool interactive) {
	if (s.cfg.user.empty()) {
		if (!interactive || !ask(RT_UserName, PLUGIN_NAME, L"User name for " + s.cfg.host + L":", s.cfg.user, 256) || s.cfg.user.empty())
			return false;
		if (!s.cfg.transient)
			saveConnection(s.cfg);
	}
	std::string user = toUtf8(s.cfg.user);
	char* list = libssh2_userauth_list(s.ssh, user.c_str(), (unsigned)user.size());
	if (!list)
		return libssh2_userauth_authenticated(s.ssh) != 0;
	std::string methods = list;
	bool pubkey = methods.find("publickey") != std::string::npos;

	if (pubkey && !s.cfg.keyFile.empty() && tryKeyFile(s, user, s.cfg.keyFile, interactive, true))
		return true;
	if (pubkey && s.cfg.keyFile.empty()) {
		if (tryAgent(s, user))
			return true;
		WCHAR profile[MAX_PATH];
		if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) {
			for (const wchar_t* k : {L"id_ed25519", L"id_ecdsa", L"id_rsa"}) {
				std::wstring path = std::wstring(profile) + L"\\.ssh\\" + k;
				if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && tryKeyFile(s, user, path, interactive, false))
					return true;
			}
		}
	}
	if (methods.find("password") == std::string::npos && methods.find("keyboard-interactive") == std::string::npos)
		return false;

	if (!s.password.empty() && tryPassword(s, user, methods, s.password))
		return true;
	std::wstring cryptName = s.cfg.name;
	if (s.cfg.savePassword && g_host.crypt && !s.cfg.transient) {
		WCHAR buf[1024] = {0};
		if (g_host.crypt(g_host.pluginNr, g_host.cryptoNr, FS_CRYPT_LOAD_PASSWORD, cryptName.data(), buf, 1023) == FS_FILE_OK) {
			std::wstring saved = buf;
			SecureZeroMemory(buf, sizeof(buf));
			if (tryPassword(s, user, methods, saved)) {
				s.password = saved;
				return true;
			}
		}
	}
	if (!interactive)
		return false;
	for (int attempt = 0; attempt < 3; attempt++) {
		std::wstring pass;
		if (!ask(RT_Password, PLUGIN_NAME, L"Password for " + s.cfg.user + L"@" + s.cfg.host + L":", pass, 512))
			return false;
		if (tryPassword(s, user, methods, pass)) {
			s.password = pass;
			if (!s.cfg.transient && g_host.crypt && !s.cfg.savePassword &&
				confirm(PLUGIN_NAME, L"Save the password in Total Commander's encrypted password store?")) {
				if (g_host.crypt(g_host.pluginNr, g_host.cryptoNr, FS_CRYPT_SAVE_PASSWORD, cryptName.data(), pass.data(), 0) == FS_FILE_OK) {
					s.cfg.savePassword = true;
					saveConnection(s.cfg);
				}
			}
			return true;
		}
	}
	return false;
}

bool Session::connect(bool interactive) {
	close();
	std::wstring error;
	logMsg(MSGTYPE_DETAILS, L"Connecting to " + cfg.host + L":" + std::to_wstring(cfg.port));
	sock = connectSocket(cfg.host, cfg.port, error);
	if (sock == INVALID_SOCKET) {
		if (interactive)
			message(PLUGIN_NAME, error);
		return false;
	}
	ssh = libssh2_session_init();
	libssh2_session_set_blocking(ssh, 1);
	libssh2_session_set_timeout(ssh, iniInt(L"TimeoutSeconds", 60) * 1000);
	if (libssh2_session_handshake(ssh, (libssh2_socket_t)sock) != 0) {
		if (interactive)
			message(PLUGIN_NAME, L"SSH handshake failed: " + lastErrorText(ssh, nullptr));
		close();
		return false;
	}
	if (!verifyHostKey(*this, interactive)) {
		close();
		return false;
	}
	if (!authenticate(*this, interactive)) {
		if (interactive)
			message(PLUGIN_NAME, L"Login to " + cfg.host + L" failed.");
		close();
		return false;
	}
	libssh2_keepalive_config(ssh, 1, 30);
	sftp = libssh2_sftp_init(ssh);
	if (!sftp) {
		if (interactive)
			message(PLUGIN_NAME, L"The server refused the SFTP subsystem: " + lastErrorText(ssh, nullptr));
		close();
		return false;
	}
	char buf[4096];
	int n = libssh2_sftp_realpath(sftp, ".", buf, sizeof(buf) - 1);
	home = n > 0 ? std::string(buf, n) : "/";
	logMsg(MSGTYPE_CONNECTCOMPLETE, L"Connected to " + cfg.host);
	return true;
}

std::shared_ptr<Session> findSession(const std::wstring& name) {
	std::lock_guard<std::mutex> g(g_sessionsLock);
	auto it = g_sessions.find(sessionKey(name));
	return it == g_sessions.end() ? nullptr : it->second;
}

std::shared_ptr<Session> getSession(const std::wstring& name, bool connectIfNeeded) {
	std::shared_ptr<Session> s = findSession(name);
	if (s && s->alive())
		return s;
	if (!connectIfNeeded)
		return nullptr;
	if (!s) {
		s = std::make_shared<Session>();
		std::lock_guard<std::mutex> g(g_sessionsLock);
		if (g_haveQuick && name == g_quick.name)
			s->cfg = g_quick;
		else if (!loadConnection(name, s->cfg))
			return nullptr;
		g_sessions[sessionKey(name)] = s;
	}
	std::lock_guard<std::mutex> g(s->lock);
	if (!s->alive()) {
		if (!s->connect(true))
			return nullptr;
		if (!t_background)
			logMsg(MSGTYPE_CONNECT, L"CONNECT \\" + name);
	}
	return s;
}

void dropSession(const std::wstring& name) {
	std::vector<std::shared_ptr<Session>> victims;
	{
		std::lock_guard<std::mutex> g(g_sessionsLock);
		for (auto it = g_sessions.begin(); it != g_sessions.end();) {
			const std::wstring& k = it->first;
			if (k == name || k.compare(0, name.size() + 1, name + L"#") == 0) {
				victims.push_back(it->second);
				it = g_sessions.erase(it);
			} else {
				++it;
			}
		}
	}
	for (auto& v : victims) {
		std::lock_guard<std::mutex> g(v->lock);
		v->close();
	}
}

void dropThreadSessions() {
	std::wstring suffix = L"#" + std::to_wstring(GetCurrentThreadId());
	std::vector<std::shared_ptr<Session>> victims;
	{
		std::lock_guard<std::mutex> g(g_sessionsLock);
		for (auto it = g_sessions.begin(); it != g_sessions.end();) {
			const std::wstring& k = it->first;
			if (k.size() > suffix.size() && k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0) {
				victims.push_back(it->second);
				it = g_sessions.erase(it);
			} else {
				++it;
			}
		}
	}
	for (auto& v : victims) {
		std::lock_guard<std::mutex> g(v->lock);
		v->close();
	}
}
