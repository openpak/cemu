#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// OpenPak: the account session and the identity application (PRD
// emulators/prds/emulator-integration-prd.md E3, NA-1a).
//
// Sign-in exchanges an OpenPak email and password for a website bearer
// (POST /api/v1/token), asks for the account's Wii U identity (GET
// /api/v1/me/wiiu — PNID, NEX credentials, Mii and the account.dat password
// cache, minted on first sight), and stores it. ApplyIdentity then writes that
// identity into the emulated console: an account.dat the ACT login accepts,
// persisid.dat, the active account and the OpenPak network service. A console
// dump is never needed.
//
// The identity always goes into the same Mii account (emulators/prds/openpak-ux-spec.md
// §5.10): the slot it was applied to first, remembered in OpenPakPrefs::Slot(). Sign-out
// leaves that Mii account alone, and the next sign-in writes into it again.
//
// The session file lives at config/openpak_session.txt (key=value). The
// bearer is a website credential: it only ever travels to the OpenPak website.
// Errors are OpenPakError codes (Errors.h) or internal English messages.

namespace OpenPakAccount
{
	struct SignInResult
	{
		bool ok = false;
		std::string error; // an OpenPakError code or a message, when !ok
	};

	// Blocks on the network; run it off the GUI thread. Stores the session and the identity but
	// does not apply it: call ApplyIdentity on the GUI thread afterwards.
	SignInResult SignIn(std::string_view email, std::string_view password, std::string_view deviceName);

	// Forgets the session now and revokes the token in the background. The Mii account stays.
	void SignOut();

	// Asks the website whether the stored token still works (GET /api/v1/me). Blocks; run it off
	// the GUI thread. When the token is refused the session is forgotten locally and true is
	// returned; any other outcome (fine, offline, server trouble) returns false.
	bool DropIfRejected();

	// Session state.
	bool IsSignedIn();
	std::string GetUsername(); // the PNID, empty when signed out
	std::string GetMiiName();
	uint32_t GetPid();

	// The website bearer for the OpenPak surfaces that speak to the website (Social). Empty when
	// signed out.
	std::string GetBearer();

	// Writes the stored identity into the emulated console, reusing the OpenPak Mii account.
	// Returns an error message, or empty on success. GUI thread only (it edits the config).
	std::string ApplyIdentity();

	// The Mii account holding the OpenPak identity, or 0 when it is not (or no longer) there.
	uint32_t AppliedSlot();
}
