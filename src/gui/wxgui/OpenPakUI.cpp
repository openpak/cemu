#include "wxgui/OpenPakUI.h"

#include "Cemu/OpenPak/Account.h"
#include "Cemu/OpenPak/Errors.h"
#include "Cemu/OpenPak/NetworkProfile.h"
#include "Cemu/OpenPak/Prefs.h"
#include "Cemu/OpenPak/Social.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/TitleList/TitleList.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"

#include <wx/activityindicator.h>
#include <wx/app.h>
#include <wx/button.h>
#include <wx/dcmemory.h>
#include <wx/dialog.h>
#include <wx/frame.h>
#include <wx/hyperlink.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/uilocale.h>
#include <wx/utils.h>
#include <wx/weakref.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <thread>

namespace
{
	OpenPakUI::Host g_host;
	int g_prompts = 0; // open confirmations and sign-in dialogs

	struct PromptScope
	{
		PromptScope() { ++g_prompts; }
		~PromptScope() { --g_prompts; }
	};
	std::map<int, std::function<void()>> g_listeners;
	int g_nextListener = 1;

	// The account card, as the website names it; filled off the UI thread after sign-in.
	wxString g_profileName;
	wxImage g_avatar;

	wxString WebsiteHost()
	{
		std::string base = OpenPakPrefs::ApiBase();
		if (const auto scheme = base.find("://"); scheme != std::string::npos)
			base = base.substr(scheme + 3);
		return wxString::FromUTF8(base);
	}

	wxImage RoundImage(const wxImage& source, int size)
	{
		wxImage image = source.Scale(size, size, wxIMAGE_QUALITY_HIGH);
		if (!image.HasAlpha())
			image.InitAlpha();
		const double r = size / 2.0;
		for (int y = 0; y < size; ++y)
		{
			for (int x = 0; x < size; ++x)
			{
				const double dx = x + 0.5 - r, dy = y + 0.5 - r;
				if (dx * dx + dy * dy > r * r)
					image.SetAlpha(x, y, 0);
			}
		}
		return image;
	}

	// ---- sign-in dialog (§3.3) ----------------------------------------------------------

	class SignInDialog : public wxDialog
	{
	  public:
		SignInDialog(wxWindow* parent, const wxString& intro)
			: wxDialog(parent, wxID_ANY, _("Sign in to OpenPak"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
		{
			auto* sizer = new wxBoxSizer(wxVERTICAL);
			if (!intro.empty())
			{
				auto* text = new wxStaticText(this, wxID_ANY, intro);
				text->Wrap(FromDIP(420));
				sizer->Add(text, 0, wxALL, FromDIP(8));
			}
			m_error = new wxStaticText(this, wxID_ANY, wxEmptyString);
			m_error->SetForegroundColour(wxColour(0xE0, 0x39, 0x3E));
			m_error->Hide();
			sizer->Add(m_error, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

			auto* form = new wxFlexGridSizer(0, 2, FromDIP(6), FromDIP(8));
			form->AddGrowableCol(1, 1);
			form->Add(new wxStaticText(this, wxID_ANY, _("Email")), 0, wxALIGN_CENTER_VERTICAL);
			m_email = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(300, -1)));
			form->Add(m_email, 1, wxEXPAND);
			form->Add(new wxStaticText(this, wxID_ANY, _("Password")), 0, wxALIGN_CENTER_VERTICAL);
			m_password = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
			form->Add(m_password, 1, wxEXPAND);
			form->Add(new wxStaticText(this, wxID_ANY, _("Device name")), 0, wxALIGN_CENTER_VERTICAL);
			wxString device = wxString::FromUTF8(OpenPakPrefs::DeviceName());
			if (device.empty())
				device = wxString::Format(_("%s on %s"), "Cemu", wxGetHostName());
			m_device = new wxTextCtrl(this, wxID_ANY, device);
			form->Add(m_device, 1, wxEXPAND);
			form->AddSpacer(0);
			auto* hint = new wxStaticText(this, wxID_ANY, _("How this machine appears in your account's device list."));
			hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
			hint->Wrap(FromDIP(300));
			form->Add(hint, 0);
			sizer->Add(form, 0, wxEXPAND | wxALL, FromDIP(8));

			sizer->Add(new wxHyperlinkCtrl(this, wxID_ANY, _("Create an account"), OpenPakUI::WebsiteUrl() + "/register"),
				0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

			auto* footer = new wxBoxSizer(wxHORIZONTAL);
			m_busy = new wxActivityIndicator(this, wxID_ANY);
			m_busy->Hide();
			footer->Add(m_busy, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
			auto* buttons = new wxStdDialogButtonSizer();
			m_submit = new wxButton(this, wxID_OK, _("Sign in"));
			m_cancel = new wxButton(this, wxID_CANCEL, _("Cancel"));
			buttons->AddButton(m_submit);
			buttons->AddButton(m_cancel);
			buttons->Realize();
			footer->Add(buttons, 1, wxEXPAND);
			sizer->Add(footer, 0, wxEXPAND | wxALL, FromDIP(8));
			m_submit->SetDefault();

			SetSizerAndFit(sizer);
			CentreOnParent();

			m_email->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { UpdateReady(); });
			m_password->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { UpdateReady(); });
			m_submit->Bind(wxEVT_BUTTON, &SignInDialog::OnSubmit, this);
			Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
				if (m_running && event.CanVeto())
					event.Veto();
				else
					event.Skip();
			});
			UpdateReady();
			(m_email->GetValue().empty() ? m_email : m_password)->SetFocus();
		}

		bool Linked() const { return m_ok; }

	  private:
		void UpdateReady()
		{
			m_submit->Enable(!m_running && !m_email->GetValue().Strip(wxString::both).empty() && !m_password->GetValue().empty());
			m_cancel->Enable(!m_running);
		}

		void ShowError(const wxString& text)
		{
			m_error->SetLabel(text);
			m_error->Wrap(FromDIP(420));
			m_error->Show(!text.empty());
			GetSizer()->Layout();
			Fit();
		}

		void OnSubmit(wxCommandEvent&)
		{
			if (m_running || !m_submit->IsEnabled())
				return;
			m_running = true;
			ShowError({});
			UpdateReady();
			m_busy->Show();
			m_busy->Start();
			GetSizer()->Layout();
			const std::string email = m_email->GetValue().Strip(wxString::both).ToStdString(wxConvUTF8);
			const std::string password = m_password->GetValue().ToStdString(wxConvUTF8);
			const std::string device = m_device->GetValue().Strip(wxString::both).ToStdString(wxConvUTF8);
			wxWeakRef<SignInDialog> self(this);
			std::thread([self, email, password, device]() {
				const auto result = OpenPakAccount::SignIn(email, password, device);
				wxTheApp->CallAfter([self, result, device]() {
					if (!self)
						return;
					self->m_running = false;
					self->m_busy->Stop();
					self->m_busy->Hide();
					if (result.ok)
					{
						OpenPakPrefs::SetDeviceName(device);
						self->m_password->Clear();
						self->m_ok = true;
						self->EndModal(wxID_OK);
						return;
					}
					self->ShowError(OpenPakUI::ErrorText(result.error));
					self->UpdateReady();
					self->m_password->SetFocus();
					self->m_password->SelectAll();
				});
			}).detach();
		}

		wxStaticText* m_error;
		wxTextCtrl* m_email;
		wxTextCtrl* m_password;
		wxTextCtrl* m_device;
		wxButton* m_submit;
		wxButton* m_cancel;
		wxActivityIndicator* m_busy;
		bool m_running = false;
		bool m_ok = false;
	};

	// ---- toasts (§3.10) -----------------------------------------------------------------

	constexpr int kToastMs = 6000;
	constexpr size_t kToastMax = 4;

	class ToastCard;
	std::vector<ToastCard*> g_cards;
	void LayoutToasts();

	class ToastCard : public wxPanel
	{
	  public:
		ToastCard(wxWindow* parent, const wxString& category, const wxString& text, OpenPakUI::ToastTarget target)
			: wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE), m_target(target)
		{
			SetBackgroundColour(wxColour(0x26, 0x28, 0x2C));
			auto* sizer = new wxBoxSizer(wxVERTICAL);
			auto* cat = new wxStaticText(this, wxID_ANY, category);
			wxFont small = cat->GetFont();
			small.SetPointSize(std::max(6, small.GetPointSize() - 2));
			small.SetWeight(wxFONTWEIGHT_BOLD);
			cat->SetFont(small);
			cat->SetForegroundColour(wxColour(0x9A, 0xA0, 0xA6));
			sizer->Add(cat, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
			auto* line = new wxStaticText(this, wxID_ANY, text);
			line->SetForegroundColour(*wxWHITE);
			line->Wrap(FromDIP(300));
			sizer->Add(line, 0, wxALL, FromDIP(10));
			SetSizerAndFit(sizer);
			SetSize(FromDIP(320), GetBestSize().y);

			for (wxWindow* w : {static_cast<wxWindow*>(this), static_cast<wxWindow*>(cat), static_cast<wxWindow*>(line)})
			{
				w->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { CallAfter(&ToastCard::Activate); });
				w->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) { m_timer.Stop(); e.Skip(); });
				w->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { m_timer.StartOnce(kToastMs / 2); e.Skip(); });
				w->SetCursor(wxCursor(wxCURSOR_HAND));
			}
			m_timer.SetOwner(this);
			Bind(wxEVT_TIMER, [this](wxTimerEvent&) { Dismiss(); });
			m_timer.StartOnce(kToastMs);
		}

		void Dismiss()
		{
			m_timer.Stop();
			g_cards.erase(std::remove(g_cards.begin(), g_cards.end(), this), g_cards.end());
			Destroy();
			LayoutToasts();
		}

	  private:
		void Activate()
		{
			const auto target = m_target;
			Dismiss();
			const auto& host = OpenPakUI::GetHost();
			switch (target)
			{
			case OpenPakUI::ToastTarget::Account:
				if (host.openWindow) host.openWindow(OpenPakUI::Page::Account);
				break;
			case OpenPakUI::ToastTarget::Friends:
				if (host.openWindow) host.openWindow(OpenPakUI::Page::Friends);
				break;
			case OpenPakUI::ToastTarget::Invitations:
				if (host.openWindow) host.openWindow(OpenPakUI::Page::Invitations);
				break;
			case OpenPakUI::ToastTarget::SignIn:
				if (host.frame && !OpenPakUI::IsGameRunning())
					OpenPakUI::SignIn(host.frame);
				break;
			default:
				break;
			}
		}

		OpenPakUI::ToastTarget m_target;
		wxTimer m_timer;
	};

	void LayoutToasts()
	{
		wxFrame* frame = g_host.frame;
		if (!frame)
			return;
		const wxSize client = frame->GetClientSize();
		const int margin = frame->FromDIP(12);
		const auto corner = OpenPakPrefs::NotificationCorner();
		const bool right = corner == OpenPakPrefs::Corner::BottomRight || corner == OpenPakPrefs::Corner::TopRight;
		const bool bottom = corner == OpenPakPrefs::Corner::BottomRight || corner == OpenPakPrefs::Corner::BottomLeft;
		int offset = margin;
		// newest nearest the corner
		for (auto it = g_cards.rbegin(); it != g_cards.rend(); ++it)
		{
			ToastCard* card = *it;
			const wxSize size = card->GetSize();
			const int x = right ? client.x - size.x - margin : margin;
			const int y = bottom ? client.y - size.y - offset : offset;
			card->Move(x, y);
			card->Raise();
			offset += size.y + margin / 2;
		}
	}

	// ---- friends poller (toasts for friends coming online, requests, invitations) --------

	class Poller : public wxEvtHandler
	{
	  public:
		Poller()
		{
			m_timer.SetOwner(this);
			Bind(wxEVT_TIMER, [this](wxTimerEvent&) { Poll(); });
		}

		void Start()
		{
			if (!m_timer.IsRunning())
				m_timer.Start(45 * 1000);
			Poll();
		}

		void Stop()
		{
			m_timer.Stop();
			Reset();
		}

		void Reset()
		{
			m_primed = false;
			m_online.clear();
			m_incoming.clear();
			m_invitations.clear();
			++m_generation;
		}

	  private:
		void Poll()
		{
			if (m_busy || !OpenPakAccount::IsSignedIn())
				return;
			m_busy = true;
			const uint64 generation = m_generation;
			std::thread([this, generation]() {
				auto friends = OpenPakSocial::GetFriends();
				auto requests = OpenPakSocial::GetRequests();
				auto invitations = OpenPakSocial::GetInvitations();
				OpenPakSocial::GetCatalogue(); // names for the toasts
				wxTheApp->CallAfter([this, generation, friends, requests, invitations]() {
					m_busy = false;
					if (generation != m_generation)
						return;
					Apply(friends, requests, invitations);
				});
			}).detach();
		}

		void Apply(const OpenPakSocial::FriendList& friends, const OpenPakSocial::RequestList& requests,
				   const OpenPakSocial::InvitationList& invitations)
		{
			const bool announce = m_primed; // the first poll after start or sign-in is silent
			if (friends.ok)
			{
				std::map<std::string, std::string> online;
				for (const auto& f : friends.friends)
				{
					if (!f.online)
						continue;
					online[f.account_id] = f.title_id;
					const auto before = m_online.find(f.account_id);
					const wxString name = wxString::FromUTF8(f.display_name);
					if (!announce)
						continue;
					const wxString game = OpenPakUI::GameName(f.title_id);
					if (before == m_online.end())
					{
						if (!game.empty())
							OpenPakUI::Toast(_("FRIEND ONLINE"), wxString::Format(_("%s is playing %s"), name, game), OpenPakUI::ToastTarget::Friends);
						else
							OpenPakUI::Toast(_("FRIEND ONLINE"), wxString::Format(_("%s is online"), name), OpenPakUI::ToastTarget::Friends);
					}
					else if (!f.title_id.empty() && before->second != f.title_id && !game.empty())
					{
						OpenPakUI::Toast(_("FRIEND ONLINE"), wxString::Format(_("%s is playing %s"), name, game), OpenPakUI::ToastTarget::Friends);
					}
				}
				m_online = std::move(online);
			}
			if (requests.ok)
			{
				std::set<std::string> incoming;
				for (const auto& r : requests.incoming)
				{
					incoming.insert(r.account_id);
					if (announce && !m_incoming.count(r.account_id))
						OpenPakUI::Toast(_("FRIEND REQUEST"), wxString::Format(_("%s wants to be your friend"), wxString::FromUTF8(r.display_name)),
							OpenPakUI::ToastTarget::Friends);
				}
				m_incoming = std::move(incoming);
			}
			if (invitations.ok)
			{
				std::set<std::string> ids;
				for (const auto& inv : invitations.invitations)
				{
					ids.insert(inv.invitation_id);
					if (announce && !m_invitations.count(inv.invitation_id))
					{
						wxString game = OpenPakUI::GameName(inv.title_id);
						if (game.empty())
							game = OpenPakUI::None();
						OpenPakUI::Toast(_("GAME INVITE"), wxString::Format(_("%s invited you to %s"), wxString::FromUTF8(inv.from), game),
							OpenPakUI::ToastTarget::Invitations, true);
					}
				}
				m_invitations = std::move(ids);
			}
			m_primed = m_primed || friends.ok;
		}

		wxTimer m_timer;
		bool m_busy = false;
		bool m_primed = false;
		uint64 m_generation = 0;
		std::map<std::string, std::string> m_online; // account id -> title id
		std::set<std::string> m_incoming;
		std::set<std::string> m_invitations;
	};

	Poller* g_poller = nullptr;

	// ---- network re-check (docs/signed-ceiling.md, client rule 4) ------------------------

	// Every six hours, give or take ten per cent, the signed ceiling and the wiiu profile are
	// fetched again off the UI thread. A new effective set raises the change notice through
	// OpenPakNetworkProfile's listener.
	constexpr int kRecheckMs = 6 * 60 * 60 * 1000;

	class NetworkRecheck : public wxEvtHandler
	{
	  public:
		NetworkRecheck()
		{
			m_timer.SetOwner(this);
			Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
				OpenPakUI::RecheckNetwork();
				Schedule();
			});
		}

		void Schedule()
		{
			std::uniform_int_distribution<int> jitter(kRecheckMs / 10 * 9, kRecheckMs / 10 * 11);
			m_timer.StartOnce(jitter(m_rng));
		}

	  private:
		wxTimer m_timer;
		std::mt19937 m_rng{std::random_device{}()};
	};

	NetworkRecheck* g_recheck = nullptr;

	// days since 1970-01-01 for a civil date (Howard Hinnant's algorithm)
	int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
	{
		y -= m <= 2;
		const int64_t era = (y >= 0 ? y : y - 399) / 400;
		const unsigned yoe = (unsigned)(y - era * 400);
		const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
		const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
		return era * 146097 + (int64_t)doe - 719468;
	}
} // namespace

namespace OpenPakUI
{
	void SetHost(Host host)
	{
		g_host = std::move(host);
		if (g_host.frame)
			g_host.frame->Bind(wxEVT_SIZE, [](wxSizeEvent& event) {
				event.Skip();
				if (!g_cards.empty())
					wxTheApp->CallAfter([]() { LayoutToasts(); });
			});
	}

	const Host& GetHost() { return g_host; }

	int AddAccountListener(std::function<void()> listener)
	{
		const int id = g_nextListener++;
		g_listeners[id] = std::move(listener);
		return id;
	}

	void RemoveAccountListener(int id) { g_listeners.erase(id); }

	void NotifyAccountChanged()
	{
		if (!OpenPakAccount::IsSignedIn())
		{
			g_profileName.clear();
			g_avatar = wxImage();
		}
		if (g_poller)
			g_poller->Reset();
		if (g_host.accountChanged)
			g_host.accountChanged();
		// copy: a listener may remove itself
		const auto listeners = g_listeners;
		for (const auto& [id, listener] : listeners)
			listener();
	}

	bool IsGameRunning() { return CafeSystem::IsTitleRunning(); }
	bool IsOn() { return ActiveSettings::GetNetworkService() == NetworkService::OpenPak; }
	bool IsSignedIn() { return OpenPakAccount::IsSignedIn(); }

	wxString SignedInName()
	{
		if (!g_profileName.empty())
			return g_profileName;
		return wxString::FromUTF8(OpenPakAccount::GetUsername());
	}

	wxString WebsiteUrl() { return wxString::FromUTF8(OpenPakPrefs::ApiBase()); }

	void OpenWebsite(const wxString& path) { wxLaunchDefaultBrowser(WebsiteUrl() + path); }

	wxString None() { return wxString::FromUTF8("\xE2\x80\x94"); }

	wxString ErrorText(const std::string& error)
	{
		if (error.empty())
			return {};
		if (error == OpenPakError::Credentials)
			return _("Wrong email or password.");
		if (error == OpenPakError::RateLimited)
			return _("Too many attempts. Wait a minute and try again.");
		if (error == OpenPakError::Unreachable)
			return wxString::Format(_("Could not reach %s."), WebsiteHost());
		if (error == OpenPakError::NotSignedIn)
			return _("Sign in to OpenPak to use this.");
		if (error == OpenPakError::Expired)
			return wxString::Format(_("The OpenPak sign-in for %s has expired. Sign in again from the OpenPak menu."), SignedInName());
		if (error == OpenPakError::NoIdentity)
			return _("Signed in, but this emulator's console account did not link.");
		if (error.rfind(OpenPakError::ServerPrefix, 0) == 0)
			return wxString::Format(_("OpenPak said: %s"), wxString::FromUTF8(error.substr(OpenPakError::ServerPrefix.size())));
		return wxString::FromUTF8(error);
	}

	wxString LocalTime(const std::string& rfc3339)
	{
		// YYYY-MM-DDTHH:MM:SS[.fraction](Z|+HH:MM|-HH:MM)
		int Y, M, D, h, m, s;
		if (rfc3339.size() < 19 || sscanf(rfc3339.c_str(), "%4d-%2d-%2d%*c%2d:%2d:%2d", &Y, &M, &D, &h, &m, &s) != 6)
			return None();
		int64_t t = DaysFromCivil(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + m * 60 + s;
		size_t pos = 19;
		if (pos < rfc3339.size() && rfc3339[pos] == '.')
		{
			++pos;
			while (pos < rfc3339.size() && isdigit((unsigned char)rfc3339[pos]))
				++pos;
		}
		if (pos < rfc3339.size() && (rfc3339[pos] == '+' || rfc3339[pos] == '-'))
		{
			int oh = 0, om = 0;
			if (sscanf(rfc3339.c_str() + pos + 1, "%2d:%2d", &oh, &om) == 2)
				t -= (rfc3339[pos] == '+' ? 1 : -1) * (oh * 3600 + om * 60);
		}
		const wxDateTime when((time_t)t);
		const wxUILocale& locale = wxUILocale::GetCurrent();
		return when.Format(locale.GetInfo(wxLOCALE_SHORT_DATE_FMT) + " " + locale.GetInfo(wxLOCALE_TIME_FMT));
	}

	wxString GameName(const std::string& titleIdHex)
	{
		if (titleIdHex.empty())
			return {};
		uint64 titleId = 0;
		try
		{
			titleId = std::stoull(titleIdHex, nullptr, 16);
		}
		catch (...)
		{
			titleId = 0;
		}
		if (titleId != 0)
		{
			TitleInfo info;
			if (CafeTitleList::GetFirstByTitleId(titleId, info))
			{
				const std::string name = info.GetMetaTitleName();
				if (!name.empty())
					return wxString::FromUTF8(name);
			}
		}
		// Only what is already fetched: this runs on the UI thread.
		std::string upper = titleIdHex;
		std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return (char)std::toupper(c); });
		const auto catalogue = OpenPakSocial::GetCatalogueIfLoaded();
		if (const auto it = catalogue.find(upper); it != catalogue.end() && !it->second.name.empty())
			return wxString::FromUTF8(it->second.name);
		return {};
	}

	wxString ConsoleName(const std::string& ns)
	{
		if (ns == "wiiu")
			return "Wii U";
		if (ns == "3ds" || ns == "ctr")
			return "3DS";
		if (ns == "switch" || ns == "nx")
			return "Switch";
		if (ns == "wii")
			return "Wii";
		if (ns == "ds" || ns == "nds")
			return "DS";
		return wxString::FromUTF8(ns);
	}

	wxBitmap Avatar(int size)
	{
		if (!g_avatar.IsOk())
			return wxNullBitmap;
		return wxBitmap(RoundImage(g_avatar, size));
	}

	void LoadAvatar()
	{
		if (!OpenPakAccount::IsSignedIn())
			return;
		std::thread([]() {
			const auto profile = OpenPakSocial::GetProfile();
			std::optional<std::vector<uint8_t>> bytes;
			if (profile.ok)
				bytes = OpenPakSocial::GetAvatar(profile.account_id);
			wxTheApp->CallAfter([profile, bytes]() {
				if (!OpenPakAccount::IsSignedIn() || !profile.ok)
					return;
				g_profileName = wxString::FromUTF8(profile.display_name);
				if (bytes && !bytes->empty())
				{
					wxMemoryInputStream stream(bytes->data(), bytes->size());
					wxImage image;
					if (image.LoadFile(stream, wxBITMAP_TYPE_ANY) && image.IsOk())
						g_avatar = image;
				}
				if (g_host.accountChanged)
					g_host.accountChanged();
				const auto listeners = g_listeners;
				for (const auto& [id, listener] : listeners)
					listener();
			});
		}).detach();
	}

	std::string ApplyIdentity()
	{
		const std::string error = OpenPakAccount::ApplyIdentity();
		if (error.empty())
			ActiveSettings::Init(); // the synthesised device identity and the online check, now
		return error;
	}

	bool SignIn(wxWindow* parent, const wxString& intro)
	{
		if (IsGameRunning())
			return false;
		PromptScope prompt;
		SignInDialog dialog(parent, intro);
		if (dialog.ShowModal() != wxID_OK || !dialog.Linked())
			return false;
		// Into the OpenPak Mii account (§5.10), which also turns the Network Service to OpenPak.
		const std::string applyError = ApplyIdentity();
		if (!applyError.empty())
			cemuLog_log(LogType::Force, "OpenPak: signed in, but applying the identity failed: {}", applyError);
		const wxString name = SignedInName();
		if (applyError.empty())
			Toast(_("OPENPAK"), wxString::Format(_("Signed in as %s, and this console is now linked to your account."), name), ToastTarget::Account);
		else
			Toast(_("OPENPAK"), wxString::Format(_("Signed in as %s."), name), ToastTarget::Account);
		NotifyAccountChanged();
		LoadAvatar();
		StartPoller();
		RecheckNetwork(); // rule 4: re-check after an OpenPak sign-in
		return true;
	}

	bool PromptOpen() { return g_prompts > 0; }

	bool Confirm(wxWindow* parent, const wxString& title, const wxString& body, const wxString& confirmLabel)
	{
		PromptScope prompt;
		wxDialog dialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE);
		auto* sizer = new wxBoxSizer(wxVERTICAL);
		auto* heading = new wxStaticText(&dialog, wxID_ANY, title);
		heading->SetFont(heading->GetFont().Bold());
		sizer->Add(heading, 0, wxLEFT | wxRIGHT | wxTOP, dialog.FromDIP(12));
		if (!body.empty())
		{
			auto* text = new wxStaticText(&dialog, wxID_ANY, body);
			text->Wrap(dialog.FromDIP(400));
			sizer->Add(text, 0, wxALL, dialog.FromDIP(12));
		}
		auto* buttons = new wxStdDialogButtonSizer();
		auto* ok = new wxButton(&dialog, wxID_OK, confirmLabel);
		auto* cancel = new wxButton(&dialog, wxID_CANCEL, _("Cancel"));
		buttons->AddButton(ok);
		buttons->AddButton(cancel);
		buttons->Realize();
		sizer->Add(buttons, 0, wxEXPAND | wxALL, dialog.FromDIP(8));
		dialog.SetSizerAndFit(sizer);
		dialog.SetEscapeId(wxID_CANCEL);
		cancel->SetDefault();
		cancel->SetFocus();
		dialog.CentreOnParent();
		return dialog.ShowModal() == wxID_OK;
	}

	bool ConfirmAndSignOut(wxWindow* parent)
	{
		if (IsGameRunning() || !OpenPakAccount::IsSignedIn())
			return false;
		if (!Confirm(parent, _("Sign out of OpenPak?"),
				_("This emulator goes offline. Your friends and cloud saves stay on your account, and you can sign in again at any time."),
				_("Sign out")))
			return false;
		OpenPakAccount::SignOut();
		StopPoller();
		Toast(_("OPENPAK"), _("Signed out of OpenPak."), ToastTarget::None);
		NotifyAccountChanged();
		return true;
	}

	void MaybeAskToConnect(wxWindow* parent)
	{
		if (OpenPakPrefs::ConnectAsked())
			return;
		OpenPakPrefs::SetConnectAsked();
		if (OpenPakAccount::IsSignedIn() || IsGameRunning())
			return;

		wxDialog dialog(parent, wxID_ANY, _("Connect to OpenPak?"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE);
		auto* sizer = new wxBoxSizer(wxVERTICAL);
		auto* heading = new wxStaticText(&dialog, wxID_ANY, _("Connect to OpenPak?"));
		wxFont font = heading->GetFont().Bold();
		font.SetPointSize(font.GetPointSize() + 2);
		heading->SetFont(font);
		sizer->Add(heading, 0, wxLEFT | wxRIGHT | wxTOP, dialog.FromDIP(12));
		auto* body = new wxStaticText(&dialog, wxID_ANY,
			_("Sign in to OpenPak to play Wii U games online without a console dump: friends and cloud saves come with it. You can sign in later from the OpenPak menu."));
		body->Wrap(dialog.FromDIP(420));
		sizer->Add(body, 0, wxALL, dialog.FromDIP(12));
		sizer->Add(new wxHyperlinkCtrl(&dialog, wxID_ANY, _("Create an account"), WebsiteUrl() + "/register"),
			0, wxLEFT | wxRIGHT, dialog.FromDIP(12));
		auto* buttons = new wxStdDialogButtonSizer();
		auto* ok = new wxButton(&dialog, wxID_OK, _("Sign in"));
		buttons->AddButton(ok);
		buttons->AddButton(new wxButton(&dialog, wxID_CANCEL, _("Not now")));
		buttons->Realize();
		sizer->Add(buttons, 0, wxEXPAND | wxALL, dialog.FromDIP(8));
		dialog.SetSizerAndFit(sizer);
		ok->SetDefault();
		ok->SetFocus();
		dialog.CentreOnParent();
		if (dialog.ShowModal() != wxID_OK)
			return;
		if (SignIn(parent))
			OpenPakPrefs::SetCloudSync(true);
	}

	void CheckStoredSignIn()
	{
		if (!OpenPakAccount::IsSignedIn())
			return;
		const wxString name = SignedInName();
		std::thread([name]() {
			const bool dropped = OpenPakAccount::DropIfRejected();
			wxTheApp->CallAfter([dropped, name]() {
				if (dropped)
				{
					Toast(_("OPENPAK"), wxString::Format(_("The OpenPak sign-in for %s has expired. Sign in again from the OpenPak menu."), name),
						ToastTarget::SignIn);
					NotifyAccountChanged();
					return;
				}
				LoadAvatar();
				StartPoller();
			});
		}).detach();
	}

	void Toast(const wxString& category, const wxString& text, ToastTarget target, bool evenIfInactive)
	{
		if (!OpenPakPrefs::Notifications())
			return;
		wxFrame* frame = g_host.frame;
		if (!frame || frame->IsIconized())
			return;
		if (!evenIfInactive && !frame->IsActive() && !wxTheApp->IsActive())
			return;
		if (IsGameRunning())
		{
			// Over the game the render canvas covers every child window: Cemu's own overlay
			// notification shows it instead (its position is the overlay's setting).
			LatteOverlay_pushNotification((category + "\n" + text).utf8_string(), kToastMs);
			return;
		}
		while (g_cards.size() >= kToastMax)
			g_cards.front()->Dismiss(); // older ones leave first
		auto* card = new ToastCard(frame, category, text, target);
		g_cards.push_back(card);
		LayoutToasts();
		card->Show();
	}

	void StartPoller()
	{
		if (!g_poller)
			g_poller = new Poller();
		if (OpenPakAccount::IsSignedIn())
			g_poller->Start();
	}

	void StopPoller()
	{
		if (g_poller)
			g_poller->Stop();
	}

	void StartNetworkWatch()
	{
		OpenPakNetworkProfile::SetChangeListener([]() {
			if (!wxTheApp)
				return;
			wxTheApp->CallAfter([]() {
				// Only for someone using OpenPak: other Network Services never read these URLs.
				if (!IsOn())
					return;
				// Cemu reads the service URLs per request, so the new set is already in place
				// for anything started from now on; a running game keeps its sessions.
				Toast(_("OPENPAK"), _("OpenPak updated this system's network redirects. Restart the game to use them."),
					ToastTarget::None, true);
			});
		});
		if (!g_recheck)
		{
			g_recheck = new NetworkRecheck();
			g_recheck->Schedule();
		}
	}

	void RecheckNetwork()
	{
		std::thread([]() { OpenPakNetworkProfile::Refresh(); }).detach();
	}
} // namespace OpenPakUI
