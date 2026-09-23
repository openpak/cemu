# Cemu for OpenPak

Fork of upstream Cemu with two additions: a way to point the emulated console at OpenPak —
including signing in to an OpenPak account, which mints and installs the console identity so
online works with no console dump — and the OpenPak menu, window and settings every OpenPak
emulator shares (`emulators/prds/openpak-ux-spec.md`, Wii U family). Everything else is
upstream, merged as it moves. Builds: `openpak-v*` tags publish a GitHub Release
(`.github/workflows/openpak_release.yml`).

A **Network Service** choice called **OpenPak** next to Nintendo, Pretendo and Custom
(Options → General settings → Account). It points every Wii U service URL (account, eShop
SOAP, NUS, IAS, CCS, IDBE, BOSS, Tagaya, Miiverse discovery) at the `openpak.org` names that
OpenPak's Wii U adapter and content services answer. TLS verification is off for this service
like for Pretendo, because the host's trust store does not hold the OpenPak CA. Saved per
account as service index 4.

The service URLs are not only compiled in: at launch Cemu fetches the OpenPak **network
profile** (`GET <website>/api/v1/network/profile?platform=wiiu`, one conditional GET,
two-second timeout — emulators/prds/emulator-network-profile-prd.md) and fills the ten URLs
from its `services[]` by id. A service the profile does not name keeps its compiled-in URL; a
profile that fails validation is rejected whole and the last-known-good (or compiled-in) URLs
apply, so a game starts either way.

## The OpenPak menu, window and settings (emulators/prds/openpak-ux-spec.md, Wii U family)

- **OpenPak menu**, top level, immediately left of Help, always there. *Sign in to OpenPak...*
  or *Signed in as {name}* (with the account avatar), Friends, Invitations, Cloud saves, Mods,
  News, Status, *OpenPak settings...* (General settings at the OpenPak tab), *OpenPak website*,
  *Sign out...*. States are recomputed each time the menu opens: sign-in and sign-out wait for
  the game to stop; with the active Mii account not on the OpenPak service the header opens the
  settings and Friends, Invitations, Cloud saves and Sign out are disabled. No shortcuts. (Tools
  → OpenPak friends is gone; the window replaces it.)
- **The OpenPak window** (`src/gui/wxgui/OpenPakWindow.cpp`): a modal dialog titled *OpenPak*,
  880×600 (760×480 minimum), a left list of seven pages — Account, Friends, Invitations, Cloud
  saves, Mods, News, Status — each with a title and Refresh, and one footer (busy bar, status
  line, Close). Ctrl+PgUp/PgDn switch pages. Every call runs off the UI thread; a failed refresh
  keeps what was shown and reports in the status line. Pages that need an account show *Sign in
  to OpenPak to use this.* and *Sign in...* when signed out.
  - *Account*: name and avatar, the friend code with Copy, *Sign out...*; Friend code, *Wii U
    identity* (PNID), Linked consoles, and *Console link* — the identity installed in the OpenPak
    Mii account, or *Try again* when it did not install.
  - *Friends*: add by friend code; requests with Accept / Decline / Cancel request; the list
    online first, with presence and *Playing {game}* (local title name, else the catalogue);
    Remove and Block, both confirmed. Refreshes every thirty seconds, with a count of incoming
    requests on the page entry.
  - *Invitations*: read-only (the game that sent an invitation accepts it), every thirty seconds.
  - *Cloud saves*: what the account holds in the cloud for Wii U titles, with usage, versions and
    what this machine has. **Cemu does not sync Wii U saves yet** (S-1): no Upload, Download or
    Resolve, and the page says so.
  - *Mods*: the catalogue's mods for a Wii U title (local titles and the catalogue; the running
    game by default), with Favourite. **Installing graphic packs from OpenPak is not in this
    build** (M-1); the page says so.
  - *News*: Wii U news lives in Miiverse — a not-here panel, without a link (there is no web Miiverse).
  - *Status*: the status page's verdict and services, players online per title and network,
    and this session (account, console link, console edge, presence, a Ping test).
- **Sign-in** (`src/gui/wxgui/OpenPakUI.cpp`): Email, Password and a Device name (default
  "Cemu on {machine}", sent as `device_name` so the account's device list tells machines apart),
  *Create an account*, errors inline, off the UI thread. Success installs the account's Wii U
  identity (below) and shows a toast. **Sign out** is confirmed, revokes the token in the
  background and leaves the Mii account alone. A first interactive launch with no game asks
  *Connect to OpenPak?* once (`connect_asked`); signing in from it turns the OpenPak service and
  cloud sync on. A stored sign-in the website refuses at startup becomes a toast, never a prompt.
- **General settings → OpenPak** (after Account): *Connect this emulator to OpenPak* (mirrors the
  Network Service radio of the shown account; on, it picks the OpenPak Mii account), the account
  row with *Sign in...* / *Sign out...*, *Open OpenPak...*, *Sync cloud saves automatically...*
  (stored for when save sync lands), *Show notifications*, *Notification corner*, and a collapsed
  *Advanced* with the Website (used for the API and the network profile; `OPENPAK_API` still
  wins) and *Refresh network settings*, which now runs off the UI thread.
- **Toasts**: a card in the chosen corner of the main window, six seconds, at most four, click to
  open the page: signed in, signed out, sign-in expired, and — from a poller while signed in —
  friends coming online or starting a game, friend requests and game invitations (the first poll
  is silent). While a game runs, the render canvas covers child windows, so the same text goes
  to Cemu's own overlay notification instead.
- OpenPak settings that are not per account live in `config/openpak_settings.txt`.

## The account and the Wii U identity

Signing in exchanges the openpak.org email and password for a website token, then asks for the
account's Wii U identity (`GET /api/v1/me/wiiu` — PNID, NEX credentials, Mii and the account.dat
password cache, minted by nn-account on first sight) and installs it as a console account
(account.dat, persisid.dat, selection, OpenPak service). **It always writes into the same Mii
account** (UX spec §5.10): the one it used first, remembered as `slot` in
`config/openpak_settings.txt` (or, after an older build, the one already holding the PID); a new
Mii account is created only when there is none. Sign-out keeps that Mii account. No
otp.bin/seeprom.bin dump and no MLC certificate store are needed on this service: when they are
absent, Cemu synthesises a device identity once and reuses it (the adapter checks certificates
structurally only), and the online check re-runs right after sign-in, so no restart is needed. A
console dump that exists is never replaced. The session lives at `config/openpak_session.txt`;
the bearer only ever travels to the website (`OPENPAK_API` or the Website setting,
https-or-loopback).

Friends, requests and invitations come from the website API with the session bearer; in-game
requests arrive through nn-friends into the same core graph, so the window shows what the games
see.

Strings go through `_()` with the UX spec's English (§7.2); core errors come back as codes and
are shown as its `error.*` texts (server sentences only as "OpenPak said: ...", HTTP codes and
transport errors only in the log).

Server side: the OpenPak network (`account`, `nn-account`, `nn-friends`, `nn-nncs`, `nn-boss`,
`nn-juxtaposition`, `nn-soap` for Wii U and 3DS; `nn-wfc` for Wii and DS) answers both the
Nintendo names and the `openpak.org` names behind one TLS front.
