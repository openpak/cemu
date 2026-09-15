# Cemu for OpenPak

Fork of upstream Cemu with one addition: a way to point the emulated console at OpenPak —
including signing in to an OpenPak account, which mints and installs the console identity so
online works with no console dump. Everything else is upstream, merged as it moves. Builds:
`openpak-v*` tags publish a GitHub Release (`.github/workflows/openpak_release.yml`).

A **Network Service** choice called **OpenPak** next to Nintendo, Pretendo and Custom
(Options → General settings → Account). It points every Wii U service URL (account, eShop
SOAP, NUS, IAS, CCS, IDBE, BOSS, Tagaya, Miiverse discovery) at the `openpak.org` names that
OpenPak's Wii U adapter and content services answer. TLS verification is off for this service
like for Pretendo, because the host's trust store does not hold the OpenPak CA. Saved per
account as service index 4.

Since 2026-09-12 the service URLs are not only compiled in: at launch Cemu fetches the OpenPak
**network profile** (`GET openpak.org/api/v1/network/profile?platform=wiiu`, one conditional
GET, two-second timeout — emulators/prds/emulator-network-profile-prd.md) and fills the ten URLs from its
`services[]` by id. A service the profile does not name keeps its compiled-in URL; a profile
that fails validation is rejected whole and the last-known-good (or compiled-in) URLs apply, so
a game starts either way. Settings → Account has a **Refresh network settings** button and a
line showing which profile is in effect (fetched/cached/built-in, and its version).

Since 2026-09-13 the Account tab has an **OpenPak account** box (emulators/prds/emulator-integration-prd.md
E3): sign in with the openpak.org email and password, and Cemu asks the website for the
account's Wii U identity (`GET /api/v1/me/wiiu` — PNID, NEX credentials, Mii and the
account.dat password cache, minted by nn-account on first sight) and installs it as a console
account (account.dat, persisid.dat, selection, OpenPak service). No otp.bin/seeprom.bin dump
and no MLC certificate store are needed on this service: when they are absent, Cemu synthesises
a device identity once and reuses it (the adapter checks certificates structurally only). A
console dump that exists is never replaced. The session lives at `config/openpak_session.txt`;
the bearer only ever travels to openpak.org (`OPENPAK_API` override, https-or-loopback).

Since 2026-09-13 Tools → **OpenPak friends** (E3) opens a notebook of Friends / Requests /
Invitations: the shared graph with presence and playing title, incoming and outgoing requests,
the invitation inbox, accept / decline / remove, and add by friend code — the piece that
completes the cross-platform loop. It speaks the website API with the session bearer, refreshes
on demand and every thirty seconds, and in-game requests arrive through nn-friends into the
same core graph, so the window shows what the games see.

Server side: the OpenPak network (`account`, `nn-account`, `nn-friends`, `nn-nncs`, `nn-boss`,
`nn-juxtaposition`, `nn-soap` for Wii U and 3DS; `nn-wfc` for Wii and DS) answers both the
Nintendo names and the `openpak.org` names behind one TLS front.
