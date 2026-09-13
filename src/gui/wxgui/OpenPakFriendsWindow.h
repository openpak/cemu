#pragma once

#include "Cemu/OpenPak/Social.h"

#include <wx/dialog.h>
#include <wx/listctrl.h>
#include <wx/stattext.h>
#include <wx/timer.h>

#include <atomic>
#include <thread>
#include <vector>

// OpenPak: the friends window (prds/emulator-integration-prd.md §3.5). One
// friend graph for every console: the list the phone and the website show,
// with presence and the running title, the pending requests (in-game requests
// land here through nn-friends), and the invitation inbox.
class OpenPakFriendsWindow : public wxDialog
{
  public:
	OpenPakFriendsWindow(wxWindow* parent);
	~OpenPakFriendsWindow();

  private:
	void OnRefresh(wxCommandEvent& event);
	void OnRefreshTimer(wxTimerEvent& event);
	void OnAcceptRequest(wxCommandEvent& event);
	void OnDeclineRequest(wxCommandEvent& event);
	void OnRemoveFriend(wxCommandEvent& event);
	void OnSelectionChanged(wxListEvent& event);
	void OnClose(wxCloseEvent& event);

	void RefreshAsync();
	void RebuildLists(const OpenPakSocial::FriendList& friends, const OpenPakSocial::RequestList& requests,
					  const OpenPakSocial::InvitationList& invitations);
	long InsertFriend(wxListCtrl* list, const OpenPakSocial::Friend& f);
	void UpdateButtons();

	wxListCtrl* m_friendsList = nullptr;
	wxListCtrl* m_incomingList = nullptr;
	wxListCtrl* m_outgoingList = nullptr;
	wxListCtrl* m_invitationsList = nullptr;
	wxStaticText* m_status = nullptr;
	wxButton* m_refreshButton = nullptr;
	wxButton* m_acceptButton = nullptr;
	wxButton* m_declineButton = nullptr;
	wxButton* m_removeButton = nullptr;
	wxTimer m_refreshTimer;

	// row order mirrors these; the lists show display names, actions key on account ids
	std::vector<OpenPakSocial::Friend> m_friends;
	std::vector<OpenPakSocial::Friend> m_incoming;
	std::vector<OpenPakSocial::Friend> m_outgoing;

	std::atomic<bool> m_refreshing{false};
	std::thread m_worker;
};
