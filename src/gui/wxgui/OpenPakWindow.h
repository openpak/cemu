#pragma once

#include "wxgui/OpenPakUI.h"

#include <wx/dialog.h>
#include <wx/timer.h>

#include <vector>

class wxGauge;
class wxListbook;
class wxStaticText;
class OpenPakPage;

// OpenPak: the OpenPak window (emulators/prds/openpak-ux-spec.md §3.6, Wii U family). A modal
// dialog with the seven pages in a left list — Account, Friends, Invitations, Cloud saves, Mods,
// News, Status — each with its title and Refresh, and one footer: a busy bar, a status line and
// Close. Every network call runs off the UI thread; a failed refresh keeps the last data and
// says so in the status line.
class OpenPakWindow : public wxDialog
{
  public:
	OpenPakWindow(wxWindow* parent, OpenPakUI::Page page);
	~OpenPakWindow() override;

	void ShowPage(OpenPakUI::Page page);

	// For the pages.
	void BeginBusy();
	void EndBusy();
	void SetStatus(const wxString& text);
	void SetFriendsBadge(size_t incoming);

	// The window that is open, if any (it is modal, so there is at most one).
	static OpenPakWindow* Current() { return s_current; }

  private:
	void OnPageChanged();
	void OnPollTimer();
	void ReloadAll();

	wxListbook* m_book = nullptr;
	wxGauge* m_gauge = nullptr;
	wxStaticText* m_status = nullptr;
	wxTimer m_pulse;
	wxTimer m_poll;
	int m_busy = 0;
	int m_listener = 0;
	std::vector<OpenPakPage*> m_pages;

	static OpenPakWindow* s_current;
};
