#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// OpenPak: what the OpenPak window shows, from the website (emulators/prds/emulator-integration-prd.md
// §3.5, emulators/prds/openpak-ux-spec.md §3.6): one friend graph for every console — the list
// the phone and the website show — with its requests, the invitation inbox, the account card,
// the catalogue, cloud saves, mods and the network status. In-game friends, notifications and
// invitations ride NEX (nn-friends) and land in the same graph, so this is what the games see.
//
// Everything here blocks on the network: run it off the GUI thread. Errors are OpenPakError
// codes (Errors.h); an empty error means success.
namespace OpenPakSocial
{
	struct Friend
	{
		std::string account_id;
		std::string display_name;
		bool online = false;
		std::string title_id; // playing title id, when online in a game
		std::string console;  // the namespace it is online on ("wiiu", "switch", ...)
		std::string since;    // RFC3339, when the presence began
	};

	struct FriendList
	{
		bool ok = false;
		std::string error; // set when !ok
		std::vector<Friend> friends;
	};

	struct RequestList
	{
		bool ok = false;
		std::string error;
		std::vector<Friend> incoming; // waiting for me
		std::vector<Friend> outgoing; // waiting for them
	};

	struct Invitation
	{
		std::string invitation_id;
		std::string from;       // display name of the sender
		std::string title_id;
		std::string expires_at; // RFC3339
	};

	struct InvitationList
	{
		bool ok = false;
		std::string error;
		std::vector<Invitation> invitations;
	};

	struct Profile
	{
		bool ok = false;
		std::string error;
		std::string account_id;
		std::string display_name;
		std::string friend_code;
		std::vector<std::string> linked_platforms;
	};

	struct CatalogueTitle
	{
		std::string name;
		std::string console;
	};

	struct SaveVersion
	{
		int number = 0;
		bool conflict = false;
		uint64_t size = 0;
		std::string device;
		std::string saved_at; // RFC3339
	};

	struct CloudSave
	{
		std::string platform;
		std::string title_id;
		std::string name;
		std::vector<SaveVersion> versions; // newest first
	};

	struct CloudSaves
	{
		bool ok = false;
		std::string error;
		uint64_t used = 0;
		uint64_t allowance = 0;
		std::vector<CloudSave> saves;
	};

	struct Mod
	{
		std::string id;
		std::string name;
		std::string version;
		std::string author;
		std::string licence;
		std::string summary;
	};

	struct ModList
	{
		bool ok = false;
		std::string error;
		std::vector<Mod> mods;
	};

	struct Players
	{
		bool ok = false;
		int players_online = 0;
		std::vector<std::pair<std::string, int>> titles;   // title id, players
		std::vector<std::pair<std::string, int>> networks; // namespace, players
	};

	struct ServiceCheck
	{
		std::string name;
		bool up = false;
		double uptime = 0;
		std::string latency;
	};

	struct ServiceStatus
	{
		bool ok = false;
		std::string url; // the status page
		std::string headline;
		std::string summary;
		std::string state; // "up", "degraded", "down", as the page words it
		std::vector<ServiceCheck> services;
	};

	FriendList GetFriends();
	RequestList GetRequests();
	InvitationList GetInvitations();
	std::string AcceptFriend(const std::string& account_id);
	std::string DeclineFriend(const std::string& account_id);
	std::string RemoveFriend(const std::string& account_id); // also cancels an outgoing request
	std::string BlockFriend(const std::string& account_id);
	// Sends a request to the account behind a friend code (the website resolves
	// codes across consoles; codes are minted per console family).
	std::string SendFriendRequest(const std::string& friend_code);

	Profile GetProfile();
	std::optional<std::vector<uint8_t>> GetAvatar(const std::string& account_id);

	// The public catalogue, keyed by upper-case title id. Fetched once and kept.
	std::map<std::string, CatalogueTitle> GetCatalogue();
	// What GetCatalogue has fetched so far, without the network (safe on the GUI thread).
	std::map<std::string, CatalogueTitle> GetCatalogueIfLoaded();

	CloudSaves GetCloudSaves();
	ModList GetMods(const std::string& title_id);
	std::vector<std::string> GetFavouriteModIds();
	std::string SetModFavourite(const std::string& mod_id, bool favourite);

	Players GetPlayers();
	ServiceStatus GetServiceStatus();
	// Round trip to the website's /healthz, or to any URL, in milliseconds; nullopt when it does
	// not answer.
	std::optional<int> PingWebsite();
	std::optional<int> PingUrl(const std::string& url);
}
