#include "Cemu/OpenPak/Social.h"
#include "Cemu/OpenPak/Account.h"

#include <curl/curl.h>
#include <rapidjson/document.h>
#include <fmt/core.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
	std::string ApiBase()
	{
		std::string base = "https://openpak.org";
		if (const char* env = getenv("OPENPAK_API"))
		{
			std::string candidate = env;
			std::transform(candidate.begin(), candidate.end(), candidate.begin(), ::tolower);
			if (candidate.rfind("https://", 0) == 0 || candidate.rfind("http://127.0.0.1", 0) == 0 ||
				candidate.rfind("http://localhost", 0) == 0 || candidate.rfind("http://[::1]", 0) == 0)
				base = env;
		}
		return base;
	}

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
						 const std::string& bearer)
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

	std::string ErrorFrom(const std::string& body, long status)
	{
		rapidjson::Document doc;
		if (!doc.Parse(body.c_str()).HasParseError() && doc.HasMember("error") && doc["error"].IsString())
			return doc["error"].GetString();
		return fmt::format("the request failed (HTTP {})", status);
	}

	std::string JsonString(const rapidjson::Value& doc, const char* field)
	{
		if (doc.HasMember(field) && doc[field].IsString())
			return doc[field].GetString();
		return {};
	}

	bool JsonBool(const rapidjson::Value& doc, const char* field)
	{
		return doc.HasMember(field) && doc[field].IsBool() && doc[field].GetBool();
	}

	OpenPakSocial::Friend ParseFriend(const rapidjson::Value& entry)
	{
		OpenPakSocial::Friend f;
		if (!entry.IsObject())
			return f;
		f.account_id = JsonString(entry, "account_id");
		f.display_name = JsonString(entry, "display_name");
		f.online = JsonBool(entry, "online");
		f.title_id = JsonString(entry, "title_id");
		f.since = JsonString(entry, "since");
		return f;
	}

	bool GetBearerOr(std::string& bearer, std::string& error)
	{
		bearer = OpenPakAccount::GetBearer();
		if (bearer.empty())
		{
			error = "not signed in";
			return false;
		}
		return true;
	}
} // namespace

namespace OpenPakSocial
{
	FriendList GetFriends()
	{
		FriendList out;
		std::string bearer;
		if (!GetBearerOr(bearer, out.error))
			return out;
		const auto resp = Request("GET", ApiBase() + "/api/v1/me/friends", {}, bearer);
		if (!resp.error.empty())
		{
			out.error = "could not reach openpak.org: " + resp.error;
			return out;
		}
		if (resp.status != 200)
		{
			out.error = ErrorFrom(resp.body, resp.status);
			return out;
		}
		rapidjson::Document doc;
		if (doc.Parse(resp.body.c_str()).HasParseError() || !doc.HasMember("friends") || !doc["friends"].IsArray())
		{
			out.error = "the friends response was not understood";
			return out;
		}
		for (const auto& entry : doc["friends"].GetArray())
			out.friends.push_back(ParseFriend(entry));
		out.ok = true;
		return out;
	}

	RequestList GetRequests()
	{
		RequestList out;
		std::string bearer;
		if (!GetBearerOr(bearer, out.error))
			return out;
		const auto resp = Request("GET", ApiBase() + "/api/v1/me/friends/requests", {}, bearer);
		if (!resp.error.empty())
		{
			out.error = "could not reach openpak.org: " + resp.error;
			return out;
		}
		if (resp.status != 200)
		{
			out.error = ErrorFrom(resp.body, resp.status);
			return out;
		}
		rapidjson::Document doc;
		if (doc.Parse(resp.body.c_str()).HasParseError())
		{
			out.error = "the requests response was not understood";
			return out;
		}
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
		std::string bearer;
		if (!GetBearerOr(bearer, out.error))
			return out;
		const auto resp = Request("GET", ApiBase() + "/api/v1/me/invitations", {}, bearer);
		if (!resp.error.empty())
		{
			out.error = "could not reach openpak.org: " + resp.error;
			return out;
		}
		if (resp.status != 200)
		{
			out.error = ErrorFrom(resp.body, resp.status);
			return out;
		}
		rapidjson::Document doc;
		if (doc.Parse(resp.body.c_str()).HasParseError() || !doc.HasMember("invitations") ||
			!doc["invitations"].IsArray())
		{
			out.error = "the invitations response was not understood";
			return out;
		}
		for (const auto& entry : doc["invitations"].GetArray())
		{
			if (!entry.IsObject())
				continue;
			Invitation inv;
			inv.invitation_id = JsonString(entry, "invitation_id");
			inv.from = JsonString(entry, "from");
			inv.title_id = JsonString(entry, "title_id");
			inv.expires_at = JsonString(entry, "expires_at");
			out.invitations.push_back(inv);
		}
		out.ok = true;
		return out;
	}

	std::string AcceptFriend(const std::string& account_id)
	{
		std::string bearer, error;
		if (!GetBearerOr(bearer, error))
			return error;
		const auto resp = Request("POST", ApiBase() + "/api/v1/me/friends/requests/accept",
			fmt::format("{{\"account_id\":\"{}\"}}", account_id), bearer);
		if (!resp.error.empty())
			return "could not reach openpak.org: " + resp.error;
		if (resp.status != 200)
			return ErrorFrom(resp.body, resp.status);
		return {};
	}

	// The website API has no separate decline: removing a pending request is the
	// decline (the core ends a pending friendship in either direction).
	std::string DeclineFriend(const std::string& account_id)
	{
		std::string bearer, error;
		if (!GetBearerOr(bearer, error))
			return error;
		const auto resp = Request("POST", ApiBase() + "/api/v1/me/friends/remove",
			fmt::format("{{\"account_id\":\"{}\"}}", account_id), bearer);
		if (!resp.error.empty())
			return "could not reach openpak.org: " + resp.error;
		if (resp.status != 200)
			return ErrorFrom(resp.body, resp.status);
		return {};
	}

	std::string RemoveFriend(const std::string& account_id)
	{
		std::string bearer, error;
		if (!GetBearerOr(bearer, error))
			return error;
		const auto resp = Request("POST", ApiBase() + "/api/v1/me/friends/remove",
			fmt::format("{{\"account_id\":\"{}\"}}", account_id), bearer);
		if (!resp.error.empty())
			return "could not reach openpak.org: " + resp.error;
		if (resp.status != 200)
			return ErrorFrom(resp.body, resp.status);
		return {};
	}
} // namespace OpenPakSocial
