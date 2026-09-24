#pragma once

#include <wx/bitmap.h>
#include <wx/string.h>
#include <wx/window.h>

#include <functional>
#include <string>

class wxFrame;

// OpenPak: the pieces of the OpenPak UX spec (emulators/prds/openpak-ux-spec.md) that the
// menu, the window and the settings share in Cemu: the sign-in dialog (§3.3), the connect
// prompt (§3.2), the sign-out and destructive-action confirmations (§3.5, §7), the toasts
// (§3.10), and the string helpers (§7.2: the English goes through _()).
namespace OpenPakUI
{
	// The OpenPak window's pages, in window order (§3.6).
	enum class Page : int
	{
		Account = 0,
		Friends,
		Invitations,
		Saves,
		Mods,
		News,
		Status,
		Count,
	};

	// What the main window lends the rest: open the window at a page, open General settings at
	// the OpenPak tab, and hear about sign-in and sign-out (menu, account list).
	struct Host
	{
		wxFrame* frame = nullptr;
		std::function<void(Page)> openWindow;
		std::function<void()> openSettings;
		std::function<void()> accountChanged;
	};
	void SetHost(Host host);
	const Host& GetHost();

	// Listeners that want to redraw after sign-in or sign-out (an open window, a settings tab).
	// Returns an id for RemoveAccountListener.
	int AddAccountListener(std::function<void()> listener);
	void RemoveAccountListener(int id);
	void NotifyAccountChanged();

	// State.
	bool IsGameRunning();
	bool IsOn(); // the active Mii account's Network Service is OpenPak
	bool IsSignedIn();
	wxString SignedInName(); // the account's display name when known, else the PNID
	wxString WebsiteUrl();
	void OpenWebsite(const wxString& path = {});

	// Strings.
	wxString ErrorText(const std::string& error); // an OpenPakError code (or message) -> §7.2 text
	wxString LocalTime(const std::string& rfc3339); // locale short date and time; "—" when empty
	wxString GameName(const std::string& titleIdHex); // local title, else catalogue, else empty
	wxString ConsoleName(const std::string& ns);      // "wiiu" -> "Wii U"
	wxString None();                                  // "—"

	// The account avatar (round), 20 px for the menu and 56 px for the Account page; null when
	// not loaded. LoadAvatar fetches it off the UI thread and notifies the listeners.
	wxBitmap Avatar(int size);
	void LoadAvatar();

	// §3.3. Runs the sign-in dialog; on success applies the Wii U identity into the OpenPak Mii
	// account, turns the OpenPak Network Service on for it, shows the toast and notifies.
	// Returns true when signed in.
	bool SignIn(wxWindow* parent, const wxString& intro = {});

	// Writes the stored identity into the OpenPak Mii account (§5.10) and re-runs the online
	// check, so a game can go online without a restart. Returns an error message or empty.
	std::string ApplyIdentity();

	// §3.5: the confirmation, then sign-out (token revoked, the Mii account stays) and the
	// toast. Returns true when signed out.
	bool ConfirmAndSignOut(wxWindow* parent);

	// True while a confirmation or the sign-in dialog is open (the window holds its refresh).
	bool PromptOpen();

	// A confirmation for a destructive action: Cancel is the default and what Escape does.
	bool Confirm(wxWindow* parent, const wxString& title, const wxString& body, const wxString& confirmLabel);

	// §3.2 connect prompt, once per install, on a plain interactive launch.
	void MaybeAskToConnect(wxWindow* parent);

	// At startup: a stored sign-in the website refuses turns into toast.sign_in_again (§5.1).
	void CheckStoredSignIn();

	// §3.10 toasts.
	enum class ToastTarget
	{
		None,
		Account,
		Friends,
		Invitations,
		SignIn,
	};
	void Toast(const wxString& category, const wxString& text, ToastTarget target, bool evenIfInactive = false);

	// The friends poller behind the friend and invitation toasts; runs while signed in.
	void StartPoller();
	void StopPoller();

	// The signed-ceiling change notice (docs/signed-ceiling.md, rule 4): hooks the network
	// profile's change listener to one OPENPAK toast per new effective wiiu set, and starts the
	// six-hour (±10 %) re-check. Once, after SetHost.
	void StartNetworkWatch();

	// Re-fetches the ceiling and the profile off the UI thread (the timer, after a sign-in).
	void RecheckNetwork();
}
