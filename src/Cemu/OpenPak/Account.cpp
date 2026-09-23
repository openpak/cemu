#include "Cemu/OpenPak/Account.h"

#include "Cemu/OpenPak/Errors.h"
#include "Cemu/OpenPak/NetworkProfile.h"
#include "Cemu/OpenPak/Prefs.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "Cafe/Account/Account.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"

#include <curl/curl.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <fmt/core.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
	std::mutex g_mutex;
	bool g_loaded = false;
	std::string g_bearer;
	std::string g_accountId;   // core account uuid
	uint32 g_pid = 0;
	std::string g_username;    // the NNID
	std::string g_miiName;
	std::string g_miiData;     // base64
	std::string g_passwordCache; // base64
	std::string g_country;
	std::string g_language;

	constexpr const char* kSessionFile = "openpak_session.txt";

	std::filesystem::path SessionPath() { return ActiveSettings::GetConfigPath(kSessionFile); }

	std::map<std::string, std::string> ParseKeyValueFile(const std::filesystem::path& path)
	{
		std::map<std::string, std::string> out;
		std::ifstream file(path);
		std::string line;
		while (std::getline(file, line))
		{
			const auto eq = line.find('=');
			if (eq == std::string::npos || eq == 0)
				continue;
			out.emplace(line.substr(0, eq), line.substr(eq + 1));
		}
		return out;
	}

	void EnsureLoaded()
	{
		if (g_loaded)
			return;
		g_loaded = true;
		const auto values = ParseKeyValueFile(SessionPath());
		auto pick = [&values](const char* key) {
			const auto it = values.find(key);
			return it != values.end() ? it->second : std::string{};
		};
		g_bearer = pick("bearer");
		g_accountId = pick("account_id");
		g_username = pick("username");
		g_miiName = pick("mii_name");
		g_miiData = pick("mii_data");
		g_passwordCache = pick("password_cache");
		g_country = pick("country");
		g_language = pick("language");
		try
		{
			g_pid = values.count("pid") ? std::stoul(values.at("pid"), nullptr, 16) : 0;
		}
		catch (...)
		{
			g_pid = 0;
		}
	}

	void SaveSession()
	{
		std::ofstream file(SessionPath(), std::ios::trunc);
		file << "account_id=" << g_accountId << std::endl;
		file << "username=" << g_username << std::endl;
		file << "pid=" << fmt::format("{:08x}", g_pid) << std::endl;
		file << "mii_name=" << g_miiName << std::endl;
		file << "mii_data=" << g_miiData << std::endl;
		file << "password_cache=" << g_passwordCache << std::endl;
		file << "country=" << g_country << std::endl;
		file << "language=" << g_language << std::endl;
		file << "bearer=" << g_bearer << std::endl;
	}

	// The website API base: OPENPAK_API, else the Website setting, https or loopback only.
	// TLS is never switched off here — the bearer only travels to a certified website.
	std::string ApiBase() { return OpenPakPrefs::ApiBase(); }

	size_t WriteBodyCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
	{
		auto* body = (std::string*)userdata;
		body->append(ptr, size * nmemb);
		return size * nmemb;
	}

	struct HttpResponse
	{
		long status = 0;
		std::string body;
		std::string error; // transport failure
	};

	HttpResponse Request(const char* method, const std::string& url, const std::string& body,
						 const std::vector<std::string>& headers)
	{
		HttpResponse out;
		CURL* curl = curl_easy_init();
		if (!curl)
		{
			out.error = "curl init failed";
			return out;
		}
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBodyCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "cemu-openpak-account");
		curl_slist* headerList = nullptr;
		for (auto& h : headers)
			headerList = curl_slist_append(headerList, h.c_str());
		if (!body.empty())
		{
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
			headerList = curl_slist_append(headerList, "Content-Type: application/json");
		}
		if (headerList)
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

		const CURLcode code = curl_easy_perform(curl);
		if (code == CURLE_OK)
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status);
		else
			out.error = curl_easy_strerror(code);
		if (headerList)
			curl_slist_free_all(headerList);
		curl_easy_cleanup(curl);
		return out;
	}

	// The server's own sentence when it sent one; otherwise the HTTP code goes to the log and
	// the UI says the website could not be reached.
	std::string ErrorFrom(const std::string& body, long status)
	{
		rapidjson::Document doc;
		if (!doc.Parse(body.c_str()).HasParseError() && doc.HasMember("error") && doc["error"].IsString() &&
			doc["error"].GetStringLength() > 0)
			return OpenPakError::Server(doc["error"].GetString());
		cemuLog_log(LogType::Force, "OpenPak: the account request failed (HTTP {})", status);
		return OpenPakError::Unreachable;
	}

	std::string TransportError(const std::string& error)
	{
		cemuLog_log(LogType::Force, "OpenPak: could not reach the website: {}", error);
		return OpenPakError::Unreachable;
	}

	std::string JsonString(const rapidjson::Value& doc, const char* field)
	{
		if (doc.HasMember(field) && doc[field].IsString())
			return doc[field].GetString();
		return {};
	}

	uint32 JsonUint(const rapidjson::Value& doc, const char* field)
	{
		if (doc.HasMember(field) && doc[field].IsUint64())
			return (uint32)doc[field].GetUint64();
		return 0;
	}

	std::string ToHex(const uint8* data, size_t len)
	{
		std::string out;
		out.reserve(len * 2);
		char buf[8];
		for (size_t i = 0; i < len; ++i)
		{
			snprintf(buf, sizeof(buf), "%02x", data[i]);
			out += buf;
		}
		return out;
	}

	// NNAS country index from an ISO name, via Cemu's own table.
	uint32 CountryIndex(const std::string& countryName)
	{
		if (!countryName.empty())
		{
			for (sint32 i = 1; i < 512; ++i)
			{
				if (std::string_view(NCrypto::GetCountryAsString(i)) == countryName)
					return (uint32)i;
			}
		}
		return 49; // US, the network profile's default
	}

	std::wstring MiiNameFromUtf8(const std::string& utf8)
	{
		std::wstring out;
		// the server truncates to ten runes; widen the same width account.dat stores
		size_t i = 0;
		while (i < utf8.size() && out.size() < 10)
		{
			// decode one UTF-8 rune
			const auto c = (unsigned char)utf8[i];
			size_t len = 1;
			if (c >= 0xF0) len = 4;
			else if (c >= 0xE0) len = 3;
			else if (c >= 0xC0) len = 2;
			if (i + len > utf8.size())
				break;
			uint32 cp = c & (0xFF >> (len + 1));
			for (size_t k = 1; k < len; ++k)
				cp = (cp << 6) | ((unsigned char)utf8[i + k] & 0x3F);
			out.push_back((wchar_t)cp);
			i += len;
		}
		return out;
	}
} // namespace

namespace OpenPakAccount
{
	SignInResult SignIn(std::string_view email, std::string_view password, std::string_view deviceName)
	{
		SignInResult out;
		EnsureLoaded();

		rapidjson::StringBuffer sb;
		rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
		writer.StartObject();
		writer.Key("email");
		writer.String(email.data(), email.size());
		writer.Key("password");
		writer.String(password.data(), password.size());
		if (!deviceName.empty())
		{
			// how this machine appears in the account's device list (UX spec §3.3)
			writer.Key("device_name");
			writer.String(deviceName.data(), deviceName.size());
		}
		writer.EndObject();

		const auto minted = Request("POST", ApiBase() + "/api/v1/token", sb.GetString(), {});
		if (!minted.error.empty())
		{
			out.error = TransportError(minted.error);
			return out;
		}
		if (minted.status == 401 || minted.status == 403)
		{
			out.error = OpenPakError::Credentials;
			return out;
		}
		if (minted.status == 429)
		{
			out.error = OpenPakError::RateLimited;
			return out;
		}
		if (minted.status != 200 && minted.status != 201)
		{
			out.error = ErrorFrom(minted.body, minted.status);
			return out;
		}
		rapidjson::Document tokenDoc;
		if (tokenDoc.Parse(minted.body.c_str()).HasParseError() || !tokenDoc.HasMember("token") ||
			!tokenDoc["token"].IsString())
		{
			cemuLog_log(LogType::Force, "OpenPak: the sign-in response was not understood");
			out.error = OpenPakError::Unreachable;
			return out;
		}
		const std::string bearer = tokenDoc["token"].GetString();

		// The Wii U identity of the account: minted server-side on first sight.
		const auto identity = Request(
			"GET", ApiBase() + "/api/v1/me/wiiu", {},
			{fmt::format("Authorization: Bearer {}", bearer)});
		if (!identity.error.empty())
		{
			out.error = TransportError(identity.error);
			return out;
		}
		if (identity.status != 200)
		{
			out.error = ErrorFrom(identity.body, identity.status);
			return out;
		}
		rapidjson::Document idDoc;
		if (idDoc.Parse(identity.body.c_str()).HasParseError())
		{
			cemuLog_log(LogType::Force, "OpenPak: the identity response was not understood");
			out.error = OpenPakError::Unreachable;
			return out;
		}
		const uint32 pid = JsonUint(idDoc, "pid");
		const std::string username = JsonString(idDoc, "username");
		const std::string miiData = JsonString(idDoc, "mii_data");
		const std::string passwordCache = JsonString(idDoc, "password_cache");
		if (pid == 0 || username.empty() || miiData.empty() || passwordCache.empty())
		{
			cemuLog_log(LogType::Force, "OpenPak: the account has no Wii U identity; is the Wii U adapter online?");
			out.error = OpenPakError::NoIdentity;
			return out;
		}

		{
			std::lock_guard lock(g_mutex);
			g_bearer = bearer;
			g_accountId = JsonString(idDoc, "account_id");
			g_pid = pid;
			g_username = username;
			g_miiName = JsonString(idDoc, "mii_name");
			g_miiData = miiData;
			g_passwordCache = passwordCache;
			g_country = JsonString(idDoc, "country");
			g_language = JsonString(idDoc, "language");
			SaveSession();
		}
		out.ok = true;
		return out;
	}

	void SignOut()
	{
		EnsureLoaded();
		std::string bearer;
		{
			std::lock_guard lock(g_mutex);
			bearer = g_bearer;
			g_bearer.clear();
			g_accountId.clear();
			g_pid = 0;
			g_username.clear();
			g_miiName.clear();
			g_miiData.clear();
			g_passwordCache.clear();
			g_country.clear();
			g_language.clear();
			SaveSession();
		}
		// End the website session too: best-effort, in the background, so presence ends now
		// without the UI waiting on the network.
		if (!bearer.empty())
		{
			std::thread([bearer, base = ApiBase()]() {
				Request("DELETE", base + "/api/v1/token", {}, {fmt::format("Authorization: Bearer {}", bearer)});
			}).detach();
		}
	}

	bool DropIfRejected()
	{
		const std::string bearer = GetBearer();
		if (bearer.empty())
			return false;
		const auto resp = Request("GET", ApiBase() + "/api/v1/me", {}, {fmt::format("Authorization: Bearer {}", bearer)});
		if (!resp.error.empty() || resp.status != 401)
			return false;
		{
			std::lock_guard lock(g_mutex);
			if (g_bearer != bearer)
				return false; // signed in again meanwhile
			g_bearer.clear();
			SaveSession();
		}
		cemuLog_log(LogType::Force, "OpenPak: the stored sign-in was refused; signed out");
		return true;
	}

	std::string GetMiiName()
	{
		EnsureLoaded();
		std::lock_guard lock(g_mutex);
		return g_miiName;
	}

	uint32_t GetPid()
	{
		EnsureLoaded();
		std::lock_guard lock(g_mutex);
		return g_pid;
	}

	uint32_t AppliedSlot()
	{
		const uint32 slot = OpenPakPrefs::Slot();
		if (slot == 0)
			return 0;
		for (const auto& account : Account::GetAccounts())
		{
			if (account.GetPersistentId() == slot)
				return slot;
		}
		return 0;
	}

	bool IsSignedIn()
	{
		EnsureLoaded();
		std::lock_guard lock(g_mutex);
		return !g_bearer.empty();
	}

	std::string GetUsername()
	{
		EnsureLoaded();
		std::lock_guard lock(g_mutex);
		return g_username;
	}

	std::string GetBearer()
	{
		EnsureLoaded();
		std::lock_guard lock(g_mutex);
		return g_bearer;
	}

	std::string ApplyIdentity()
	{
		EnsureLoaded();
		std::string username, miiName, miiData, passwordCache, country;
		uint32 pid = 0;
		{
			std::lock_guard lock(g_mutex);
			username = g_username;
			pid = g_pid;
			miiName = g_miiName;
			miiData = g_miiData;
			passwordCache = g_passwordCache;
			country = g_country;
		}
		if (pid == 0 || miiData.empty() || passwordCache.empty())
			return "no OpenPak identity to apply; sign in first";

		auto miiBytes = NCrypto::base64Decode(miiData);
		auto cacheBytes = NCrypto::base64Decode(passwordCache);
		if (miiBytes.size() != 96 || cacheBytes.size() != 32)
			return "the stored identity is malformed; sign in again";

		// UX spec §5.10: write into the Mii account OpenPak used before — the remembered slot,
		// else one already holding this PID (an earlier build made a new one per sign-in) — and
		// take a new slot only when there is none.
		Account::RefreshAccounts();
		uint32 persistentId = 0;
		std::array<uint8, 16> uuid{};
		bool haveUuid = false;
		const uint32 remembered = OpenPakPrefs::Slot();
		for (const auto& account : Account::GetAccounts())
		{
			if (remembered != 0 && account.GetPersistentId() == remembered)
			{
				persistentId = remembered;
				uuid = account.GetUuid();
				haveUuid = true;
				break;
			}
		}
		if (persistentId == 0)
		{
			for (const auto& account : Account::GetAccounts())
			{
				if (account.GetPrincipalId() == pid)
				{
					persistentId = account.GetPersistentId();
					uuid = account.GetUuid();
					haveUuid = true;
					break;
				}
			}
		}
		if (persistentId == 0)
		{
			if (!Account::HasFreeAccountSlots())
				return "every Mii account slot is taken; delete one in General settings > Account";
			persistentId = Account::GetNextPersistentId();
		}

		// account.dat in the exact shape Account::ParseFile reads.
		const auto accountDir = ActiveSettings::GetMlcPath(fmt::format(L"usr/save/system/act/{:08x}", persistentId));
		std::error_code ec;
		std::filesystem::create_directories(accountDir, ec);
		if (ec)
			return fmt::format("cannot create the account directory: {}", ec.message());

		if (!haveUuid)
		{
			std::mt19937 rng(std::random_device{}());
			std::uniform_int_distribution<int> dist(0, 255);
			for (auto& b : uuid)
				b = (uint8)dist(rng);
		}

		const std::wstring miiNameWide = MiiNameFromUtf8(miiName);
		std::ostringstream file;
		file << "AccountInstance_20120705" << std::endl;
		file << fmt::format("PersistentId={:08x}", persistentId) << std::endl;
		file << fmt::format("TransferableIdBase={:x}", (0x2000004ULL << 32) | ((uint64)uuid[12] << 24) | ((uint64)uuid[13] << 16) | ((uint64)uuid[14] << 8) | (uint64)uuid[15]) << std::endl;
		file << "Uuid=" << ToHex(uuid.data(), uuid.size()) << std::endl;
		file << "MiiData=" << ToHex(miiBytes.data(), miiBytes.size()) << std::endl;
		file << "MiiName=";
		for (wchar_t c : miiNameWide)
			file << fmt::format("{:04x}", (uint16)c);
		file << std::endl;
		// The NNAS login sends AccountId as the user_id; the adapter resolves it
		// by username, so this is the minted NNID and never the core uuid.
		file << "AccountId=" << username << std::endl;
		file << fmt::format("BirthYear={:x}", 1990) << std::endl;
		file << fmt::format("BirthMonth={:x}", 1) << std::endl;
		file << fmt::format("BirthDay={:x}", 1) << std::endl;
		file << fmt::format("Gender={:x}", 0) << std::endl;
		file << "EmailAddress=" << std::endl;
		file << fmt::format("Country={:x}", CountryIndex(country)) << std::endl;
		file << fmt::format("SimpleAddressId={:x}", 0) << std::endl;
		file << fmt::format("PrincipalId={:x}", pid) << std::endl;
		file << fmt::format("IsPasswordCacheEnabled={:x}", 1) << std::endl;
		file << "AccountPasswordCache=" << ToHex(cacheBytes.data(), cacheBytes.size()) << std::endl;

		{
			std::ofstream out(accountDir / "account.dat", std::ios::trunc);
			if (!out.is_open())
				return "cannot write account.dat";
			out << file.str();
		}

		Account::UpdatePersisidDat();
		OpenPakPrefs::SetSlot(persistentId);

		// Select the account and point it at the OpenPak service.
		GetConfig().account.m_persistent_id = persistentId;
		GetConfig().SetAccountSelectedService(persistentId, NetworkService::OpenPak);
		GetConfigHandle().Save();
		Account::RefreshAccounts();

		cemuLog_log(LogType::Force,
			"OpenPak: applied the identity of {} (PID {:x}) as account {:08x}; the OpenPak service is selected",
			miiName.empty() ? g_username : miiName, pid, persistentId);
		return {};
	}
} // namespace OpenPakAccount
