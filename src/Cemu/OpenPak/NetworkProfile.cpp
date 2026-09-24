#include "Cemu/OpenPak/NetworkProfile.h"
#include "Cemu/OpenPak/Ceiling.h"
#include "Cemu/OpenPak/Prefs.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif
#include <curl/curl.h>
#include <fmt/core.h>
#include <openssl/evp.h>
#include <rapidjson/document.h>

#include "config/ActiveSettings.h"
#include "Cemu/Logging/CemuLogging.h"
#include "config/NetworkSettings.h"

namespace
{
constexpr const char* kPlatform = "wiiu";
// Two seconds is generous for a profile that must never hold a launch open.
constexpr long kTimeoutMs = 2000;

std::mutex g_refreshMutex; // one Refresh at a time (launch, the settings button, the re-check)
std::mutex g_mutex;
std::map<std::string, std::string> g_applied; // id -> url, from the last validated profile
std::string g_source = "built-in";
int g_version = -1;
std::string g_lastDrops; // the dropped names last logged, so a repeat is not logged again

// Change notice (docs/signed-ceiling.md, client rule 4): the digest of the effective set in use,
// every digest a notice was already raised for, and who shows it.
bool g_haveDigest = false;
std::string g_digestInUse;
std::set<std::string> g_notified;
std::function<void()> g_onChange;

// The ids ServiceURL answers; the effective set is their resolved URLs.
constexpr std::array<const char*, 10> kServiceIds{
	"act", "boss", "ccs", "ccsu", "ecs", "ias", "idbe", "nus", "olv", "tagaya",
};

std::filesystem::path ProfilePath() { return ActiveSettings::GetConfigPath("openpak_network_profile.json"); }
std::filesystem::path EtagPath() { return ActiveSettings::GetConfigPath("openpak_network_profile.etag"); }

std::string ReadFile(std::filesystem::path const& path)
{
	std::error_code ec;
	if (!std::filesystem::exists(path, ec))
		return {};
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return {};
	return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool WriteFile(std::filesystem::path const& path, std::string const& contents)
{
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (!f)
		return false;
	f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
	return static_cast<bool>(f);
}

std::string ToLower(std::string s)
{
	for (char& c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
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
	CURL* curl = curl_easy_init();
	if (!curl)
		return 0;

	// OPENPAK_API, else the Website setting (Advanced), https or loopback for a local stack.
	// TLS verification is never switched off — the profile arrives from a certified website
	// or not at all.
	const std::string base = OpenPakPrefs::ApiBase();

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

// Applies services[] by id, through the ceiling for wiiu (docs/signed-ceiling.md): a name outside
// it is dropped on its own and logged, never the whole profile. A dropped or missing id keeps its
// compiled-in value rather than emptying.
void Apply(const rapidjson::Document& doc, const std::vector<std::string>& families)
{
	std::vector<std::string> dropped;

	// Cemu addresses service URLs, not names, so redirect.* changes nothing it does; it is still
	// held to the ceiling so the log shows what a DNS-level client would drop. redirect.never
	// only keeps names off OpenPak and cannot widen anything.
	if (doc.HasMember("redirect") && doc["redirect"].IsObject())
	{
		for (const char* field : {"suffixes", "exact"})
		{
			if (!doc["redirect"].HasMember(field))
				continue;
			for (auto const& entry : doc["redirect"][field].GetArray())
			{
				if (!entry.IsString() || !OpenPakCeiling::Inside(entry.GetString(), families))
					dropped.push_back(fmt::format("redirect.{} \"{}\"", field, entry.IsString() ? entry.GetString() : "(non-string)"));
			}
		}
	}

	std::map<std::string, std::string> applied;
	if (doc.HasMember("services") && doc["services"].IsArray())
	{
		for (auto const& entry : doc["services"].GetArray())
		{
			if (!entry.IsObject() || !entry.HasMember("id") || !entry["id"].IsString() ||
				!entry.HasMember("url") || !entry["url"].IsString())
			{
				dropped.push_back("a malformed services entry");
				continue;
			}
			const std::string id = entry["id"].GetString();
			const std::string url = entry["url"].GetString();
			std::string scheme, host;
			if (!SplitURL(url, scheme, host))
			{
				dropped.push_back(fmt::format("service {} (unparsable url)", id));
				continue;
			}
			if (!OpenPakCeiling::Inside(host, families))
			{
				dropped.push_back(fmt::format("service {} \"{}\"", id, host));
				continue;
			}
			applied.emplace(id, url);
		}
	}

	std::string drops;
	for (const std::string& d : dropped)
		drops += (drops.empty() ? "" : ", ") + d;

	std::lock_guard lock(g_mutex);
	g_applied = std::move(applied);
	g_version = doc.HasMember("version") && doc["version"].IsInt() ? doc["version"].GetInt() : -1;
	if (drops != g_lastDrops)
	{
		g_lastDrops = drops;
		if (!drops.empty())
			cemuLog_log(LogType::Force, "network profile: dropped outside the ceiling ({}): {}", OpenPakCeiling::Source(), drops);
	}
}

// The shape of the profile. Names are not judged here: Apply drops the ones outside the ceiling
// one by one.
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

	if (doc.HasMember("redirect") && !doc["redirect"].IsObject())
		return "redirect is not an object";
	for (const char* field : {"suffixes", "exact", "never"})
	{
		if (!doc.HasMember("redirect") || !doc["redirect"].HasMember(field))
			continue;
		if (!doc["redirect"][field].IsArray())
			return fmt::format("redirect.{} is not an array", field);
	}
	if (doc.HasMember("services") && !doc["services"].IsArray())
		return "services is not an array";
	return {};
}

// sha256 over the sorted effective set: "service:<id>=<url>" for every id Cemu resolves. Only
// wiiu's services go in, so a change to another platform's list never changes it.
std::string EffectiveDigest()
{
	std::vector<std::string> entries;
	for (const char* id : kServiceIds)
		entries.push_back(fmt::format("service:{}={}", id, OpenPakNetworkProfile::ServiceURL(id)));
	std::sort(entries.begin(), entries.end());
	std::string joined;
	for (const std::string& e : entries)
		joined += e + "\n";
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int len = 0;
	if (EVP_Digest(joined.data(), joined.size(), md, &len, EVP_sha256(), nullptr) != 1)
		return joined; // still a faithful identity of the set
	std::string hex;
	for (unsigned int i = 0; i < len; ++i)
		hex += fmt::format("{:02x}", md[i]);
	return hex;
}

// Rule 4: the first Refresh sets the digest in use; a later one that lands on a digest not seen
// before raises the notice once.
void CheckForChange()
{
	const std::string digest = EffectiveDigest();
	std::function<void()> notify;
	{
		std::lock_guard lock(g_mutex);
		if (!g_haveDigest)
		{
			g_haveDigest = true;
			g_digestInUse = digest;
			g_notified.insert(digest);
		}
		else if (digest != g_digestInUse)
		{
			g_digestInUse = digest;
			if (g_notified.insert(digest).second)
				notify = g_onChange;
		}
	}
	if (notify)
	{
		cemuLog_log(LogType::Force, "network profile: the effective wiiu redirects changed");
		notify();
	}
}

// Everything Refresh does after the two fetches; the caller runs CheckForChange afterwards.
void RefreshLocked()
{
	// The signed ceiling from its pinned origin, alongside the profile, so a launch waits for one
	// two-second timeout at most.
	std::future<void> ceiling;
	try
	{
		ceiling = std::async(std::launch::async, OpenPakCeiling::Update);
	}
	catch (const std::exception&)
	{
		OpenPakCeiling::Update();
	}

	const std::string etag_sent = ReadFile(EtagPath());
	std::string body, etag;
	const long status = FetchOnce(etag_sent, body, etag);
	if (ceiling.valid())
		ceiling.wait();
	const std::vector<std::string> families = OpenPakCeiling::Families();

	if (status == 304)
	{
		const std::string stored = ReadFile(ProfilePath());
		rapidjson::Document doc;
		doc.Parse(stored.c_str());
		if (!stored.empty() && !doc.HasParseError() && Validate(doc).empty())
		{
			Apply(doc, families);
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
			Apply(doc, families);
			std::lock_guard lock(g_mutex);
			g_source = "cached";
			cemuLog_log(LogType::Force, fmt::format("network profile: cached v{} in use (HTTP {})", g_version, status));
			return;
		}
		std::lock_guard lock(g_mutex);
		g_applied.clear();
		g_source = "built-in";
		g_version = -1;
		cemuLog_log(LogType::Force, fmt::format("network profile: built-in service URLs in use (HTTP {})", status));
		return;
	}

	rapidjson::Document doc;
	doc.Parse(body.c_str());
	const std::string problem = doc.HasParseError() ? fmt::format("not valid JSON ({})", static_cast<int>(doc.GetParseError()))
													: Validate(doc);
	if (!problem.empty())
	{
		// A profile of the wrong shape is rejected whole; names outside the ceiling never are.
		const std::string stored = ReadFile(ProfilePath());
		rapidjson::Document previous;
		previous.Parse(stored.c_str());
		if (!stored.empty() && !previous.HasParseError() && Validate(previous).empty())
		{
			Apply(previous, families);
			std::lock_guard lock(g_mutex);
			g_source = "cached";
			cemuLog_log(LogType::Force, fmt::format("network profile: rejected the fetched profile ({}); cached v{} in use", problem, g_version));
			return;
		}
		std::lock_guard lock(g_mutex);
		g_applied.clear();
		g_source = "built-in";
		g_version = -1;
		cemuLog_log(LogType::Force, fmt::format("network profile: rejected the fetched profile ({}); built-in service URLs in use", problem));
		return;
	}

	WriteFile(ProfilePath(), body);
	WriteFile(EtagPath(), etag);
	Apply(doc, families);
	std::lock_guard lock(g_mutex);
	g_source = "fetched";
	cemuLog_log(LogType::Force, fmt::format("network profile: fetched v{} for {} (ceiling {})", g_version, kPlatform, OpenPakCeiling::Source()));
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
	{
		std::lock_guard refresh(g_refreshMutex);
		curl_global_init(CURL_GLOBAL_DEFAULT);
		RefreshLocked();
	}
	CheckForChange();
}

void SetChangeListener(std::function<void()> listener)
{
	std::lock_guard lock(g_mutex);
	g_onChange = std::move(listener);
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
