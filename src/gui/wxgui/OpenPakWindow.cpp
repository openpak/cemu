#include "wxgui/OpenPakWindow.h"

#include "Cemu/OpenPak/Account.h"
#include "Cemu/OpenPak/Errors.h"
#include "Cemu/OpenPak/NetworkProfile.h"
#include "Cemu/OpenPak/Social.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/TitleList.h"
#include "config/ActiveSettings.h"

#include <wx/app.h>
#include <wx/artprov.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/filename.h>
#include <wx/gauge.h>
#include <wx/hyperlink.h>
#include <wx/listbook.h>
#include <wx/listctrl.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/uilocale.h>
#include <wx/weakref.h>

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <set>
#include <thread>

OpenPakWindow* OpenPakWindow::s_current = nullptr;

namespace
{
	const wxColour kGreen(0x3F, 0xB9, 0x50);
	const wxColour kGrey(0x7A, 0x7A, 0x7A);
	const wxColour kRed(0xE0, 0x39, 0x3E);
	const wxColour kAmber(0xD2, 0x99, 0x22);

	wxStaticText* Text(wxWindow* parent, const wxString& text, bool bold = false, bool dim = false, int wrap = 560)
	{
		auto* label = new wxStaticText(parent, wxID_ANY, text);
		if (bold)
			label->SetFont(label->GetFont().Bold());
		if (dim)
			label->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
		if (wrap > 0)
			label->Wrap(parent->FromDIP(wrap));
		return label;
	}

	wxStaticText* Dot(wxWindow* parent, const wxColour& colour)
	{
		auto* dot = new wxStaticText(parent, wxID_ANY, wxString::FromUTF8("\xE2\x97\x8F")); // ●
		dot->SetForegroundColour(colour);
		return dot;
	}

	wxStaticText* Heading(wxWindow* parent, const wxString& text)
	{
		auto* label = new wxStaticText(parent, wxID_ANY, text);
		wxFont font = label->GetFont().Bold();
		font.SetPointSize(font.GetPointSize() + 1);
		label->SetFont(font);
		return label;
	}

	std::string Upper(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::toupper(c); });
		return s;
	}

	uint64 ParseTitleId(const std::string& hex)
	{
		try
		{
			return hex.empty() ? 0 : std::stoull(hex, nullptr, 16);
		}
		catch (...)
		{
			return 0;
		}
	}

	wxString GameOrId(const std::string& titleId)
	{
		const wxString name = OpenPakUI::GameName(titleId);
		if (!name.empty())
			return name;
		return titleId.empty() ? OpenPakUI::None() : wxString::FromUTF8(titleId);
	}
} // namespace

// ---- the page base --------------------------------------------------------------------------

class OpenPakPage : public wxPanel
{
  public:
	OpenPakPage(OpenPakWindow* window, wxWindow* parent, const wxString& title, bool needsAccount)
		: wxPanel(parent), m_window(window), m_needsAccount(needsAccount)
	{
		auto* sizer = new wxBoxSizer(wxVERTICAL);
		auto* header = new wxBoxSizer(wxHORIZONTAL);
		auto* titleLabel = new wxStaticText(this, wxID_ANY, title);
		wxFont font = titleLabel->GetFont().Bold();
		font.SetPointSize(font.GetPointSize() + 4);
		titleLabel->SetFont(font);
		header->Add(titleLabel, 1, wxALIGN_CENTER_VERTICAL);
		m_refresh = new wxButton(this, wxID_ANY, _("Refresh"));
		m_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Reload(); });
		header->Add(m_refresh, 0, wxALIGN_CENTER_VERTICAL);
		sizer->Add(header, 0, wxEXPAND | wxALL, FromDIP(10));

		// Signed out: common.need_sign_in, centred, with "Sign in...".
		m_signedOut = new wxPanel(this);
		{
			auto* s = new wxBoxSizer(wxVERTICAL);
			s->AddStretchSpacer(1);
			auto* text = new wxStaticText(m_signedOut, wxID_ANY, _("Sign in to OpenPak to use this."));
			s->Add(text, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(8));
			m_signIn = new wxButton(m_signedOut, wxID_ANY, _("Sign in..."));
			m_signIn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OpenPakUI::SignIn(wxGetTopLevelParent(this)); });
			s->Add(m_signIn, 0, wxALIGN_CENTER_HORIZONTAL);
			s->AddStretchSpacer(2);
			m_signedOut->SetSizer(s);
		}
		sizer->Add(m_signedOut, 1, wxEXPAND);

		m_top = new wxBoxSizer(wxVERTICAL);
		sizer->Add(m_top, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

		m_body = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
		m_body->SetScrollRate(0, FromDIP(12));
		m_bodySizer = new wxBoxSizer(wxVERTICAL);
		m_body->SetSizer(m_bodySizer);
		sizer->Add(m_body, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
		SetSizer(sizer);
	}

	// Shows the signed-out panel or the content, and loads when there is something to load.
	void Reload()
	{
		const bool signedOut = m_needsAccount && !OpenPakAccount::IsSignedIn();
		m_signedOut->Show(signedOut);
		m_body->Show(!signedOut);
		for (auto* item : m_top->GetChildren())
			item->Show(!signedOut);
		m_refresh->Show(!signedOut);
		const bool running = OpenPakUI::IsGameRunning();
		m_signIn->Enable(!running);
		m_signIn->SetToolTip(running ? _("Stop the running game first.") : wxString());
		Layout();
		if (signedOut)
		{
			m_loaded = false;
			return;
		}
		m_loaded = true;
		Load();
	}

	bool WasLoaded() const { return m_loaded; }

  protected:
	virtual void Load() = 0;

	// Runs work() on a worker thread and done(result) back on the UI thread, if this page still
	// exists. Refresh is disabled and the window's busy bar runs meanwhile.
	template <typename Work, typename Done>
	void Run(Work work, Done done)
	{
		using Result = decltype(work());
		++m_inflight;
		m_window->BeginBusy();
		m_refresh->Enable(false);
		wxWeakRef<OpenPakPage> self(this);
		std::thread([self, work, done]() {
			Result result = work();
			wxTheApp->CallAfter([self, result, done]() mutable {
				if (!self)
					return;
				self->m_inflight--;
				self->m_window->EndBusy();
				self->m_refresh->Enable(self->m_inflight == 0);
				done(result);
			});
		}).detach();
	}

	// Rebuild the body from scratch.
	void ClearBody()
	{
		m_body->Freeze();
		m_bodySizer->Clear(true);
	}

	void FinishBody()
	{
		m_body->FitInside();
		m_body->Layout();
		m_body->Thaw();
		Layout();
	}

	// A row: a left column of lines and a right column of buttons.
	struct Row
	{
		wxPanel* panel;
		wxBoxSizer* lines;
		wxBoxSizer* buttons;
	};

	Row AddRow()
	{
		auto* panel = new wxPanel(m_body);
		auto* h = new wxBoxSizer(wxHORIZONTAL);
		auto* lines = new wxBoxSizer(wxVERTICAL);
		auto* buttons = new wxBoxSizer(wxHORIZONTAL);
		h->Add(lines, 1, wxEXPAND | wxRIGHT, FromDIP(8));
		h->Add(buttons, 0, wxALIGN_CENTER_VERTICAL);
		panel->SetSizer(h);
		m_bodySizer->Add(panel, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(4));
		m_bodySizer->Add(new wxStaticLine(m_body), 0, wxEXPAND);
		return {panel, lines, buttons};
	}

	void AddHeading(const wxString& text)
	{
		m_bodySizer->Add(Heading(m_body, text), 0, wxTOP | wxBOTTOM, FromDIP(8));
	}

	void AddEmpty(const wxString& text)
	{
		m_bodySizer->Add(Text(m_body, text, false, true), 0, wxALL, FromDIP(8));
	}

	// A centred panel for a page that does not apply to the Wii U (§3.6).
	void AddNotHere(const wxString& text, const wxString& link = {}, const wxString& url = {})
	{
		m_bodySizer->AddStretchSpacer(1);
		m_bodySizer->Add(new wxStaticBitmap(m_body, wxID_ANY, wxArtProvider::GetBitmapBundle(wxART_INFORMATION, wxART_MESSAGE_BOX)),
			0, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(8));
		auto* label = Text(m_body, text, false, false, 420);
		label->SetWindowStyle(label->GetWindowStyle() | wxALIGN_CENTRE_HORIZONTAL);
		m_bodySizer->Add(label, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(8));
		if (!link.empty())
			m_bodySizer->Add(new wxHyperlinkCtrl(m_body, wxID_ANY, link, url), 0, wxALIGN_CENTER_HORIZONTAL);
		m_bodySizer->AddStretchSpacer(2);
	}

	void Status(const std::string& error)
	{
		m_window->SetStatus(OpenPakUI::ErrorText(error));
	}

	OpenPakWindow* m_window;
	wxBoxSizer* m_top;          // fixed controls above the list (add row, pickers)
	wxScrolledWindow* m_body;
	wxBoxSizer* m_bodySizer;

  private:
	bool m_needsAccount;
	bool m_loaded = false;
	int m_inflight = 0;
	wxButton* m_refresh;
	wxPanel* m_signedOut;
	wxButton* m_signIn;
};

// ---- Account --------------------------------------------------------------------------------

class AccountPage : public OpenPakPage
{
  public:
	AccountPage(OpenPakWindow* window, wxWindow* parent) : OpenPakPage(window, parent, _("Account"), true) {}

  protected:
	void Load() override
	{
		Build(); // the local identity shows at once; the card fills in when the site answers
		Run([]() { return OpenPakSocial::GetProfile(); },
			[this](const OpenPakSocial::Profile& profile) {
				if (!profile.ok)
				{
					Status(profile.error);
					return;
				}
				m_profile = profile;
				Build();
			});
	}

  private:
	void Build()
	{
		ClearBody();
		const bool running = OpenPakUI::IsGameRunning();

		// 1. Identity card.
		auto* card = new wxBoxSizer(wxHORIZONTAL);
		const wxBitmap avatar = OpenPakUI::Avatar(FromDIP(56));
		if (avatar.IsOk())
			card->Add(new wxStaticBitmap(m_body, wxID_ANY, avatar), 0, wxALIGN_TOP | wxRIGHT, FromDIP(12));
		auto* identity = new wxBoxSizer(wxVERTICAL);
		wxString name = wxString::FromUTF8(m_profile.display_name);
		if (name.empty())
			name = OpenPakUI::SignedInName();
		auto* nameLabel = Text(m_body, name, true);
		wxFont big = nameLabel->GetFont();
		big.SetPointSize(big.GetPointSize() + 3);
		nameLabel->SetFont(big);
		identity->Add(nameLabel, 0, wxBOTTOM, FromDIP(6));
		if (!m_profile.friend_code.empty())
		{
			auto* codeRow = new wxBoxSizer(wxHORIZONTAL);
			auto* code = new wxTextCtrl(m_body, wxID_ANY, wxString::FromUTF8(m_profile.friend_code), wxDefaultPosition,
				FromDIP(wxSize(200, -1)), wxTE_READONLY);
			code->SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
			codeRow->Add(code, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
			auto* copy = new wxButton(m_body, wxID_ANY, _("Copy"));
			const wxString codeText = wxString::FromUTF8(m_profile.friend_code);
			copy->Bind(wxEVT_BUTTON, [this, codeText](wxCommandEvent&) {
				if (wxTheClipboard->Open())
				{
					wxTheClipboard->SetData(new wxTextDataObject(codeText));
					wxTheClipboard->Close();
					m_window->SetStatus(_("Copied."));
				}
				else
					m_window->SetStatus(wxString::Format(_("The clipboard is not available here. Your friend code is %s."), codeText));
			});
			codeRow->Add(copy, 0, wxALIGN_CENTER_VERTICAL);
			identity->Add(codeRow, 0, wxBOTTOM, FromDIP(6));
		}
		auto* signOut = new wxButton(m_body, wxID_ANY, _("Sign out..."));
		signOut->Enable(!running);
		if (running)
			signOut->SetToolTip(_("Stop the running game first."));
		// Deferred: signing out rebuilds this page, and with it this button.
		signOut->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { CallAfter([this]() { OpenPakUI::ConfirmAndSignOut(wxGetTopLevelParent(this)); }); });
		identity->Add(signOut, 0);
		card->Add(identity, 1, wxEXPAND);
		m_bodySizer->Add(card, 0, wxEXPAND | wxBOTTOM, FromDIP(16));

		// 2. Details.
		auto* grid = new wxFlexGridSizer(0, 2, FromDIP(8), FromDIP(16));
		grid->AddGrowableCol(1, 1);
		auto addRow = [&](const wxString& label, wxWindow* value) {
			grid->Add(Text(m_body, label, false, true), 0, wxALIGN_TOP);
			grid->Add(value, 1, wxEXPAND);
		};
		addRow(_("Friend code"), Text(m_body, m_profile.friend_code.empty() ? OpenPakUI::None() : wxString::FromUTF8(m_profile.friend_code)));
		const std::string pnid = OpenPakAccount::GetUsername();
		addRow(wxString::Format(_("%s identity"), "Wii U"),
			Text(m_body, pnid.empty() ? OpenPakUI::None() : wxString::Format(_("PNID %s"), wxString::FromUTF8(pnid))));
		wxString consoles;
		for (const auto& ns : m_profile.linked_platforms)
		{
			if (!consoles.empty())
				consoles += ", ";
			consoles += OpenPakUI::ConsoleName(ns);
		}
		addRow(_("Linked consoles"), Text(m_body, consoles.empty() ? OpenPakUI::None() : consoles));

		// Console link: the Wii U identity installed into the OpenPak Mii account, or the way to
		// try again.
		auto* link = new wxPanel(m_body);
		auto* linkSizer = new wxBoxSizer(wxVERTICAL);
		if (OpenPakAccount::AppliedSlot() != 0)
		{
			linkSizer->Add(Text(link, wxString::Format(_("Linked as %s"), wxString::FromUTF8(pnid))));
		}
		else
		{
			linkSizer->Add(Text(link, _("Signed in, but this emulator's console account did not link.")), 0, wxBOTTOM, FromDIP(4));
			auto* again = new wxButton(link, wxID_ANY, _("Try again"));
			again->Enable(!running);
			if (running)
				again->SetToolTip(_("Stop the running game first."));
			again->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
				CallAfter([this]() { // the rebuild below replaces this button
					const std::string error = OpenPakUI::ApplyIdentity();
					if (!error.empty())
						m_window->SetStatus(OpenPakUI::ErrorText(error));
					OpenPakUI::NotifyAccountChanged();
				});
			});
			linkSizer->Add(again, 0);
		}
		link->SetSizer(linkSizer);
		addRow(_("Console link"), link);
		m_bodySizer->Add(grid, 0, wxEXPAND);
		FinishBody();
	}

	OpenPakSocial::Profile m_profile;
};

// ---- Friends --------------------------------------------------------------------------------

class FriendsPage : public OpenPakPage
{
  public:
	FriendsPage(OpenPakWindow* window, wxWindow* parent) : OpenPakPage(window, parent, _("Friends"), true)
	{
		auto* add = new wxBoxSizer(wxHORIZONTAL);
		m_code = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
		m_code->SetHint(_("Friend code"));
		m_code->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { AddFriend(); });
		add->Add(m_code, 1, wxEXPAND | wxRIGHT, FromDIP(6));
		m_add = new wxButton(this, wxID_ANY, _("Add"));
		m_add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AddFriend(); });
		add->Add(m_add, 0);
		m_top->Add(add, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
	}

  protected:
	struct Data
	{
		OpenPakSocial::FriendList friends;
		OpenPakSocial::RequestList requests;
	};

	void Load() override
	{
		Run([]() {
				Data data{OpenPakSocial::GetFriends(), OpenPakSocial::GetRequests()};
				OpenPakSocial::GetCatalogue(); // game names for "Playing {game}"
				return data;
			},
			[this](const Data& data) {
				if (!data.friends.ok)
				{
					Status(data.friends.error); // the last list stays
					return;
				}
				m_data = data;
				Build();
			});
	}

  private:
	void AddFriend()
	{
		const std::string code = m_code->GetValue().Strip(wxString::both).ToStdString(wxConvUTF8);
		if (code.empty())
		{
			m_window->SetStatus(_("Type a friend code first."));
			return;
		}
		m_add->Enable(false);
		Run([code]() { return OpenPakSocial::SendFriendRequest(code); },
			[this, code](const std::string& error) {
				m_add->Enable(true);
				if (!error.empty())
				{
					Status(error);
					return;
				}
				m_code->Clear();
				m_window->SetStatus(wxString::Format(_("Asked %s to be your friend."), wxString::FromUTF8(code)));
				Load();
			});
	}

	void Act(std::function<std::string()> action)
	{
		Run(action, [this](const std::string& error) {
			if (!error.empty())
				Status(error);
			Load();
		});
	}

	static wxString StatusLine(const OpenPakSocial::Friend& f)
	{
		if (!f.online)
			return _("Offline");
		if (!f.title_id.empty())
		{
			const wxString game = OpenPakUI::GameName(f.title_id);
			return game.empty() ? _("Playing") : wxString::Format(_("Playing %s"), game);
		}
		if (!f.console.empty())
			return wxString::Format(_("On %s"), OpenPakUI::ConsoleName(f.console));
		return _("Online");
	}

	void Build()
	{
		ClearBody();
		const auto& req = m_data.requests;
		m_window->SetFriendsBadge(req.ok ? req.incoming.size() : 0);
		if (req.ok && (!req.incoming.empty() || !req.outgoing.empty()))
		{
			AddHeading(_("Requests"));
			for (const auto& r : req.incoming)
			{
				Row row = AddRow();
				row.lines->Add(Text(row.panel, wxString::FromUTF8(r.display_name), true));
				row.lines->Add(Text(row.panel, _("Wants to be your friend"), false, true));
				auto* accept = new wxButton(row.panel, wxID_ANY, _("Accept"));
				const std::string id = r.account_id;
				accept->Bind(wxEVT_BUTTON, [this, id](wxCommandEvent&) { Act([id]() { return OpenPakSocial::AcceptFriend(id); }); });
				auto* decline = new wxButton(row.panel, wxID_ANY, _("Decline"));
				decline->Bind(wxEVT_BUTTON, [this, id](wxCommandEvent&) { Act([id]() { return OpenPakSocial::DeclineFriend(id); }); });
				row.buttons->Add(accept, 0, wxRIGHT, FromDIP(4));
				row.buttons->Add(decline);
			}
			for (const auto& r : req.outgoing)
			{
				Row row = AddRow();
				row.lines->Add(Text(row.panel, wxString::FromUTF8(r.display_name), true));
				row.lines->Add(Text(row.panel, _("Waiting for them to accept"), false, true));
				auto* cancel = new wxButton(row.panel, wxID_ANY, _("Cancel request"));
				const std::string id = r.account_id;
				cancel->Bind(wxEVT_BUTTON, [this, id](wxCommandEvent&) { Act([id]() { return OpenPakSocial::RemoveFriend(id); }); });
				row.buttons->Add(cancel);
			}
		}

		AddHeading(_("Friends"));
		auto friends = m_data.friends.friends;
		std::sort(friends.begin(), friends.end(), [](const OpenPakSocial::Friend& a, const OpenPakSocial::Friend& b) {
			if (a.online != b.online)
				return a.online;
			return wxString::FromUTF8(a.display_name).CmpNoCase(wxString::FromUTF8(b.display_name)) < 0;
		});
		if (friends.empty())
			AddEmpty(_("No friends yet. Add someone by their friend code."));
		for (const auto& f : friends)
		{
			Row row = AddRow();
			auto* nameLine = new wxBoxSizer(wxHORIZONTAL);
			nameLine->Add(Dot(row.panel, f.online ? kGreen : kGrey), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
			nameLine->Add(Text(row.panel, wxString::FromUTF8(f.display_name), true), 0, wxALIGN_CENTER_VERTICAL);
			row.lines->Add(nameLine);
			row.lines->Add(Text(row.panel, StatusLine(f), false, true));
			if (f.online && !f.since.empty())
				row.lines->Add(Text(row.panel, wxString::Format(_("Online since %s"), OpenPakUI::LocalTime(f.since)), false, true));

			const std::string id = f.account_id;
			const wxString name = wxString::FromUTF8(f.display_name);
			auto* remove = new wxButton(row.panel, wxID_ANY, _("Remove"));
			remove->SetToolTip(_("Remove"));
			remove->Bind(wxEVT_BUTTON, [this, id, name](wxCommandEvent&) {
				if (OpenPakUI::Confirm(wxGetTopLevelParent(this), _("Remove friend"), wxString::Format(_("Remove %s from your friends?"), name), _("Remove")))
					Act([id]() { return OpenPakSocial::RemoveFriend(id); });
			});
			auto* block = new wxButton(row.panel, wxID_ANY, _("Block"));
			block->SetToolTip(_("Block"));
			block->Bind(wxEVT_BUTTON, [this, id, name](wxCommandEvent&) {
				if (OpenPakUI::Confirm(wxGetTopLevelParent(this), wxString::Format(_("Block %s?"), name),
						_("They will not appear on your list, cannot send you anything, and cannot see you."), _("Block")))
					Act([id]() { return OpenPakSocial::BlockFriend(id); });
			});
			row.buttons->Add(remove, 0, wxRIGHT, FromDIP(4));
			row.buttons->Add(block);
		}
		FinishBody();
	}

	wxTextCtrl* m_code;
	wxButton* m_add;
	Data m_data;
};

// ---- Invitations ----------------------------------------------------------------------------

class InvitationsPage : public OpenPakPage
{
  public:
	InvitationsPage(OpenPakWindow* window, wxWindow* parent) : OpenPakPage(window, parent, _("Invitations"), true)
	{
		// Wii U: read-only; the game that sent an invitation accepts it.
		m_top->Add(Text(this, _("Accept these inside the game that sent them."), false, true), 0, wxBOTTOM, FromDIP(6));
	}

  protected:
	void Load() override
	{
		Run([]() {
				auto list = OpenPakSocial::GetInvitations();
				OpenPakSocial::GetCatalogue();
				return list;
			},
			[this](const OpenPakSocial::InvitationList& list) {
				if (!list.ok)
				{
					Status(list.error);
					return;
				}
				ClearBody();
				if (list.invitations.empty())
					AddEmpty(_("Nothing waiting."));
				for (const auto& inv : list.invitations)
				{
					Row row = AddRow();
					row.lines->Add(Text(row.panel, GameOrId(inv.title_id), true));
					row.lines->Add(Text(row.panel, wxString::Format(_("From %s"), wxString::FromUTF8(inv.from))));
					row.lines->Add(Text(row.panel, wxString::Format(_("Expires %s"), OpenPakUI::LocalTime(inv.expires_at)), false, true));
				}
				FinishBody();
			});
	}
};

// ---- Cloud saves ----------------------------------------------------------------------------

class SavesPage : public OpenPakPage
{
  public:
	SavesPage(OpenPakWindow* window, wxWindow* parent) : OpenPakPage(window, parent, _("Cloud saves"), true)
	{
		m_usage = Text(this, wxEmptyString, false, true);
		m_top->Add(m_usage, 0, wxBOTTOM, FromDIP(4));
		// S-1 (Wii U save sync) has no backend in Cemu yet: say so rather than offer buttons
		// that do nothing.
		m_top->Add(Text(this, _("Cemu does not upload or download Wii U saves yet. This is what your account holds in the cloud."), false, true),
			0, wxBOTTOM, FromDIP(6));
	}

  protected:
	void Load() override
	{
		Run([]() {
				auto saves = OpenPakSocial::GetCloudSaves();
				OpenPakSocial::GetCatalogue();
				return saves;
			},
			[this](const OpenPakSocial::CloudSaves& saves) {
				if (!saves.ok)
				{
					Status(saves.error);
					return;
				}
				m_usage->SetLabel(wxString::Format(_("Used: %s of %s"), wxFileName::GetHumanReadableSize(wxULongLong(saves.used)),
					wxFileName::GetHumanReadableSize(wxULongLong(saves.allowance))));
				ClearBody();
				size_t shown = 0;
				for (const auto& save : saves.saves)
				{
					if (save.platform != "wiiu")
						continue;
					++shown;
					Row row = AddRow();
					auto* title = new wxBoxSizer(wxHORIZONTAL);
					wxString name = wxString::FromUTF8(save.name);
					if (name.empty())
						name = GameOrId(save.title_id);
					title->Add(Text(row.panel, name, true), 0, wxALIGN_CENTER_VERTICAL);
					if (!save.versions.empty() && save.versions.front().conflict)
					{
						auto* pill = Text(row.panel, _("Conflict"), true);
						pill->SetForegroundColour(kRed);
						title->Add(pill, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
					}
					row.lines->Add(title);
					if (!save.versions.empty())
					{
						const auto& latest = save.versions.front();
						row.lines->Add(Text(row.panel, wxString::Format(_("In the cloud: %s · Versions: %s"),
							wxFileName::GetHumanReadableSize(wxULongLong(latest.size)), wxString::Format("%zu", save.versions.size()))));
						row.lines->Add(Text(row.panel, wxString::Format(_("Latest: version %s from %s, %s"), wxString::Format("%d", latest.number),
							latest.device.empty() ? OpenPakUI::None() : wxString::FromUTF8(latest.device), OpenPakUI::LocalTime(latest.saved_at)), false, true));
					}
					row.lines->Add(Text(row.panel, LocalLine(save.title_id), false, true));
				}
				if (shown == 0)
					AddEmpty(_("Nothing in the cloud yet."));
				FinishBody();
			});
	}

  private:
	// What this machine has for the title: the Mii account's save folder in the MLC.
	static wxString LocalLine(const std::string& titleIdHex)
	{
		const uint64 titleId = ParseTitleId(titleIdHex);
		TitleInfo info;
		if (titleId == 0 || !CafeTitleList::GetFirstByTitleId(titleId, info))
			return _("Not installed on this machine.");
		uint32 slot = OpenPakAccount::AppliedSlot();
		if (slot == 0)
			slot = ActiveSettings::GetPersistentId();
		const auto dir = ActiveSettings::GetMlcPath("usr/save/{:08x}/{:08x}/user/{:08x}", (uint32)(titleId >> 32), (uint32)titleId, slot);
		std::error_code ec;
		if (!std::filesystem::is_directory(dir, ec))
			return _("Nothing on this machine yet.");
		const auto written = std::filesystem::last_write_time(dir, ec);
		if (ec)
			return _("Nothing on this machine yet.");
		const auto sys = std::chrono::time_point_cast<std::chrono::seconds>(
			written - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
		const wxDateTime when((time_t)std::chrono::system_clock::to_time_t(sys));
		const wxUILocale& locale = wxUILocale::GetCurrent();
		return wxString::Format(_("On this machine: last written %s, never synced with the cloud."),
			when.Format(locale.GetInfo(wxLOCALE_SHORT_DATE_FMT) + " " + locale.GetInfo(wxLOCALE_TIME_FMT)));
	}

	wxStaticText* m_usage;
};

// ---- Mods -----------------------------------------------------------------------------------

class ModsPage : public OpenPakPage
{
  public:
	ModsPage(OpenPakWindow* window, wxWindow* parent) : OpenPakPage(window, parent, _("Mods"), false)
	{
		m_titles = new wxChoice(this, wxID_ANY);
		m_titles->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { LoadMods(); });
		m_top->Add(m_titles, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
		// M-1 (graphic packs from the catalogue) has no installer in Cemu yet.
		m_top->Add(Text(this, _("Installing graphic packs from OpenPak is not in this build yet. This is the catalogue for the title."), false, true),
			0, wxBOTTOM, FromDIP(6));
	}

  protected:
	struct Data
	{
		std::map<std::string, OpenPakSocial::CatalogueTitle> catalogue;
		std::vector<std::string> favourites;
	};

	void Load() override
	{
		Run([]() { return Data{OpenPakSocial::GetCatalogue(), OpenPakSocial::GetFavouriteModIds()}; },
			[this](const Data& data) {
				m_favourites = std::set<std::string>(data.favourites.begin(), data.favourites.end());
				FillTitles(data.catalogue);
				LoadMods();
			});
	}

  private:
	void FillTitles(const std::map<std::string, OpenPakSocial::CatalogueTitle>& catalogue)
	{
		std::string keep = m_selected;
		if (keep.empty() && CafeSystem::IsTitleRunning())
			keep = Upper(fmt::format("{:016x}", CafeSystem::GetForegroundTitleId()));
		std::map<std::string, wxString> titles; // id -> name
		for (const TitleId id : CafeTitleList::GetAllTitleIds())
		{
			if ((id >> 32) != 0x00050000) // base games only
				continue;
			const std::string hex = Upper(fmt::format("{:016x}", id));
			titles[hex] = GameOrId(hex);
		}
		for (const auto& [id, title] : catalogue)
		{
			if (title.console == "wiiu" && !titles.count(id))
				titles[id] = title.name.empty() ? wxString::FromUTF8(id) : wxString::FromUTF8(title.name);
		}
		std::vector<std::pair<wxString, std::string>> sorted;
		for (const auto& [id, name] : titles)
			sorted.emplace_back(name, id);
		std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first.CmpNoCase(b.first) < 0; });
		m_titles->Clear();
		m_ids.clear();
		int select = sorted.empty() ? wxNOT_FOUND : 0;
		for (const auto& [name, id] : sorted)
		{
			if (id == keep)
				select = (int)m_ids.size();
			m_titles->Append(name);
			m_ids.push_back(id);
		}
		m_titles->SetSelection(select);
		Layout();
	}

	void LoadMods()
	{
		const int sel = m_titles->GetSelection();
		if (sel == wxNOT_FOUND || sel >= (int)m_ids.size())
		{
			ClearBody();
			AddEmpty(_("No mods published for this title."));
			FinishBody();
			return;
		}
		m_selected = m_ids[sel];
		const std::string id = m_selected;
		Run([id]() { return OpenPakSocial::GetMods(id); },
			[this, id](const OpenPakSocial::ModList& list) {
				if (id != m_selected)
					return;
				if (!list.ok)
				{
					Status(list.error);
					return;
				}
				ClearBody();
				if (list.mods.empty())
					AddEmpty(_("No mods published for this title."));
				const bool signedIn = OpenPakAccount::IsSignedIn();
				for (const auto& mod : list.mods)
				{
					Row row = AddRow();
					row.lines->Add(Text(row.panel, wxString::FromUTF8(mod.name) + " " + wxString::FromUTF8(mod.version), true));
					if (!mod.summary.empty())
						row.lines->Add(Text(row.panel, wxString::FromUTF8(mod.summary)));
					row.lines->Add(Text(row.panel, wxString::Format(_("by %s, %s"), wxString::FromUTF8(mod.author),
						mod.licence.empty() ? OpenPakUI::None() : wxString::FromUTF8(mod.licence)), false, true));
					const bool fav = m_favourites.count(mod.id) != 0;
					auto* star = new wxButton(row.panel, wxID_ANY, wxString::FromUTF8(fav ? "\xE2\x98\x85" : "\xE2\x98\x86"),
						wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
					star->SetToolTip(_("Favourite"));
					star->Enable(signedIn);
					if (!signedIn)
						star->SetToolTip(_("Sign in to OpenPak to use this."));
					const std::string modId = mod.id;
					star->Bind(wxEVT_BUTTON, [this, modId, fav, star](wxCommandEvent&) {
						star->Enable(false);
						Run([modId, fav]() { return OpenPakSocial::SetModFavourite(modId, !fav); },
							[this, modId, fav](const std::string& error) {
								if (!error.empty())
								{
									Status(error);
									return;
								}
								if (fav)
									m_favourites.erase(modId);
								else
									m_favourites.insert(modId);
								LoadMods();
							});
					});
					row.buttons->Add(star);
				}
				FinishBody();
			});
	}

	wxChoice* m_titles;
	std::vector<std::string> m_ids;
	std::string m_selected;
	std::set<std::string> m_favourites;
};

// ---- News -----------------------------------------------------------------------------------

class NewsPage : public OpenPakPage
{
  public:
	NewsPage(OpenPakWindow* window, wxWindow* parent) : OpenPakPage(window, parent, _("News"), false) {}

  protected:
	void Load() override
	{
		if (m_built)
			return;
		m_built = true;
		ClearBody();
		// No link: OpenPak serves Miiverse to consoles only; there is no web Miiverse to open.
		AddNotHere(_("Wii U news lives in Miiverse, on the network."));
		FinishBody();
	}

  private:
	bool m_built = false;
};

// ---- Status ---------------------------------------------------------------------------------

class StatusPage : public OpenPakPage
{
  public:
	StatusPage(OpenPakWindow* window, wxWindow* parent) : OpenPakPage(window, parent, _("Status"), false) {}

  protected:
	struct Data
	{
		OpenPakSocial::ServiceStatus services;
		OpenPakSocial::Players players;
		std::string edgeHost;
		std::optional<int> edgeMs;
	};

	void Load() override
	{
		Run([]() {
				Data data;
				data.services = OpenPakSocial::GetServiceStatus();
				data.players = OpenPakSocial::GetPlayers();
				OpenPakSocial::GetCatalogue();
				// The console edge: the Wii U account server the emulated console talks to.
				const std::string act = OpenPakNetworkProfile::ServiceURL("act");
				std::string host = act;
				if (const auto scheme = host.find("://"); scheme != std::string::npos)
					host = host.substr(scheme + 3);
				if (const auto slash = host.find('/'); slash != std::string::npos)
					host = host.substr(0, slash);
				data.edgeHost = host;
				data.edgeMs = OpenPakSocial::PingUrl(act);
				return data;
			},
			[this](const Data& data) {
				m_data = data;
				m_refreshed = wxDateTime::Now();
				Build();
			});
	}

  private:
	void Build()
	{
		ClearBody();
		const auto& services = m_data.services;
		const auto& players = m_data.players;

		// Verdict card.
		auto* verdict = new wxBoxSizer(wxHORIZONTAL);
		wxColour colour = kGrey;
		if (services.ok)
			colour = services.state == "up" || services.state == "ok" ? kGreen : (services.state == "down" ? kRed : kAmber);
		verdict->Add(Dot(m_body, colour), 0, wxALIGN_TOP | wxRIGHT, FromDIP(8));
		auto* verdictLines = new wxBoxSizer(wxVERTICAL);
		if (services.ok)
		{
			verdictLines->Add(Heading(m_body, wxString::FromUTF8(services.headline)));
			if (!services.summary.empty())
				verdictLines->Add(Text(m_body, wxString::FromUTF8(services.summary)));
		}
		else
		{
			std::string host = services.url;
			if (const auto scheme = host.find("://"); scheme != std::string::npos)
				host = host.substr(scheme + 3);
			verdictLines->Add(Text(m_body, wxString::Format(_("The status box at %s did not answer."), wxString::FromUTF8(host))));
		}
		const wxUILocale& locale = wxUILocale::GetCurrent();
		verdictLines->Add(Text(m_body, wxString::Format(_("Refreshed %s"),
			m_refreshed.Format(locale.GetInfo(wxLOCALE_SHORT_DATE_FMT) + " " + locale.GetInfo(wxLOCALE_TIME_FMT))), false, true));
		verdictLines->Add(Text(m_body, wxString::Format(_("Players online: %s"),
			players.ok ? wxString::Format("%d", players.players_online) : OpenPakUI::None()), false, true));
		verdict->Add(verdictLines, 1, wxEXPAND);
		m_bodySizer->Add(verdict, 0, wxEXPAND | wxBOTTOM, FromDIP(12));

		auto* columns = new wxBoxSizer(wxHORIZONTAL);
		auto column = [&](const wxString& title) {
			auto* c = new wxBoxSizer(wxVERTICAL);
			c->Add(Heading(m_body, title), 0, wxBOTTOM, FromDIP(6));
			columns->Add(c, 1, wxEXPAND | wxRIGHT, FromDIP(12));
			return c;
		};

		// Services.
		auto* svc = column(_("Services"));
		for (const auto& s : services.services)
		{
			auto* line = new wxBoxSizer(wxHORIZONTAL);
			line->Add(Dot(m_body, s.up ? kGreen : kRed), 0, wxRIGHT, FromDIP(6));
			auto* lines = new wxBoxSizer(wxVERTICAL);
			lines->Add(Text(m_body, wxString::FromUTF8(s.name) + "  " + (s.up ? _("Up") : _("Down")), false, false, 200));
			lines->Add(Text(m_body, wxString::Format(_("%s%% up, %s"), wxString::Format(s.uptime < 100 ? "%.1f" : "%.0f", s.uptime),
				wxString::FromUTF8(s.latency)), false, true, 200));
			line->Add(lines, 1);
			svc->Add(line, 0, wxBOTTOM, FromDIP(4));
		}
		if (services.services.empty())
			svc->Add(Text(m_body, OpenPakUI::None(), false, true));

		// This session.
		auto* session = column(_("This session"));
		auto pair = [&](const wxString& label, const wxString& value) {
			session->Add(Text(m_body, label, true, false, 220));
			session->Add(Text(m_body, value, false, true, 220), 0, wxBOTTOM, FromDIP(6));
		};
		const bool signedIn = OpenPakAccount::IsSignedIn();
		pair(_("OpenPak account"), signedIn ? wxString::Format(_("Signed in as %s"), OpenPakUI::SignedInName()) : _("Not signed in"));
		const std::string pnid = OpenPakAccount::GetUsername();
		pair(_("Console link"), signedIn && OpenPakAccount::AppliedSlot() != 0
			? wxString::Format(_("Linked as %s (%s)"), wxString::FromUTF8(pnid), "Wii U")
			: _("No console account is linked."));
		pair(_("Console edge"), m_data.edgeMs
			? wxString::Format(_("%s answered in %s ms"), wxString::FromUTF8(m_data.edgeHost), wxString::Format("%d", *m_data.edgeMs))
			: wxString::Format(_("%s did not answer"), wxString::FromUTF8(m_data.edgeHost)));
		pair(_("Presence"), OpenPakUI::IsGameRunning() && OpenPakUI::IsOn() && signedIn
			? wxString::Format(_("Published as %s"), wxString::FromUTF8(pnid))
			: _("Nothing published yet."));
		session->Add(Text(m_body, _("Ping"), true, false, 220));
		m_ping = Text(m_body, m_pingText.empty() ? _("Not tested") : m_pingText, false, true, 220);
		session->Add(m_ping, 0, wxBOTTOM, FromDIP(4));
		auto* test = new wxButton(m_body, wxID_ANY, _("Test connection"));
		const auto sinceTest = std::chrono::steady_clock::now() - m_lastTest;
		test->Enable(sinceTest > std::chrono::seconds(10));
		test->Bind(wxEVT_BUTTON, [this, test](wxCommandEvent&) {
			m_lastTest = std::chrono::steady_clock::now();
			test->Enable(false);
			m_ping->SetLabel(_("Checking..."));
			Run([]() { return OpenPakSocial::PingWebsite(); },
				[this](const std::optional<int>& ms) {
					m_pingText = ms ? wxString::Format("%d ms", *ms) : wxString::Format(_("%s did not answer"), OpenPakUI::WebsiteUrl());
					Build();
				});
		});
		session->Add(test, 0);

		// Players.
		auto* playersColumn = column(_("Players"));
		if (players.ok)
		{
			for (const auto& [title, count] : players.titles)
				playersColumn->Add(Text(m_body, GameOrId(Upper(title)) + wxString::Format(": %d", count), false, false, 200));
			for (const auto& [ns, count] : players.networks)
				playersColumn->Add(Text(m_body, OpenPakUI::ConsoleName(ns) + wxString::Format(": %d", count), false, true, 200));
		}
		if (!players.ok || (players.titles.empty() && players.networks.empty()))
			playersColumn->Add(Text(m_body, OpenPakUI::None(), false, true));

		m_bodySizer->Add(columns, 0, wxEXPAND);
		FinishBody();
	}

	Data m_data;
	wxDateTime m_refreshed = wxDateTime::Now();
	wxStaticText* m_ping = nullptr;
	wxString m_pingText;
	std::chrono::steady_clock::time_point m_lastTest{};
};

// ---- the window -----------------------------------------------------------------------------

OpenPakWindow::OpenPakWindow(wxWindow* parent, OpenPakUI::Page page)
	: wxDialog(parent, wxID_ANY, _("OpenPak"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	s_current = this;
	auto* sizer = new wxBoxSizer(wxVERTICAL);
	m_book = new wxListbook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLB_LEFT);
	m_pages = {
		new AccountPage(this, m_book),
		new FriendsPage(this, m_book),
		new InvitationsPage(this, m_book),
		new SavesPage(this, m_book),
		new ModsPage(this, m_book),
		new NewsPage(this, m_book),
		new StatusPage(this, m_book),
	};
	const wxString titles[] = {_("Account"), _("Friends"), _("Invitations"), _("Cloud saves"), _("Mods"), _("News"), _("Status")};
	for (size_t i = 0; i < m_pages.size(); ++i)
		m_book->AddPage(m_pages[i], titles[i]);
	sizer->Add(m_book, 1, wxEXPAND | wxALL, FromDIP(6));

	auto* footer = new wxBoxSizer(wxHORIZONTAL);
	m_gauge = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, FromDIP(wxSize(90, 6)));
	m_gauge->Hide();
	footer->Add(m_gauge, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
	m_status = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
	footer->Add(m_status, 1, wxALIGN_CENTER_VERTICAL);
	auto* close = new wxButton(this, wxID_CLOSE, _("Close"));
	close->SetDefault();
	close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); });
	footer->Add(close, 0, wxALL, FromDIP(8));
	sizer->Add(footer, 0, wxEXPAND);
	SetSizer(sizer);
	SetEscapeId(wxID_CLOSE);
	SetAffirmativeId(wxID_CLOSE);

	SetMinSize(FromDIP(wxSize(760, 480)));
	SetSize(FromDIP(wxSize(880, 600)));
	CentreOnParent();

	constexpr int kPulseTimer = wxID_HIGHEST + 1, kPollTimer = wxID_HIGHEST + 2;
	m_pulse.SetOwner(this, kPulseTimer);
	m_poll.SetOwner(this, kPollTimer);
	Bind(wxEVT_TIMER, [this](wxTimerEvent&) { m_gauge->Pulse(); }, kPulseTimer);
	Bind(wxEVT_TIMER, [this](wxTimerEvent&) { OnPollTimer(); }, kPollTimer);
	m_poll.Start(30 * 1000);

	m_book->Bind(wxEVT_LISTBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& e) {
		e.Skip();
		if (e.GetEventObject() == m_book)
			OnPageChanged();
	});
	// Ctrl+PgUp / Ctrl+PgDn switch pages.
	Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
		if (e.ControlDown() && (e.GetKeyCode() == WXK_PAGEUP || e.GetKeyCode() == WXK_PAGEDOWN))
		{
			m_book->AdvanceSelection(e.GetKeyCode() == WXK_PAGEDOWN);
			return;
		}
		e.Skip();
	});

	m_listener = OpenPakUI::AddAccountListener([this]() { ReloadAll(); });

	ShowPage(page);
}

OpenPakWindow::~OpenPakWindow()
{
	OpenPakUI::RemoveAccountListener(m_listener);
	m_pulse.Stop();
	m_poll.Stop();
	if (s_current == this)
		s_current = nullptr;
}

void OpenPakWindow::ShowPage(OpenPakUI::Page page)
{
	const int index = (int)page;
	if (index < 0 || index >= (int)m_pages.size())
		return;
	if (m_book->GetSelection() == index)
		OnPageChanged();
	else
		m_book->SetSelection(index); // sends PAGE_CHANGED
}

void OpenPakWindow::OnPageChanged()
{
	const int index = m_book->GetSelection();
	if (index >= 0 && index < (int)m_pages.size())
		m_pages[index]->Reload(); // a page loads when it is shown
}

void OpenPakWindow::OnPollTimer()
{
	// Not under an open confirmation or sign-in: a rebuild would pull its row away.
	if (OpenPakUI::PromptOpen())
		return;
	// Friends and Invitations refresh every 30 s while the window is open; Status while shown.
	const int current = m_book->GetSelection();
	for (const auto page : {OpenPakUI::Page::Friends, OpenPakUI::Page::Invitations})
	{
		auto* p = m_pages[(int)page];
		if (p->WasLoaded() || current == (int)page)
			p->Reload();
	}
	if (current == (int)OpenPakUI::Page::Status)
		m_pages[current]->Reload();
}

void OpenPakWindow::ReloadAll()
{
	// Signing in or out reloads every page that has been shown; the current one at once.
	const int current = m_book->GetSelection();
	for (size_t i = 0; i < m_pages.size(); ++i)
	{
		if ((int)i == current || m_pages[i]->WasLoaded())
			m_pages[i]->Reload();
	}
	if (!OpenPakAccount::IsSignedIn())
		SetFriendsBadge(0);
}

void OpenPakWindow::BeginBusy()
{
	if (m_busy++ == 0)
	{
		m_gauge->Show();
		m_gauge->Pulse();
		m_pulse.Start(100);
		Layout();
	}
}

void OpenPakWindow::EndBusy()
{
	if (m_busy > 0 && --m_busy == 0)
	{
		m_pulse.Stop();
		m_gauge->SetValue(0);
		m_gauge->Hide();
		Layout();
	}
}

void OpenPakWindow::SetStatus(const wxString& text)
{
	m_status->SetLabel(text);
	m_status->SetToolTip(text);
}

void OpenPakWindow::SetFriendsBadge(size_t incoming)
{
	wxString label = _("Friends");
	if (incoming > 0)
		label += incoming > 99 ? wxString(" (99+)") : wxString::Format(" (%zu)", incoming);
	m_book->SetPageText((size_t)OpenPakUI::Page::Friends, label);
}
