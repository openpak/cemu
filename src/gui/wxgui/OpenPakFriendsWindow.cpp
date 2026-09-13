#include "OpenPakFriendsWindow.h"

#include "Cemu/Logging/CemuLogging.h"

#include <wx/button.h>
#include <wx/intl.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>

#include <chrono>

namespace
{
	// one thread-wide flag: never two refreshes racing the same lists
	std::atomic<uint64> g_refreshGeneration{0};
}

OpenPakFriendsWindow::OpenPakFriendsWindow(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, _("OpenPak friends"), wxDefaultPosition, wxSize(560, 520),
			   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	auto* notebook = new wxNotebook(this, wxID_ANY);

	// Friends
	auto* friendsPanel = new wxPanel(notebook);
	auto* friendsSizer = new wxBoxSizer(wxVERTICAL);
	m_friendsList = new wxListCtrl(friendsPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
		wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_NO_HEADER);
	m_friendsList->AppendColumn(_("Friend"), wxLIST_FORMAT_LEFT, 190);
	m_friendsList->AppendColumn(_("Status"), wxLIST_FORMAT_LEFT, 110);
	m_friendsList->AppendColumn(_("Playing"), wxLIST_FORMAT_LEFT, 160);
	m_friendsList->Bind(wxEVT_LIST_ITEM_SELECTED, &OpenPakFriendsWindow::OnSelectionChanged, this);
	m_friendsList->Bind(wxEVT_LIST_ITEM_DESELECTED, &OpenPakFriendsWindow::OnSelectionChanged, this);
	friendsSizer->Add(m_friendsList, 1, wxEXPAND | wxALL, 5);
	m_removeButton = new wxButton(friendsPanel, wxID_ANY, _("Remove friend"));
	m_removeButton->Bind(wxEVT_BUTTON, &OpenPakFriendsWindow::OnRemoveFriend, this);
	friendsSizer->Add(m_removeButton, 0, wxALIGN_RIGHT | wxALL, 5);
	friendsPanel->SetSizer(friendsSizer);
	notebook->AddPage(friendsPanel, _("Friends"));

	// Requests
	auto* requestsPanel = new wxPanel(notebook);
	auto* requestsSizer = new wxBoxSizer(wxVERTICAL);
	requestsSizer->Add(new wxStaticText(requestsPanel, wxID_ANY, _("Waiting for your answer")), 0, wxALL, 5);
	m_incomingList = new wxListCtrl(requestsPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
		wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_NO_HEADER);
	m_incomingList->AppendColumn(_("From"), wxLIST_FORMAT_LEFT, 190);
	m_incomingList->AppendColumn(_("Since"), wxLIST_FORMAT_LEFT, 160);
	m_incomingList->Bind(wxEVT_LIST_ITEM_SELECTED, &OpenPakFriendsWindow::OnSelectionChanged, this);
	m_incomingList->Bind(wxEVT_LIST_ITEM_DESELECTED, &OpenPakFriendsWindow::OnSelectionChanged, this);
	requestsSizer->Add(m_incomingList, 1, wxEXPAND | wxALL, 5);
	auto* requestButtons = new wxBoxSizer(wxHORIZONTAL);
	m_acceptButton = new wxButton(requestsPanel, wxID_ANY, _("Accept"));
	m_acceptButton->Bind(wxEVT_BUTTON, &OpenPakFriendsWindow::OnAcceptRequest, this);
	requestButtons->Add(m_acceptButton, 0, wxALL, 2);
	m_declineButton = new wxButton(requestsPanel, wxID_ANY, _("Decline"));
	m_declineButton->Bind(wxEVT_BUTTON, &OpenPakFriendsWindow::OnDeclineRequest, this);
	requestButtons->Add(m_declineButton, 0, wxALL, 2);
	requestsSizer->Add(requestButtons, 0, wxALIGN_RIGHT);
	requestsSizer->Add(new wxStaticText(requestsPanel, wxID_ANY, _("Sent by you, not yet answered")), 0, wxALL, 5);
	m_outgoingList = new wxListCtrl(requestsPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
		wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_NO_HEADER);
	m_outgoingList->AppendColumn(_("To"), wxLIST_FORMAT_LEFT, 190);
	m_outgoingList->AppendColumn(_("Sent"), wxLIST_FORMAT_LEFT, 160);
	requestsSizer->Add(m_outgoingList, 1, wxEXPAND | wxALL, 5);
	requestsPanel->SetSizer(requestsSizer);
	notebook->AddPage(requestsPanel, _("Requests"));

	// Invitations
	auto* invitationsPanel = new wxPanel(notebook);
	auto* invitationsSizer = new wxBoxSizer(wxVERTICAL);
	invitationsSizer->Add(new wxStaticText(invitationsPanel, wxID_ANY,
		_("Game invitations waiting for you. Accept them inside the game that sent them.")), 0, wxALL, 5);
	m_invitationsList = new wxListCtrl(invitationsPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
		wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_NO_HEADER);
	m_invitationsList->AppendColumn(_("From"), wxLIST_FORMAT_LEFT, 190);
	m_invitationsList->AppendColumn(_("Title id"), wxLIST_FORMAT_LEFT, 160);
	m_invitationsList->AppendColumn(_("Expires"), wxLIST_FORMAT_LEFT, 160);
	invitationsSizer->Add(m_invitationsList, 1, wxEXPAND | wxALL, 5);
	invitationsPanel->SetSizer(invitationsSizer);
	notebook->AddPage(invitationsPanel, _("Invitations"));

	auto* bottom = new wxBoxSizer(wxHORIZONTAL);
	m_status = new wxStaticText(this, wxID_ANY, _("Loading..."));
	bottom->Add(m_status, 1, wxALIGN_CENTRE_VERTICAL | wxLEFT, 10);
	m_refreshButton = new wxButton(this, wxID_ANY, _("Refresh"));
	m_refreshButton->Bind(wxEVT_BUTTON, &OpenPakFriendsWindow::OnRefresh, this);
	bottom->Add(m_refreshButton, 0, wxALL, 5);

	auto* topSizer = new wxBoxSizer(wxVERTICAL);
	topSizer->Add(notebook, 1, wxEXPAND);
	topSizer->Add(bottom, 0, wxEXPAND);
	SetSizer(topSizer);

	m_refreshTimer.SetOwner(this);
	m_refreshTimer.Start(30 * 1000); // presence and requests move; poll like the phone does
	m_refreshTimer.Bind(wxEVT_TIMER, &OpenPakFriendsWindow::OnRefreshTimer, this);

	Bind(wxEVT_CLOSE_WINDOW, &OpenPakFriendsWindow::OnClose, this);

	UpdateButtons();
	RefreshAsync();
}

OpenPakFriendsWindow::~OpenPakFriendsWindow()
{
	if (m_worker.joinable())
		m_worker.join();
}

void OpenPakFriendsWindow::OnClose(wxCloseEvent& event)
{
	if (m_worker.joinable())
		m_worker.join();
	event.Skip();
}

void OpenPakFriendsWindow::OnRefresh(wxCommandEvent& event)
{
	RefreshAsync();
}

void OpenPakFriendsWindow::OnRefreshTimer(wxTimerEvent& event)
{
	RefreshAsync();
}

void OpenPakFriendsWindow::RefreshAsync()
{
	if (m_refreshing.exchange(true))
		return;
	m_refreshButton->Enable(false);
	const uint64 generation = ++g_refreshGeneration;
	if (m_worker.joinable())
		m_worker.join();
	m_worker = std::thread([this, generation]() {
		const auto friends = OpenPakSocial::GetFriends();
		const auto requests = OpenPakSocial::GetRequests();
		const auto invitations = OpenPakSocial::GetInvitations();
		CallAfter([this, generation, friends, requests, invitations]() {
			if (generation != g_refreshGeneration.load())
				return; // a newer refresh is in flight
			RebuildLists(friends, requests, invitations);
			m_refreshing = false;
			m_refreshButton->Enable(true);
		});
	});
}

namespace
{
	void ClearList(wxListCtrl* list)
	{
		list->DeleteAllItems();
	}

	std::string SinceText(const std::string& since)
	{
		// RFC3339 → the time part; presence "since" is informational here
		if (since.size() >= 19)
			return since.substr(11, 8);
		return since;
	}
} // namespace

long OpenPakFriendsWindow::InsertFriend(wxListCtrl* list, const OpenPakSocial::Friend& f)
{
	const long index = list->InsertItem(list->GetItemCount(), wxString::FromUTF8(f.display_name));
	if (list == m_friendsList)
	{
		wxString status = _("Offline");
		wxString playing;
		if (f.online)
		{
			status = _("Online");
			if (!f.title_id.empty())
			{
				status = _("Playing");
				playing = wxString::FromUTF8(f.title_id);
			}
		}
		list->SetItem(index, 1, status);
		list->SetItem(index, 2, playing);
	}
	else if (list == m_incomingList || list == m_outgoingList)
	{
		list->SetItem(index, 1, wxString::FromUTF8(f.since));
	}
	return index;
}

void OpenPakFriendsWindow::RebuildLists(const OpenPakSocial::FriendList& friends,
	const OpenPakSocial::RequestList& requests, const OpenPakSocial::InvitationList& invitations)
{
	ClearList(m_friendsList);
	ClearList(m_incomingList);
	ClearList(m_outgoingList);
	ClearList(m_invitationsList);
	m_friends = friends.friends;
	m_incoming = requests.incoming;
	m_outgoing = requests.outgoing;

	if (!friends.ok)
		m_status->SetLabel(wxString::FromUTF8(friends.error));
	else
	{
		for (const auto& f : m_friends)
			InsertFriend(m_friendsList, f);
		m_status->SetLabel(wxString::Format(wxPLURAL("%d friend", "%d friends", (int)m_friends.size()),
			(int)m_friends.size()));
	}
	if (requests.ok)
	{
		for (const auto& f : m_incoming)
			InsertFriend(m_incomingList, f);
		for (const auto& f : m_outgoing)
			InsertFriend(m_outgoingList, f);
	}
	if (invitations.ok)
	{
		for (const auto& inv : invitations.invitations)
		{
			const long index = m_invitationsList->InsertItem(m_invitationsList->GetItemCount(),
				wxString::FromUTF8(inv.from));
			m_invitationsList->SetItem(index, 1, wxString::FromUTF8(inv.title_id));
			m_invitationsList->SetItem(index, 2, wxString::FromUTF8(SinceText(inv.expires_at)));
		}
	}
	UpdateButtons();
}

void OpenPakFriendsWindow::UpdateButtons()
{
	const bool hasSelection = m_friendsList->GetSelectedItemCount() > 0;
	m_removeButton->Enable(hasSelection);
	const bool hasIncoming = m_incomingList->GetSelectedItemCount() > 0;
	m_acceptButton->Enable(hasIncoming);
	m_declineButton->Enable(hasIncoming);
}

void OpenPakFriendsWindow::OnSelectionChanged(wxListEvent& event)
{
	UpdateButtons();
}

void OpenPakFriendsWindow::OnAcceptRequest(wxCommandEvent& event)
{
	const long index = m_incomingList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	if (index < 0 || (size_t)index >= m_incoming.size())
		return;
	const std::string account_id = m_incoming[index].account_id;
	m_acceptButton->Enable(false);
	std::thread([this, account_id]() {
		const std::string error = OpenPakSocial::AcceptFriend(account_id);
		CallAfter([this, error]() {
			if (!error.empty())
				m_status->SetLabel(wxString::FromUTF8(error));
			RefreshAsync();
		});
	}).detach();
}

void OpenPakFriendsWindow::OnDeclineRequest(wxCommandEvent& event)
{
	const long index = m_incomingList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	if (index < 0 || (size_t)index >= m_incoming.size())
		return;
	const std::string account_id = m_incoming[index].account_id;
	m_declineButton->Enable(false);
	std::thread([this, account_id]() {
		const std::string error = OpenPakSocial::DeclineFriend(account_id);
		CallAfter([this, error]() {
			if (!error.empty())
				m_status->SetLabel(wxString::FromUTF8(error));
			RefreshAsync();
		});
	}).detach();
}

void OpenPakFriendsWindow::OnRemoveFriend(wxCommandEvent& event)
{
	const long index = m_friendsList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	if (index < 0 || (size_t)index >= m_friends.size())
		return;
	const std::string account_id = m_friends[index].account_id;
	m_removeButton->Enable(false);
	std::thread([this, account_id]() {
		const std::string error = OpenPakSocial::RemoveFriend(account_id);
		CallAfter([this, error]() {
			if (!error.empty())
				m_status->SetLabel(wxString::FromUTF8(error));
			RefreshAsync();
		});
	}).detach();
}
