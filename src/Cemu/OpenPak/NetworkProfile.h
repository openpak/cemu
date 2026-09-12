#pragma once

#include <string>
#include <string_view>

// OpenPak: the applied network profile (prds/emulator-network-profile-prd.md §4b).
//
// Cemu addresses service URLs rather than resolving names, so this consumes the profile's
// services[] and nothing else from redirect.*. One conditional GET per launch, two-second
// timeout, best-effort: last-known-good beats a fetch, the compiled-in OpenPakURLs beat
// nothing, and a game starts either way.
namespace OpenPakNetworkProfile
{
	// §2: one conditional GET with the stored ETag; a fresh profile is validated against the
	// compiled-in families, cached and applied; 304 or any failure applies what is stored;
	// nothing stored means the compiled-in OpenPakURLs. Never retries, never blocks longer
	// than two seconds.
	void ApplyAtLaunch();

	// The "Refresh network settings" action. Same flow, safe to ask for while Cemu runs.
	void Refresh();

	// The URL for a service id ("act", "ecs", "nus", "ias", "ccsu", "ccs", "idbe", "boss",
	// "tagaya", "olv"): the applied profile's value when it names one, the compiled-in
	// OpenPakURLs value when it does not.
	std::string ServiceURL(std::string_view id);

	// What is in effect, for the settings UI: "fetched", "cached" or "built-in", and the
	// profile version (-1 for the compiled-in defaults).
	std::string GetSource();
	int GetVersion();
}
