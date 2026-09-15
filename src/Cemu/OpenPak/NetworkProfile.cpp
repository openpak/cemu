#include "Cemu/OpenPak/NetworkProfile.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <curl/curl.h>
#include <fmt/core.h>
#include <rapidjson/document.h>

#include "config/ActiveSettings.h"
#include "Cemu/Logging/CemuLogging.h"
#include "config/NetworkSettings.h"

namespace
{
constexpr const char* kPlatform = "wiiu";
// Two seconds is generous for a profile that must never hold a launch open.
constexpr long kTimeoutMs = 2000;

std::mutex g_mutex;
std::map<std::string, std::string> g_applied; // id -> url, from the last validated profile
std::string g_source = "built-in";
int g_version = -1;

std::filesystem::path ProfilePath() { return ActiveSettings::GetConfigPath("openpak_network_profile.json"); }
std::filesystem::path EtagPath() { return ActiveSettings::GetConfigPath("openpak_network_profile.etag"); }

std::string ReadFile(std::filesystem::path const& path)
{
	std::error_code ec;
	if (!std::filesystem::exists(path, ec))
		return {};
	std::FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return {};
	std::string out;
	char buffer[4096];
	size_t n;
	while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
		out.append(buffer, n);
	std::fclose(f);
	return out;
}

bool WriteFile(std::filesystem::path const& path, std::string const& contents)
{
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	std::FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(contents.data(), 1, contents.size(), f) == contents.size();
	std::fclose(f);
	return ok;
}

std::string ToLower(std::string s)
{
	for (char& c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

// The compiled-in ceiling (emulators/prds/emulator-network-profile-prd.md §3): the domain families Cemu
// will ever address. The profile chooses within it; anything outside rejects the whole
// profile. A name sits inside a family on a label boundary, so "a.nintendo.net" is in and
// "evila.nintendo.net.evil.example" is not.
const std::array<const char*, 7>& AllowedFamilies()
{
	static const std::array<const char*, 7> families{
		".nintendo.net", ".nintendo.com",        ".nintendo.co.jp", ".nintendowifi.net",
		".nintendo-europe.com", ".gamespy.com",  ".openpak.org",
	};
	return families;
}

bool NameAllowed(const std::string& host)
{
	const std::string name = ToLower(host);
	if (name.empty() || name.front() == '.' || name.find('/') != std::string::npos ||
		name.find('@') != std::string::npos)
		return false;
	for (const char* family : AllowedFamilies())
	{
		const size_t n = std::strlen(family);
		if (name.size() > n && name.compare(name.size() - n, n, family) == 0)
			return true;
	}
	return false;
}

// A literal IPv4/IPv6 address that is safe to point traffic at: never a name, never loopback,
// link-local, unspecified or multicast.
bool AddressUsable(const std::string& literal)
{
	in_addr v4{};
	in6_addr v6{};
	if (inet_pton(AF_INET, literal.c_str(), &v4) == 1)
	{
		const uint32_t ip = ntohl(v4.s_addr);
		return (ip >> 24) != 127 && (ip >> 24) != 0 && (ip >> 16) != 0xA9FE &&
			   ((ip >> 24) & 0xF0) != 0xE0 && ip != 0xFFFFFFFF;
	}
	if (inet_pton(AF_INET6, literal.c_str(), &v6) == 1)
	{
		if (IN6_IS_ADDR_LOOPBACK(&v6) || IN6_IS_ADDR_LINKLOCAL(&v6) || IN6_IS_ADDR_UNSPECIFIED(&v6) ||
			IN6_IS_ADDR_MULTICAST(&v6))
			return false;
		// ::ffff:x.y.z.w counts as its IPv4 self
		static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
		if (std::memcmp(v6.s6_addr, mapped, sizeof(mapped)) == 0)
			return AddressUsable(fmt::format("{}.{}.{}.{}", v6.s6_addr[12], v6.s6_addr[13],
				v6.s6_addr[14], v6.s6_addr[15]));
		return true;
	}
	return false;
}

// scheme://host[:port]/... for the URLs the validation rules look at.
bool SplitURL(const std::string& url, std::string& scheme, std::string& host)
{
	const auto scheme_end = url.find("://");
	if (scheme_end == std::string::npos || scheme_end == 0)
		return false;
	scheme = ToLower(url.substr(0, scheme_end));
	std::string rest = url.substr(scheme_end + 3);
	const auto path = rest.find_first_of("/?#");
	if (path != std::string::npos)
		rest = rest.substr(0, path);
	if (rest.empty() || rest.find('@') != std::string::npos)
		return false;
	if (!rest.empty() && rest.front() == '[')
	{
		const auto close = rest.find(']');
		if (close == std::string::npos)
			return false;
		host = rest.substr(1, close - 1);
		return !host.empty();
	}
	const auto colon = rest.rfind(':');
	host = colon == std::string::npos ? rest : rest.substr(0, colon);
	return !host.empty();
}

size_t WriteBody(char* ptr, size_t size, size_t nmemb, void* userdata)
{
	static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
	return size * nmemb;
}

size_t WriteHeader(char* ptr, size_t size, size_t nmemb, void* userdata)
{
	auto* etag = static_cast<std::string*>(userdata);
	std::string line(ptr, size * nmemb);
	const auto colon = line.find(':');
	if (colon != std::string::npos && ToLower(line.substr(0, colon)) == "etag")
	{
		*etag = line.substr(colon + 1);
		while (!etag->empty() && (etag->front() == ' ' || etag->front() == '\r' || etag->front() == '\n'))
			etag->erase(etag->begin());
		while (!etag->empty() && (etag->back() == ' ' || etag->back() == '\r' || etag->back() == '\n'))
			etag->pop_back();
	}
	return size * nmemb;
}

// One conditional GET. Returns the HTTP status (0 when the network failed), the body and the
// response ETag. Two seconds, one attempt, no redirects: a profile endpoint has no business
// moving the goalposts mid-request.
long FetchOnce(std::string const& etag_sent, std::string& body, std::string& etag_out)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
	CURL* curl = curl_easy_init();
	if (!curl)
		return 0;

	// OPENPAK_API is honoured the same way napi's servers are: https, or loopback for a
	// local stack. TLS verification is never switched off — the profile arrives from a
	// publicly certified openpak.org or not at all.
	std::string base = "https://openpak.org";
	if (const char* env = getenv("OPENPAK_API"))
	{
		if (std::string candidate = ToLower(env);
			candidate.rfind("https://", 0) == 0 || candidate.rfind("http://127.0.0.1", 0) == 0 ||
			candidate.rfind("http://localhost", 0) == 0 || candidate.rfind("http://[::1]", 0) == 0)
			base = env;
	}

	std::string url = base + "/api/v1/network/profile?platform=" + kPlatform;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kTimeoutMs);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kTimeoutMs);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBody);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteHeader);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &etag_out);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "cemu-openpak-network-profile");
	curl_slist* headers = nullptr;
	if (!etag_sent.empty())
		headers = curl_slist_append(headers, ("If-None-Match: " + etag_sent).c_str());
	if (headers)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	const CURLcode code = curl_easy_perform(curl);
	long status = 0;
	if (code == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	if (headers)
		curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return status;
}

// Applies services[] by id. A missing id keeps its compiled-in value rather than emptying.
void Apply(const rapidjson::Document& doc)
{
	std::map<std::string, std::string> applied;
	if (doc.HasMember("services") && doc["services"].IsArray())
	{
		for (auto const& entry : doc["services"].GetArray())
		{
			if (!entry.IsObject() || !entry.HasMember("id") || !entry["id"].IsString() ||
				!entry.HasMember("url") || !entry["url"].IsString())
				continue;
			applied.emplace(entry["id"].GetString(), entry["url"].GetString());
		}
	}
	std::lock_guard lock(g_mutex);
	g_applied = std::move(applied);
	g_version = doc.HasMember("version") && doc["version"].IsInt() ? doc["version"].GetInt() : -1;
}

std::string Validate(const rapidjson::Document& doc)
{
	if (!doc.IsObject())
		return "profile is not an object";
	if (!doc.HasMember("version") || !doc["version"].IsInt() || doc["version"].GetInt() < 0)
		return "version is missing or negative";
	if (!doc.HasMember("platform") || !doc["platform"].IsString() ||
		std::string(doc["platform"].GetString()) != kPlatform)
		return fmt::format("platform is not \"{}\"", kPlatform);

	if (doc.HasMember("server") && doc["server"].IsObject())
	{
		auto const& server = doc["server"];
		if (!server.HasMember("address") || !server["address"].IsString() ||
			!AddressUsable(server["address"].GetString()))
			return "server.address is not a usable literal address";
		if (server.HasMember("https_port") &&
			(!server["https_port"].IsInt() || server["https_port"].GetInt() < 1 ||
			 server["https_port"].GetInt() > 65535))
			return "server.https_port is out of range";
	}

	for (const char* field : {"suffixes", "exact", "never"})
	{
		if (!doc.HasMember("redirect") || !doc["redirect"].IsObject() ||
			!doc["redirect"].HasMember(field))
			continue;
		auto const& list = doc["redirect"][field];
		if (!list.IsArray())
			return fmt::format("redirect.{} is not an array", field);
		for (auto const& entry : list.GetArray())
		{
			if (!entry.IsString() || !NameAllowed(entry.GetString()))
				return fmt::format("redirect.{} names \"{}\", outside the families Cemu addresses",
					field, entry.IsString() ? entry.GetString() : "(non-string)");
		}
	}

	if (doc.HasMember("services") && doc["services"].IsArray())
	{
		for (auto const& entry : doc["services"].GetArray())
		{
			if (!entry.IsObject() || !entry.HasMember("id") || !entry["id"].IsString() ||
				!entry.HasMember("url") || !entry["url"].IsString())
				return "services contains a malformed entry";
			std::string scheme, host;
			if (!SplitURL(entry["url"].GetString(), scheme, host))
				return fmt::format("service {} has an unparsable url", entry["id"].GetString());
			if (!NameAllowed(host))
				return fmt::format("service {} points at \"{}\", outside the families Cemu addresses",
					entry["id"].GetString(), host);
		}
	}
	return {};
}

} // namespace

namespace OpenPakNetworkProfile
{

void ApplyAtLaunch()
{
	Refresh();
}

void Refresh()
{
	const std::string etag_sent = ReadFile(EtagPath());
	std::string body, etag;
	const long status = FetchOnce(etag_sent, body, etag);

	if (status == 304)
	{
		const std::string stored = ReadFile(ProfilePath());
		rapidjson::Document doc;
		doc.Parse(stored.c_str());
		if (!stored.empty() && !doc.HasParseError() && Validate(doc).empty())
		{
			Apply(doc);
			std::lock_guard lock(g_mutex);
			g_source = "cached";
			cemuLog_log(LogType::Force, fmt::format("network profile: cached v{} in use (unchanged at the server)", g_version));
			return;
		}
		// An ETag with no body behind it cannot be applied; drop it so the next launch
		// gets the full profile instead of a lifetime of 304s.
		std::error_code ec;
		std::filesystem::remove(EtagPath(), ec);
	}

	if (status != 200)
	{
		const std::string stored = ReadFile(ProfilePath());
		rapidjson::Document doc;
		doc.Parse(stored.c_str());
		if (!stored.empty() && !doc.HasParseError() && Validate(doc).empty())
		{
			Apply(doc);
			std::lock_guard lock(g_mutex);
			g_source = "cached";
			cemuLog_log(LogType::Force, fmt::format("network profile: cached v{} in use (HTTP {})", g_version, status));
			return;
		}
		std::lock_guard lock(g_mutex);
		g_source = "built-in";
		g_version = -1;
		cemuLog_log(LogType::Force, fmt::format("network profile: built-in service URLs in use (HTTP {})", status));
		return;
	}

	rapidjson::Document doc;
	doc.Parse(body.c_str());
	const std::string problem = doc.HasParseError() ? fmt::format("not valid JSON ({})", doc.GetParseError())
													: Validate(doc);
	if (!problem.empty())
	{
		// Rejected whole, never partially applied.
		const std::string stored = ReadFile(ProfilePath());
		rapidjson::Document previous;
		previous.Parse(stored.c_str());
		if (!stored.empty() && !previous.HasParseError() && Validate(previous).empty())
		{
			Apply(previous);
			std::lock_guard lock(g_mutex);
			g_source = "cached";
			cemuLog_log(LogType::Force, fmt::format("network profile: rejected the fetched profile ({}); cached v{} in use", problem, g_version));
			return;
		}
		std::lock_guard lock(g_mutex);
		g_source = "built-in";
		g_version = -1;
		cemuLog_log(LogType::Force, fmt::format("network profile: rejected the fetched profile ({}); built-in service URLs in use", problem));
		return;
	}

	WriteFile(ProfilePath(), body);
	WriteFile(EtagPath(), etag);
	Apply(doc);
	std::lock_guard lock(g_mutex);
	g_source = "fetched";
	cemuLog_log(LogType::Force, fmt::format("network profile: fetched v{} for {}", g_version, kPlatform));
}

std::string ServiceURL(std::string_view id)
{
	{
		std::lock_guard lock(g_mutex);
		if (auto const it = g_applied.find(std::string{id}); it != g_applied.end())
			return it->second;
	}
	// The compiled-in OpenPakURLs stay the fallback: a profile that stops naming a service
	// does not empty it.
	if (id == "act") return OpenPakURLs::ACTURL;
	if (id == "ecs") return OpenPakURLs::ECSURL;
	if (id == "nus") return OpenPakURLs::NUSURL;
	if (id == "ias") return OpenPakURLs::IASURL;
	if (id == "ccsu") return OpenPakURLs::CCSUURL;
	if (id == "ccs") return OpenPakURLs::CCSURL;
	if (id == "idbe") return OpenPakURLs::IDBEURL;
	if (id == "boss") return OpenPakURLs::BOSSURL;
	if (id == "tagaya") return OpenPakURLs::TAGAYAURL;
	if (id == "olv") return OpenPakURLs::OLVURL;
	return {};
}

std::string GetSource()
{
	std::lock_guard lock(g_mutex);
	return g_source;
}

int GetVersion()
{
	std::lock_guard lock(g_mutex);
	return g_version;
}

} // namespace OpenPakNetworkProfile
