#include "Cemu/OpenPak/Prefs.h"

#include "config/ActiveSettings.h"

#include <fmt/core.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>

namespace
{
	std::mutex g_mutex;
	bool g_loaded = false;
	std::map<std::string, std::string> g_values;

	constexpr const char* kFile = "openpak_settings.txt";
	constexpr const char* kDefaultWebsite = "https://openpak.org";

	std::filesystem::path FilePath() { return ActiveSettings::GetConfigPath(kFile); }

	void LoadLocked()
	{
		if (g_loaded)
			return;
		g_loaded = true;
		std::ifstream file(FilePath());
		std::string line;
		while (std::getline(file, line))
		{
			const auto eq = line.find('=');
			if (eq == std::string::npos || eq == 0)
				continue;
			g_values[line.substr(0, eq)] = line.substr(eq + 1);
		}
	}

	void SaveLocked()
	{
		std::ofstream file(FilePath(), std::ios::trunc);
		for (const auto& [key, value] : g_values)
			file << key << '=' << value << '\n';
	}

	std::string Get(const char* key, const std::string& fallback = {})
	{
		std::lock_guard lock(g_mutex);
		LoadLocked();
		const auto it = g_values.find(key);
		return it != g_values.end() ? it->second : fallback;
	}

	void Set(const char* key, const std::string& value)
	{
		std::lock_guard lock(g_mutex);
		LoadLocked();
		g_values[key] = value;
		SaveLocked();
	}

	bool GetBool(const char* key, bool fallback)
	{
		const std::string value = Get(key);
		if (value.empty())
			return fallback;
		return value == "1" || value == "true";
	}

	std::string ToLower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		return s;
	}

	// https anywhere, plain http only to this machine (a local stack). The bearer never
	// travels in the clear to anything else.
	bool IsAcceptableBase(const std::string& url)
	{
		const std::string lower = ToLower(url);
		return lower.rfind("https://", 0) == 0 || lower.rfind("http://127.0.0.1", 0) == 0 ||
			   lower.rfind("http://localhost", 0) == 0 || lower.rfind("http://[::1]", 0) == 0;
	}

	std::string TrimSlash(std::string url)
	{
		while (!url.empty() && url.back() == '/')
			url.pop_back();
		return url;
	}
} // namespace

namespace OpenPakPrefs
{
	bool CloudSync() { return GetBool("cloud_sync", true); }
	void SetCloudSync(bool on) { Set("cloud_sync", on ? "1" : "0"); }
	bool Notifications() { return GetBool("notifications", true); }
	void SetNotifications(bool on) { Set("notifications", on ? "1" : "0"); }

	Corner NotificationCorner()
	{
		const std::string value = Get("notification_corner");
		if (value == "1")
			return Corner::BottomLeft;
		if (value == "2")
			return Corner::TopRight;
		if (value == "3")
			return Corner::TopLeft;
		return Corner::BottomRight;
	}

	void SetNotificationCorner(Corner corner) { Set("notification_corner", fmt::format("{}", (int)corner)); }

	std::string Website()
	{
		const std::string value = Get("website");
		return value.empty() ? std::string(kDefaultWebsite) : value;
	}

	void SetWebsite(const std::string& website) { Set("website", TrimSlash(website)); }

	std::string ApiBase()
	{
		if (const char* env = getenv("OPENPAK_API"); env && IsAcceptableBase(env))
			return TrimSlash(env);
		const std::string website = Website();
		if (IsAcceptableBase(website))
			return TrimSlash(website);
		return kDefaultWebsite;
	}

	std::string DeviceName() { return Get("device_name"); }
	void SetDeviceName(const std::string& name) { Set("device_name", name); }

	bool ConnectAsked() { return GetBool("connect_asked", false); }
	void SetConnectAsked() { Set("connect_asked", "1"); }

	uint32_t Slot()
	{
		try
		{
			const std::string value = Get("slot");
			return value.empty() ? 0 : (uint32_t)std::stoul(value, nullptr, 16);
		}
		catch (...)
		{
			return 0;
		}
	}

	void SetSlot(uint32_t persistentId) { Set("slot", fmt::format("{:08x}", persistentId)); }
} // namespace OpenPakPrefs
