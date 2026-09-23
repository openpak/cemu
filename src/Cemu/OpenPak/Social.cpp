#include "Cemu/OpenPak/Social.h"
#include "Cemu/OpenPak/Account.h"
#include "Cemu/OpenPak/Errors.h"
#include "Cemu/OpenPak/Prefs.h"
#include "Cemu/Logging/CemuLogging.h"

#include <curl/curl.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <fmt/core.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace
{
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
		std::string error;
	};

	HttpResponse Request(const char* method, const std::string& url, const std::string& body,
						 const std::string& bearer, long timeoutSeconds = 15, bool followRedirects = false)
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
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, followRedirects ? 1L : 0L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBodyCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "cemu-openpak-social");
		curl_slist* headers = nullptr;
		if (!bearer.empty())
			headers = curl_slist_append(headers, fmt::format("Authorization: Bearer {}", bearer).c_str());
		if (!body.empty())
		{
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
			headers = curl_slist_append(headers, "Content-Type: application/json");
		}
		if (headers)
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		const CURLcode code = curl_easy_perform(curl);
		if (code == CURLE_OK)
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status);
		else
			out.error = curl_easy_strerror(code);
		if (headers)
			curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
		return out;
	}

	// The error code for a response that is not a success (see Errors.h).
	std::string ErrorFor(const HttpResponse& resp, const char* what)
	{
		if (!resp.error.empty())
		{
			cemuLog_log(LogType::Force, "OpenPak: {}: {}", what, resp.error);
			return OpenPakError::Unreachable;
		}
		if (resp.status == 401)
			return OpenPakError::Expired;
		if (resp.status == 429)
			return OpenPakError::RateLimited;
		rapidjson::Document doc;
		if (!doc.Parse(resp.body.c_str()).HasParseError() && doc.IsObject() && doc.HasMember("error") &&
			doc["error"].IsString() && doc["error"].GetStringLength() > 0)
			return OpenPakError::Server(doc["error"].GetString());
		cemuLog_log(LogType::Force, "OpenPak: {} failed (HTTP {})", what, resp.status);
		return OpenPakError::Unreachable;
	}

	std::string JsonString(const rapidjson::Value& doc, const char* field)
	{
		if (doc.IsObject() && doc.HasMember(field) && doc[field].IsString())
			return doc[field].GetString();
		return {};
	}

	bool JsonBool(const rapidjson::Value& doc, const char* field)
	{
		return doc.IsObject() && doc.HasMember(field) && doc[field].IsBool() && doc[field].GetBool();
	}

	int64_t JsonInt(const rapidjson::Value& doc, const char* field)
	{
		if (doc.IsObject() && doc.HasMember(field) && doc[field].IsInt64())
			return doc[field].GetInt64();
		return 0;
	}

	uint64_t JsonUint(const rapidjson::Value& doc, const char* field)
	{
		if (doc.IsObject() && doc.HasMember(field) && doc[field].IsUint64())
			return doc[field].GetUint64();
		return 0;
	}

	double JsonDouble(const rapidjson::Value& doc, const char* field)
	{
		if (doc.IsObject() && doc.HasMember(field) && doc[field].IsNumber())
			return doc[field].GetDouble();
		return 0;
	}

	std::string ToUpper(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::toupper(c); });
		return s;
	}

	std::string ToLower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		return s;
	}

	// Ids that go into a path: only the shapes the site mints.
	bool IsTameId(const std::string& id)
	{
		return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
			return std::isalnum(c) || c == '-' || c == '_';
		});
	}

	std::string JsonBody(const char* key, const std::string& value)
	{
		rapidjson::StringBuffer sb;
		rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
		writer.StartObject();
		writer.Key(key);
		writer.String(value.c_str(), (rapidjson::SizeType)value.size());
		writer.EndObject();
		return sb.GetString();
	}

	OpenPakSocial::Friend ParseFriend(const rapidjson::Value& entry)
	{
		OpenPakSocial::Friend f;
		if (!entry.IsObject())
			return f;
		f.account_id = JsonString(entry, "account_id");
		f.display_name = JsonString(entry, "display_name");
		f.online = JsonBool(entry, "online");
		if (f.online)
		{
			f.title_id = ToUpper(JsonString(entry, "title_id"));
			f.console = JsonString(entry, "namespace");
			f.since = JsonString(entry, "since");
		}
		return f;
	}

	bool GetBearerOr(std::string& bearer, std::string& error)
	{
		bearer = OpenPakAccount::GetBearer();
		if (bearer.empty())
		{
			error = OpenPakError::NotSignedIn;
			return false;
		}
		return true;
	}

	// GET a JSON document with the bearer; on failure the error is set and false returned.
	bool GetJson(const std::string& path, bool withBearer, rapidjson::Document& doc, std::string& error, const char* what)
	{
		std::string bearer;
		if (withBearer && !GetBearerOr(bearer, error))
			return false;
		const auto resp = Request("GET", ApiBase() + path, {}, bearer);
		if (!resp.error.empty() || resp.status != 200)
		{
			error = ErrorFor(resp, what);
			return false;
		}
		if (doc.Parse(resp.body.c_str()).HasParseError() || !doc.IsObject())
		{
			cemuLog_log(LogType::Force, "OpenPak: {}: the response was not understood", what);
			error = OpenPakError::Unreachable;
			return false;
		}
		return true;
	}

	std::string PostAccount(const std::string& path, const std::string& account_id, const char* what)
	{
		std::string bearer, error;
		if (!GetBearerOr(bearer, error))
			return error;
		const auto resp = Request("POST", ApiBase() + path, JsonBody("account_id", account_id), bearer);
		if (!resp.error.empty() || (resp.status != 200 && resp.status != 201 && resp.status != 204))
			return ErrorFor(resp, what);
		return {};
	}

	std::optional<std::vector<uint8_t>> FetchPublic(const std::string& path)
	{
		const auto resp = Request("GET", ApiBase() + path, {}, {}, 15, true);
		if (!resp.error.empty() || resp.status != 200 || resp.body.empty())
			return std::nullopt;
		return std::vector<uint8_t>(resp.body.begin(), resp.body.end());
	}
} // namespace

namespace OpenPakSocial
{
	FriendList GetFriends()
	{
		FriendList out;
		rapidjson::Document doc;
		if (!GetJson("/api/v1/me/friends", true, doc, out.error, "friends"))
			return out;
		if (doc.HasMember("friends") && doc["friends"].IsArray())
			for (const auto& entry : doc["friends"].GetArray())
				out.friends.push_back(ParseFriend(entry));
		out.ok = true;
		return out;
	}

	RequestList GetRequests()
	{
		RequestList out;
		rapidjson::Document doc;
		if (!GetJson("/api/v1/me/friends/requests", true, doc, out.error, "friend requests"))
			return out;
		if (doc.HasMember("incoming") && doc["incoming"].IsArray())
			for (const auto& entry : doc["incoming"].GetArray())
				out.incoming.push_back(ParseFriend(entry));
		if (doc.HasMember("outgoing") && doc["outgoing"].IsArray())
			for (const auto& entry : doc["outgoing"].GetArray())
				out.outgoing.push_back(ParseFriend(entry));
		out.ok = true;
		return out;
	}

	InvitationList GetInvitations()
	{
		InvitationList out;
		rapidjson::Document doc;
		if (!GetJson("/api/v1/me/invitations", true, doc, out.error, "invitations"))
			return out;
		if (doc.HasMember("invitations") && doc["invitations"].IsArray())
		{
			for (const auto& entry : doc["invitations"].GetArray())
			{
				if (!entry.IsObject())
					continue;
				Invitation inv;
				inv.invitation_id = JsonString(entry, "invitation_id");
				inv.from = JsonString(entry, "from");
				inv.title_id = ToUpper(JsonString(entry, "title_id"));
				inv.expires_at = JsonString(entry, "expires_at");
				out.invitations.push_back(inv);
			}
		}
		out.ok = true;
		return out;
	}

	std::string AcceptFriend(const std::string& account_id)
	{
		return PostAccount("/api/v1/me/friends/requests/accept", account_id, "accept friend");
	}

	// The website API has no separate decline: removing a pending request is the
	// decline (the core ends a pending friendship in either direction).
	std::string DeclineFriend(const std::string& account_id)
	{
		return PostAccount("/api/v1/me/friends/remove", account_id, "decline friend");
	}

	std::string RemoveFriend(const std::string& account_id)
	{
		return PostAccount("/api/v1/me/friends/remove", account_id, "remove friend");
	}

	std::string BlockFriend(const std::string& account_id)
	{
		return PostAccount("/api/v1/me/friends/block", account_id, "block");
	}

	std::string SendFriendRequest(const std::string& friend_code)
	{
		std::string bearer, error;
		if (!GetBearerOr(bearer, error))
			return error;
		const auto resp = Request("POST", ApiBase() + "/api/v1/me/friends/requests",
			JsonBody("friend_code", friend_code), bearer);
		if (!resp.error.empty() || (resp.status != 200 && resp.status != 201))
			return ErrorFor(resp, "friend request");
		return {};
	}

	Profile GetProfile()
	{
		Profile out;
		rapidjson::Document doc;
		if (!GetJson("/api/v1/me", true, doc, out.error, "profile"))
			return out;
		out.account_id = JsonString(doc, "account_id");
		out.display_name = JsonString(doc, "display_name");
		out.friend_code = JsonString(doc, "friend_code");
		if (doc.HasMember("linked_platforms") && doc["linked_platforms"].IsArray())
		{
			for (const auto& platform : doc["linked_platforms"].GetArray())
			{
				const std::string ns = JsonString(platform, "namespace");
				if (!ns.empty())
					out.linked_platforms.push_back(ns);
			}
		}
		out.ok = true;
		return out;
	}

	std::optional<std::vector<uint8_t>> GetAvatar(const std::string& account_id)
	{
		if (!IsTameId(account_id))
			return std::nullopt;
		return FetchPublic(fmt::format("/media/avatars/{}/256", account_id));
	}

	namespace
	{
		std::mutex s_catalogueMutex;
		std::map<std::string, CatalogueTitle> s_catalogue;
	}

	std::map<std::string, CatalogueTitle> GetCatalogueIfLoaded()
	{
		std::lock_guard lock(s_catalogueMutex);
		return s_catalogue;
	}

	std::map<std::string, CatalogueTitle> GetCatalogue()
	{
		auto& s_mutex = s_catalogueMutex;
		{
			std::lock_guard lock(s_mutex);
			if (!s_catalogue.empty())
				return s_catalogue;
		}
		std::map<std::string, CatalogueTitle> out;
		rapidjson::Document doc;
		std::string error;
		if (!GetJson("/api/v1/titles", false, doc, error, "catalogue"))
			return out;
		if (doc.HasMember("titles") && doc["titles"].IsArray())
		{
			for (const auto& title : doc["titles"].GetArray())
			{
				const std::string id = ToUpper(JsonString(title, "title_id"));
				if (!id.empty())
					out[id] = {JsonString(title, "name"), JsonString(title, "console")};
			}
		}
		std::lock_guard lock(s_mutex);
		s_catalogue = out;
		return out;
	}

	CloudSaves GetCloudSaves()
	{
		CloudSaves out;
		rapidjson::Document doc;
		if (!GetJson("/api/v1/me/saves", true, doc, out.error, "cloud saves"))
			return out;
		if (doc.HasMember("usage") && doc["usage"].IsObject())
		{
			out.used = JsonUint(doc["usage"], "allowance_used");
			out.allowance = JsonUint(doc["usage"], "allowance");
		}
		if (doc.HasMember("saves") && doc["saves"].IsArray())
		{
			for (const auto& item : doc["saves"].GetArray())
			{
				CloudSave save;
				save.platform = JsonString(item, "platform");
				save.title_id = ToUpper(JsonString(item, "title_id"));
				save.name = JsonString(item, "name");
				if (item.HasMember("versions") && item["versions"].IsArray())
				{
					for (const auto& v : item["versions"].GetArray())
						save.versions.push_back({(int)JsonInt(v, "number"), JsonBool(v, "conflict"), JsonUint(v, "size"),
												 JsonString(v, "device"), JsonString(v, "saved_at")});
				}
				std::stable_sort(save.versions.begin(), save.versions.end(),
					[](const SaveVersion& a, const SaveVersion& b) { return a.number > b.number; });
				out.saves.push_back(std::move(save));
			}
		}
		out.ok = true;
		return out;
	}

	ModList GetMods(const std::string& title_id)
	{
		ModList out;
		if (!IsTameId(title_id))
		{
			out.ok = true;
			return out;
		}
		rapidjson::Document doc;
		if (!GetJson(fmt::format("/api/v1/titles/{}/mods", ToLower(title_id)), false, doc, out.error, "mods"))
			return out;
		if (doc.HasMember("mods") && doc["mods"].IsArray())
		{
			for (const auto& item : doc["mods"].GetArray())
				out.mods.push_back({JsonString(item, "id"), JsonString(item, "name"), JsonString(item, "version"),
									JsonString(item, "author"), JsonString(item, "licence"), JsonString(item, "summary")});
		}
		out.ok = true;
		return out;
	}

	std::vector<std::string> GetFavouriteModIds()
	{
		std::vector<std::string> out;
		rapidjson::Document doc;
		std::string error;
		if (OpenPakAccount::GetBearer().empty() || !GetJson("/api/v1/me/favourites", true, doc, error, "favourites"))
			return out;
		if (doc.HasMember("favourites") && doc["favourites"].IsArray())
			for (const auto& item : doc["favourites"].GetArray())
				out.push_back(JsonString(item, "id"));
		return out;
	}

	std::string SetModFavourite(const std::string& mod_id, bool favourite)
	{
		std::string bearer, error;
		if (!GetBearerOr(bearer, error))
			return error;
		if (!IsTameId(mod_id))
			return OpenPakError::Unreachable;
		const auto resp = Request(favourite ? "PUT" : "DELETE", ApiBase() + "/api/v1/me/favourites/" + mod_id, {}, bearer);
		if (!resp.error.empty() || resp.status / 100 != 2)
			return ErrorFor(resp, "favourite");
		return {};
	}

	Players GetPlayers()
	{
		Players out;
		rapidjson::Document doc;
		std::string error;
		if (!GetJson("/api/v1/status", false, doc, error, "status"))
			return out;
		out.ok = true;
		out.players_online = (int)JsonInt(doc, "players_online");
		auto read = [&doc](const char* list, const char* key, std::vector<std::pair<std::string, int>>& into) {
			if (!doc.HasMember(list) || !doc[list].IsArray())
				return;
			for (const auto& row : doc[list].GetArray())
				into.emplace_back(JsonString(row, key), (int)JsonInt(row, "players"));
		};
		read("titles", "title_id", out.titles);
		read("networks", "namespace", out.networks);
		return out;
	}

	ServiceStatus GetServiceStatus()
	{
		ServiceStatus out;
		// The status page lives beside the site: https://status.<site host>.
		const std::string base = ApiBase();
		const auto authority = base.find("://");
		out.url = authority == std::string::npos ? base : base.substr(0, authority + 3) + "status." + base.substr(authority + 3);
		if (const char* env = getenv("OPENPAK_STATUS"); env && std::string_view(env).rfind("https://", 0) == 0)
			out.url = env;
		const auto resp = Request("GET", out.url + "/api/status", {}, {}, 10, true);
		if (!resp.error.empty() || resp.status != 200)
			return out;
		rapidjson::Document doc;
		if (doc.Parse(resp.body.c_str()).HasParseError() || !doc.IsObject())
			return out;
		// The status page encodes its own view struct, so the keys are Go's field names.
		out.headline = JsonString(doc, "Headline");
		out.summary = JsonString(doc, "Sub");
		out.state = JsonString(doc, "State");
		if (doc.HasMember("Groups") && doc["Groups"].IsArray())
		{
			for (const auto& group : doc["Groups"].GetArray())
			{
				if (!group.IsObject() || !group.HasMember("Services") || !group["Services"].IsArray())
					continue;
				for (const auto& service : group["Services"].GetArray())
					out.services.push_back({JsonString(service, "Name"), JsonBool(service, "Up"),
											JsonDouble(service, "Uptime"), JsonString(service, "Latency")});
			}
		}
		out.ok = true;
		return out;
	}

	std::optional<int> PingUrl(const std::string& url)
	{
		const auto start = std::chrono::steady_clock::now();
		const auto resp = Request("GET", url, {}, {}, 5);
		if (!resp.error.empty() || resp.status == 0)
			return std::nullopt;
		return (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	}

	std::optional<int> PingWebsite()
	{
		const auto start = std::chrono::steady_clock::now();
		const auto resp = Request("GET", ApiBase() + "/healthz", {}, {}, 5);
		if (!resp.error.empty() || resp.status != 200)
			return std::nullopt;
		return (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	}
} // namespace OpenPakSocial
