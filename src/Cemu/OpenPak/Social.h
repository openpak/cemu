#pragma once

#include <cstdint>
#include <string>
#include <vector>

// OpenPak: the social surfaces the website serves for every console at once
// (emulators/prds/emulator-integration-prd.md §3.5): one friend graph — the same list
// the phone and the website show — plus its requests and the invitation
// inbox. In-game friends, notifications and invitations ride NEX (nn-friends)
// and land in this same graph, so what this module shows is what the games see.
namespace OpenPakSocial
{
	struct Friend
	{
		std::string account_id;
		std::string display_name;
		bool online = false;
		std::string title_id; // playing title id, when online in a game
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

	// All blocking; run off the GUI thread. Empty-string error means success.
	FriendList GetFriends();
	RequestList GetRequests();
	InvitationList GetInvitations();
	std::string AcceptFriend(const std::string& account_id);
	std::string DeclineFriend(const std::string& account_id);
	std::string RemoveFriend(const std::string& account_id);
	// Sends a request to the account behind a friend code (the website resolves
	// codes across consoles; codes are minted per console family).
	std::string SendFriendRequest(const std::string& friend_code);
}
