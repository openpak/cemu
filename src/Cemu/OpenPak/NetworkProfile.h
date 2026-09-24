#pragma once

#include <functional>
#include <string>
#include <string_view>

// OpenPak: the applied network profile (emulators/prds/emulator-network-profile-prd.md §4b).
//
// Cemu addresses service URLs rather than resolving names, so this consumes the profile's
// services[] and nothing else from redirect.*. One conditional GET per launch, two-second
// timeout, best-effort: last-known-good beats a fetch, the compiled-in OpenPakURLs beat
// nothing, and a game starts either way.
namespace OpenPakNetworkProfile
{
	// §2: one conditional GET with the stored ETag, alongside the signed ceiling
	// (docs/signed-ceiling.md, OpenPakCeiling). A fresh profile of the right shape is cached;
	// 304 or any failure uses what is stored; nothing stored means the compiled-in OpenPakURLs.
	// Whatever profile is used goes through the verified ceiling: a service or redirect name
	// outside it is dropped on its own and logged, never the whole profile. Never retries, never
	// blocks longer than two seconds.
	void ApplyAtLaunch();

	// The "Refresh network settings" action, the six-hour re-check and the re-check after a
	// sign-in. Same flow, safe to ask for from any thread while Cemu runs.
	void Refresh();

	// Client rule 4 of the ceiling contract: called (on the refreshing thread) once for each new
	// digest of the effective wiiu service set that a Refresh after launch lands on.
	void SetChangeListener(std::function<void()> listener);

	// The URL for a service id ("act", "ecs", "nus", "ias", "ccsu", "ccs", "idbe", "boss",
	// "tagaya", "olv"): the applied profile's value when it names one, the compiled-in
	// OpenPakURLs value when it does not.
	std::string ServiceURL(std::string_view id);

	// What is in effect, for the settings UI: "fetched", "cached" or "built-in", and the
	// profile version (-1 for the compiled-in defaults).
	std::string GetSource();
	int GetVersion();
}
