////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_update.cpp
//	Description : Anomaly Co-op: updates from the game, in place of CoopLauncher's updater. The newest release of
//	              TRUMEN228/anomaly-coop carries AnomalyCoop.zip (engine, mod, steam_api64.dll laid out as the
//	              game folder); it is downloaded to coop_update\ and unpacked over the game once the game has quit.
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "coop_update.h"

#include <urlmon.h>
#include <wininet.h>
#include <thread>
#include <mutex>
#include <string>

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "wininet.lib")

namespace coop_update
{
namespace
{
const char* LATEST = "https://api.github.com/repos/TRUMEN228/anomaly-coop/releases/latest";
const char* ASSET = "AnomalyCoop.zip";

std::mutex m;
std::string state = "idle"; // idle, checking, none, available|ver, downloading|ver, ready|ver, error|text
std::string zip_url, latest;
bool busy = false;

std::string root()
{
	string_path p;
	FS.update_path(p, "$fs_root$", "");
	return p;
}

std::string read_file(const std::string& path)
{
	std::string out;
	if (FILE* f = fopen(path.c_str(), "rb"))
	{
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			out.append(buf, n);
		fclose(f);
	}
	return out;
}

// the value of "key": "..." after a position in the JSON (enough for GitHub's flat release fields)
std::string json_string(const std::string& json, const std::string& key, size_t from = 0, size_t* at = nullptr)
{
	size_t k = json.find("\"" + key + "\"", from);
	if (k == std::string::npos)
		return "";
	size_t q = json.find('"', json.find(':', k) + 1);
	size_t e = json.find('"', q + 1);
	if (q == std::string::npos || e == std::string::npos)
		return "";
	if (at)
		*at = e;
	return json.substr(q + 1, e - q - 1);
}

bool newer(const std::string& theirs, const std::string& ours)
{
	int a1 = 0, a2 = 0, a3 = 0, b1 = 0, b2 = 0, b3 = 0;
	sscanf(theirs.c_str(), "%d.%d.%d", &a1, &a2, &a3);
	sscanf(ours.c_str(), "%d.%d.%d", &b1, &b2, &b3);
	if (a1 != b1)
		return a1 > b1;
	if (a2 != b2)
		return a2 > b2;
	return a3 > b3;
}

void set(const std::string& s)
{
	std::lock_guard<std::mutex> g(m);
	state = s;
	busy = false;
}
} // namespace

void check(LPCSTR current)
{
	{
		std::lock_guard<std::mutex> g(m);
		if (busy || state.compare(0, 5, "ready") == 0)
			return;
		busy = true;
		state = "checking";
	}
	std::string ours = current ? current : "";
	std::string dir = root();
	std::thread([ours, dir]()
	{
		std::string json_path = dir + "coop_update_latest.json";
		// a fresh copy, not the one Windows cached from the last check
		DeleteUrlCacheEntryA(LATEST);
		if (URLDownloadToFileA(nullptr, LATEST, json_path.c_str(), 0, nullptr) != S_OK)
		{
			set("error|нет связи с GitHub");
			return;
		}
		std::string json = read_file(json_path);
		DeleteFileA(json_path.c_str());
		std::string tag = json_string(json, "tag_name");
		std::string ver = tag.size() > 1 && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
		// the zip's download link (its name comes before its URL in each asset)
		std::string url;
		size_t pos = 0, at = 0;
		for (;;)
		{
			std::string name = json_string(json, "name", pos, &at);
			if (name.empty())
				break;
			pos = at;
			if (name == ASSET)
			{
				url = json_string(json, "browser_download_url", pos, &at);
				break;
			}
		}
		std::lock_guard<std::mutex> g(m);
		busy = false;
		if (ver.empty())
			state = "error|не удалось прочитать версию";
		else if (!newer(ver, ours) || url.empty())
			state = "none";
		else
		{
			zip_url = url;
			latest = ver;
			state = "available|" + ver;
		}
	}).detach();
}

void download()
{
	std::string url, ver;
	{
		std::lock_guard<std::mutex> g(m);
		if (busy || state.compare(0, 9, "available") != 0)
			return;
		busy = true;
		url = zip_url;
		ver = latest;
		state = "downloading|" + ver;
	}
	std::string dir = root() + "coop_update\\";
	std::thread([url, ver, dir]()
	{
		CreateDirectoryA(dir.c_str(), nullptr);
		std::string path = dir + ASSET;
		DeleteFileA(path.c_str());
		if (URLDownloadToFileA(nullptr, url.c_str(), path.c_str(), 0, nullptr) != S_OK)
		{
			set("error|архив не скачался");
			return;
		}
		// a zip starts with "PK"
		std::string head = read_file(path).substr(0, 2);
		if (head != "PK")
		{
			DeleteFileA(path.c_str());
			set("error|скачанный архив повреждён");
			return;
		}
		set("ready|" + ver);
	}).detach();
}

bool apply()
{
	{
		std::lock_guard<std::mutex> g(m);
		if (state.compare(0, 5, "ready") != 0)
			return false;
	}
	std::string dir = root();
	std::string zip = dir + "coop_update\\" + ASSET;
	// unpacked once this game has quit (its exe is in use until then), then the download is removed
	string4096 cmd;
	xr_sprintf(cmd,
		"powershell.exe -NoProfile -WindowStyle Hidden -Command \"Wait-Process -Id %u -ErrorAction SilentlyContinue; "
		"Expand-Archive -LiteralPath '%s' -DestinationPath '%s' -Force; Remove-Item -LiteralPath '%scoop_update' -Recurse -Force\"",
		GetCurrentProcessId(), zip.c_str(), dir.c_str(), dir.c_str());
	STARTUPINFOA si = {sizeof(si)};
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, dir.c_str(), &si, &pi))
		return false;
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	Msg("~ coop update: will unpack %s after the game quits", zip.c_str());
	return true;
}

LPCSTR status()
{
	static string256 out;
	std::lock_guard<std::mutex> g(m);
	// the texts here are UTF-8 (this file is), the game shows cp1251
	wchar_t w[256];
	int n = MultiByteToWideChar(CP_UTF8, 0, state.c_str(), -1, w, 256);
	if (n <= 0 || !WideCharToMultiByte(1251, 0, w, -1, out, sizeof(out), nullptr, nullptr))
		xr_strcpy(out, state.c_str());
	return out;
}
} // namespace coop_update
