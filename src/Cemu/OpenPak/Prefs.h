#pragma once

#include <cstdint>
#include <string>

// OpenPak: the emulator-side OpenPak settings (emulators/prds/openpak-ux-spec.md §3.13) that are
// not the per-account Network Service choice, kept in config/openpak_settings.txt (key=value).
// They survive sign-out; the session file does not.
namespace OpenPakPrefs
{
	enum class Corner : int
	{
		BottomRight = 0,
		BottomLeft = 1,
		TopRight = 2,
		TopLeft = 3,
	};

	bool CloudSync();          // default on
	void SetCloudSync(bool on);
	bool Notifications();      // default on
	void SetNotifications(bool on);
	Corner NotificationCorner(); // default bottom right
	void SetNotificationCorner(Corner corner);

	// The website as typed in Advanced ("https://openpak.org" when unset).
	std::string Website();
	void SetWebsite(const std::string& website);

	// Where the account API lives: OPENPAK_API when it is https or loopback, else the Website
	// setting when it is, else https://openpak.org. No trailing slash.
	std::string ApiBase();

	// The device name the sign-in dialog last used (empty until the first sign-in).
	std::string DeviceName();
	void SetDeviceName(const std::string& name);

	// The connect prompt (§3.2) is asked once per install.
	bool ConnectAsked();
	void SetConnectAsked();

	// The Mii account (persistent id) the OpenPak identity was applied to (§5.10); 0 for none.
	uint32_t Slot();
	void SetSlot(uint32_t persistentId);
}
