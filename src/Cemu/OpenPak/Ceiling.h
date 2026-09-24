#pragma once

#include <string>
#include <vector>

// OpenPak: the signed redirect ceiling (docs/signed-ceiling.md, the contract every OpenPak
// client follows). The ceiling is the set of domain families Cemu may ever address for the
// wiiu platform. It is a file signed with an Ed25519 key that never touches the server; Cemu
// pins the public key, so a compromised box can serve the file but cannot widen it.
//
// The network profile chooses within the ceiling; a profile name outside it is dropped on its
// own (see NetworkProfile.cpp), never the whole profile.
namespace OpenPakCeiling
{
	// Client rules 1-3: the first call loads and re-verifies the cached envelope; every call then
	// fetches a fresh one from the pinned https://openpak.org origin (two seconds, one attempt,
	// public TLS trust), verifies it and accepts it when its version is at least the highest
	// version ever accepted. Any failure keeps what is held and logs the reason once. Blocking;
	// run it off the UI thread or alongside the profile fetch.
	void Update();

	// The wiiu families in effect: the verified ceiling's, or, when no verified ceiling has ever
	// been held, the compiled-in first-boot fallback.
	std::vector<std::string> Families();

	// A host name, or a redirect suffix in family form (".x.example"), lies inside one of the
	// families on a label boundary. A family covers its apex and everything below it.
	bool Inside(const std::string& name, const std::vector<std::string>& families);

	// "signed v3" or "built-in", for the log and the settings.
	std::string Source();
}
