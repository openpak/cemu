#pragma once

#include <string>
#include <string_view>

// OpenPak: the account session and the identity application (PRD
// emulators/prds/emulator-integration-prd.md E3, NA-1a).
//
// Sign-in exchanges an OpenPak email and password for a website bearer
// (POST /api/v1/token), asks for the account's Wii U identity (GET
// /api/v1/me/wiiu — PNID, NEX credentials, Mii and the account.dat password
// cache, minted on first sight), and writes that identity into the emulated
// console: an account.dat the ACT login accepts, persisid.dat, the active
// account and the OpenPak network service. A console dump is never needed.
//
// The session file lives at config/openpak_session.txt (key=value). The
// bearer is a website credential: it only ever travels to openpak.org.
// Keychain-backed storage is tracked in the PRD; this file is the same trust
// level as the account.dat a console keeps.

namespace OpenPakAccount
{
	struct SignInResult
	{
		bool ok = false;
		std::string error; // human-readable, when !ok
	};

	// Both network calls block; run them off the GUI thread.
	SignInResult SignIn(std::string_view email, std::string_view password);
	void SignOut();

	// Session state, for the settings UI.
	bool IsSignedIn();
	std::string GetUsername(); // the NNID / display name, empty when signed out

	// The website bearer for the OpenPak surfaces that speak to openpak.org
	// (Social). Empty when signed out.
	std::string GetBearer();

	// Writes the stored identity into the emulated console. Returns an error
	// message, or empty on success. Called by SignIn and by the settings UI
	// ("apply again" after a failed or partial apply).
	std::string ApplyIdentity();
}
