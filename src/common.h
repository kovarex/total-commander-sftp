#pragma once
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "wfx.h"

#define PLUGIN_NAME L"tc-sftp"
#define PLUGIN_VERSION L"0.4"

// Callbacks and settings handed to us by Total Commander.
struct Host {
	int pluginNr = 0;
	tProgressProcW progress = nullptr;
	tLogProcW log = nullptr;
	tRequestProcW request = nullptr;
	tCryptProcW crypt = nullptr;
	int cryptoNr = 0;
	std::wstring iniDir;  // folder of TC's ini, ends with a backslash
};
extern Host g_host;

// ---- strings and time ----
std::string toUtf8(const std::wstring& s);
std::wstring fromUtf8(const std::string& s);
std::wstring fromAnsi(const char* s);
void toAnsi(const std::wstring& s, char* out, int maxlen);
void copyW(WCHAR* out, const std::wstring& s, int maxlen);
FILETIME unixToFileTime(unsigned long t);
unsigned long fileTimeToUnix(const FILETIME& ft);
std::string shellQuote(const std::string& s);
std::wstring lastErrorText(LIBSSH2_SESSION* ssh, LIBSSH2_SFTP* sftp);

// ---- dialogs and logging ----
void logMsg(int type, const std::wstring& text);
bool ask(int type, const std::wstring& title, const std::wstring& text, std::wstring& value, int maxlen = 1024);
void message(const std::wstring& title, const std::wstring& text);
bool confirm(const std::wstring& title, const std::wstring& text);

// ---- saved connections (tcsftp.ini next to TC's ini) ----
struct ConnConfig {
	std::wstring name;
	std::wstring host;
	int port = 22;
	std::wstring user;
	std::wstring keyFile;      // empty: try agent and default keys
	std::wstring initialDir;   // empty: server's home directory
	bool savePassword = false;
	bool transient = false;    // quick connection, never written to the ini
};
std::wstring iniPath();
std::vector<std::wstring> listConnections();
bool loadConnection(const std::wstring& name, ConnConfig& c);
void saveConnection(const ConnConfig& c);
void deleteConnection(const std::wstring& name);
bool parseUrl(const std::wstring& url, ConnConfig& c);
int iniInt(const wchar_t* key, int def);
long iniOctal(const wchar_t* key, long def);  // permissions, written like 644

// ---- SSH sessions ----
struct Session {
	ConnConfig cfg;
	SOCKET sock = INVALID_SOCKET;
	LIBSSH2_SESSION* ssh = nullptr;
	LIBSSH2_SFTP* sftp = nullptr;
	std::string home;          // remote home directory (UTF-8)
	std::mutex lock;
	std::wstring password;     // kept in memory for reconnects only
	~Session();
	void close();
	bool connect(bool interactive);
	bool alive() const { return sftp != nullptr; }
	int lastSftpError() const;
	bool connectionLost() const;
};

std::shared_ptr<Session> getSession(const std::wstring& name, bool connectIfNeeded = true);
std::shared_ptr<Session> findSession(const std::wstring& name);
void dropSession(const std::wstring& name);
void dropThreadSessions();
void setQuickConnection(const ConnConfig& c);
extern thread_local bool t_background;
