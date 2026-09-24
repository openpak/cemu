#include "Cemu/OpenPak/Ceiling.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <curl/curl.h>
#include <fmt/core.h>
#include <openssl/evp.h>
#include <rapidjson/document.h>

#include "config/ActiveSettings.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Cemu/ncrypto/ncrypto.h"

namespace
{
constexpr const char* kPlatform = "wiiu";
// Bootstrap rule: always the pinned origin with normal public TLS trust — never the Website
// setting, OPENPAK_API, a URL from the profile or the OpenPak CA.
constexpr const char* kCeilingURL = "https://openpak.org/api/v1/network/ceiling";
constexpr long kTimeoutMs = 2000;
constexpr size_t kMaxBytes = 256 * 1024;

// Pinned Ed25519 public keys. A list, so a future key can be added before the old one retires.
struct PinnedKey
{
	const char* keyid; // hex sha256 of the raw key
	std::array<uint8_t, 32> raw;
};
const std::array<PinnedKey, 1> kPinnedKeys{{
	{"36e8bcdd93c2c1a1d7e5d87bbba05a7a4f97882131cf6878f1c053a25a377fe4",
	 {0x0f, 0xd2, 0xa2, 0x66, 0x08, 0x68, 0xe5, 0x3d, 0x20, 0xfc, 0x81, 0x1e, 0x3f, 0x15, 0x71, 0x0c,
	  0xb1, 0xad, 0x15, 0x7b, 0xa7, 0xda, 0x80, 0x19, 0xf7, 0xa5, 0x9e, 0x8a, 0xd6, 0x04, 0xd8, 0x7c}},
}};

// The compiled-in first-boot fallback (emulators/prds/emulator-network-profile-prd.md §3), used
// only while no verified ceiling has ever been held. Never updated again: new families arrive
// in the signed ceiling.
const std::vector<std::string>& CompiledFallback()
{
	static const std::vector<std::string> families{
		".nintendo.net", ".nintendo.com",        ".nintendo.co.jp", ".nintendowifi.net",
		".nintendo-europe.com", ".gamespy.com",  ".openpak.org",
	};
	return families;
}

std::mutex g_updateMutex; // one Update at a time
std::mutex g_mutex;       // the state below
bool g_cacheLooked = false;
bool g_have = false;      // a verified ceiling is held
int64_t g_version = 0;
int64_t g_maxVersion = 0; // highest version ever accepted (rollback protection)
std::vector<std::string> g_families;
std::string g_lastNote;

std::filesystem::path CachePath() { return ActiveSettings::GetConfigPath("openpak_network_ceiling.json"); }
std::filesystem::path VersionPath() { return ActiveSettings::GetConfigPath("openpak_network_ceiling.version"); }

std::string ToLower(std::string s)
{
	for (char& c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

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

// Written to a sibling and renamed over the target, so a crash never leaves half a file; owner
// read/write only where the platform has permissions.
bool WriteAtomic(std::filesystem::path const& path, std::string const& contents)
{
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	std::filesystem::path tmp = path;
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f)
			return false;
		f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
		f.close();
		if (!f)
		{
			std::filesystem::remove(tmp, ec);
			return false;
		}
	}
#ifndef _WIN32
	std::filesystem::permissions(tmp, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
		std::filesystem::perm_options::replace, ec);
#endif
	std::filesystem::rename(tmp, path, ec);
	if (ec)
	{
		std::filesystem::remove(tmp, ec);
		return false;
	}
	return true;
}

// Logs a line only when it differs from the last one, so a repeated failure is logged once.
// Call with g_mutex held.
void NoteLocked(const std::string& line)
{
	if (line == g_lastNote)
		return;
	g_lastNote = line;
	cemuLog_log(LogType::Force, "network ceiling: {}", line);
}

std::string HoldingLocked()
{
	return g_have ? fmt::format("signed v{} stays in use", g_version) : std::string("built-in families stay in use");
}

// ".example.net": a leading dot, lower case, at least two labels of [a-z0-9-] that neither start
// nor end with '-'. A bare public suffix (".com") is refused.
bool FamilyValid(const std::string& family)
{
	if (family.size() < 4 || family.size() > 254 || family.front() != '.')
		return false;
	int labels = 0;
	size_t start = 1;
	while (true)
	{
		const size_t dot = family.find('.', start);
		const size_t end = dot == std::string::npos ? family.size() : dot;
		const size_t len = end - start;
		if (len == 0 || len > 63 || family[start] == '-' || family[end - 1] == '-')
			return false;
		for (size_t i = start; i < end; ++i)
		{
			const char c = family[i];
			if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
				return false;
		}
		++labels;
		if (dot == std::string::npos)
			break;
		start = dot + 1;
	}
	return labels >= 2;
}

bool Ed25519Verify(const std::array<uint8_t, 32>& key, const std::vector<uint8>& sig, const std::vector<uint8>& msg)
{
	EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key.data(), key.size());
	if (!pkey)
		return false;
	EVP_MD_CTX* ctx = EVP_MD_CTX_new();
	const bool ok = ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
					EVP_DigestVerify(ctx, sig.data(), sig.size(), msg.data(), msg.size()) == 1;
	EVP_MD_CTX_free(ctx);
	EVP_PKEY_free(pkey);
	return ok;
}

struct Parsed
{
	int64_t version = 0;
	std::vector<std::string> families; // this platform's
};

// Verifies the envelope and parses the signed payload bytes (never a re-serialisation).
// Returns the reason it is refused, or empty.
std::string VerifyEnvelope(const std::string& bytes, Parsed& out)
{
	rapidjson::Document env;
	env.Parse(bytes.data(), bytes.size());
	if (env.HasParseError() || !env.IsObject())
		return "the envelope is not a JSON object";
	if (!env.HasMember("payload") || !env["payload"].IsString())
		return "the envelope has no payload";
	if (!env.HasMember("signatures") || !env["signatures"].IsArray())
		return "the envelope has no signatures";

	const std::vector<uint8> payload =
		NCrypto::base64Decode(std::string_view(env["payload"].GetString(), env["payload"].GetStringLength()));
	if (payload.empty())
		return "the payload is not base64";

	bool verified = false;
	for (auto const& s : env["signatures"].GetArray())
	{
		if (!s.IsObject() || !s.HasMember("keyid") || !s["keyid"].IsString() || !s.HasMember("sig") ||
			!s["sig"].IsString())
			continue;
		const std::string keyid = ToLower(s["keyid"].GetString());
		for (auto const& key : kPinnedKeys)
		{
			if (keyid != key.keyid)
				continue;
			const std::vector<uint8> sig =
				NCrypto::base64Decode(std::string_view(s["sig"].GetString(), s["sig"].GetStringLength()));
			if (sig.size() == 64 && Ed25519Verify(key.raw, sig, payload))
				verified = true;
		}
		if (verified)
			break;
	}
	if (!verified)
		return "no signature verifies under a pinned key";

	rapidjson::Document doc;
	doc.Parse(reinterpret_cast<const char*>(payload.data()), payload.size());
	if (doc.HasParseError() || !doc.IsObject())
		return "the payload is not a JSON object";
	if (!doc.HasMember("type") || !doc["type"].IsString() || std::string(doc["type"].GetString()) != "openpak-ceiling")
		return "the payload type is not openpak-ceiling";
	if (!doc.HasMember("version") || !doc["version"].IsInt64() || doc["version"].GetInt64() < 1)
		return "the payload version is not a positive integer";
	if (!doc.HasMember("platforms") || !doc["platforms"].IsObject())
		return "the payload has no platforms";

	Parsed parsed;
	parsed.version = doc["version"].GetInt64();
	auto const& platforms = doc["platforms"];
	for (auto it = platforms.MemberBegin(); it != platforms.MemberEnd(); ++it)
	{
		const std::string name(it->name.GetString(), it->name.GetStringLength());
		if (!it->value.IsArray())
			return fmt::format("platform {} is not a list", name);
		for (auto const& family : it->value.GetArray())
		{
			// One malformed entry anywhere refuses the whole file.
			if (!family.IsString() || !FamilyValid(std::string(family.GetString(), family.GetStringLength())))
				return fmt::format("platform {} holds a malformed family", name);
			if (name == kPlatform)
				parsed.families.emplace_back(family.GetString(), family.GetStringLength());
		}
	}
	out = std::move(parsed);
	return {};
}

size_t WriteBody(char* ptr, size_t size, size_t nmemb, void* userdata)
{
	auto* body = static_cast<std::string*>(userdata);
	if (body->size() + size * nmemb > kMaxBytes)
		return 0; // aborts the transfer
	body->append(ptr, size * nmemb);
	return size * nmemb;
}

// One GET, two seconds, no redirects, public TLS trust. Returns the HTTP status, 0 on failure.
long Fetch(std::string& body)
{
	CURL* curl = curl_easy_init();
	if (!curl)
		return 0;
	curl_easy_setopt(curl, CURLOPT_URL, kCeilingURL);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kTimeoutMs);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kTimeoutMs);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBody);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "cemu-openpak-network-ceiling");
	const CURLcode code = curl_easy_perform(curl);
	long status = 0;
	if (code == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	return status;
}

// Rule 1: the cached envelope, re-verified. Call with g_mutex held.
void LoadCacheLocked()
{
	if (g_cacheLooked)
		return;
	g_cacheLooked = true;
	const std::string stored = ReadFile(VersionPath());
	g_maxVersion = stored.empty() ? 0 : std::max<int64_t>(0, std::strtoll(stored.c_str(), nullptr, 10));

	const std::string bytes = ReadFile(CachePath());
	if (bytes.empty())
		return;
	Parsed parsed;
	const std::string problem = VerifyEnvelope(bytes, parsed);
	if (!problem.empty())
	{
		NoteLocked(fmt::format("the cached ceiling is refused ({})", problem));
		return;
	}
	if (parsed.version < g_maxVersion)
	{
		NoteLocked(fmt::format("the cached ceiling v{} is older than the accepted v{}", parsed.version, g_maxVersion));
		return;
	}
	g_have = true;
	g_version = parsed.version;
	g_maxVersion = parsed.version;
	g_families = std::move(parsed.families);
}

} // namespace

namespace OpenPakCeiling
{

void Update()
{
	std::lock_guard update(g_updateMutex);
	{
		std::lock_guard lock(g_mutex);
		LoadCacheLocked();
	}

	std::string body;
	const long status = Fetch(body);
	if (status != 200)
	{
		std::lock_guard lock(g_mutex);
		NoteLocked(fmt::format("fetch failed (HTTP {}); {}", status, HoldingLocked()));
		return;
	}

	Parsed parsed;
	const std::string problem = VerifyEnvelope(body, parsed);
	std::lock_guard lock(g_mutex);
	if (!problem.empty())
	{
		NoteLocked(fmt::format("refused the fetched ceiling ({}); {}", problem, HoldingLocked()));
		return;
	}
	if (parsed.version < g_maxVersion)
	{
		NoteLocked(fmt::format("refused the fetched ceiling v{}: older than the accepted v{}; {}", parsed.version,
			g_maxVersion, HoldingLocked()));
		return;
	}
	// The bytes exactly as received, then the rollback floor: a crash between the two leaves a
	// cache at or above the floor, which the next launch still accepts.
	WriteAtomic(CachePath(), body);
	if (parsed.version > g_maxVersion)
		WriteAtomic(VersionPath(), std::to_string(parsed.version));
	g_have = true;
	g_version = parsed.version;
	g_maxVersion = parsed.version;
	g_families = std::move(parsed.families);
	NoteLocked(fmt::format("signed v{} in use ({} {} families)", g_version, g_families.size(), kPlatform));
}

std::vector<std::string> Families()
{
	std::lock_guard lock(g_mutex);
	return g_have ? g_families : CompiledFallback();
}

bool Inside(const std::string& host, const std::vector<std::string>& families)
{
	std::string name = ToLower(host);
	// A redirect suffix comes in family form; its apex is what has to lie inside.
	if (!name.empty() && name.front() == '.')
		name.erase(0, 1);
	if (name.empty() || name.find("..") != std::string::npos || name.find_first_of("/@:[]% \\") != std::string::npos)
		return false;
	for (const std::string& family : families)
	{
		if (family.size() < 2)
			continue;
		if (name.compare(0, std::string::npos, family, 1, std::string::npos) == 0)
			return true; // the apex
		if (name.size() > family.size() && name.compare(name.size() - family.size(), family.size(), family) == 0)
			return true;
	}
	return false;
}

std::string Source()
{
	std::lock_guard lock(g_mutex);
	return g_have ? fmt::format("signed v{}", g_version) : std::string("built-in");
}

} // namespace OpenPakCeiling
