# Cemu for OpenPak

Fork of upstream Cemu with one addition: a way to point the emulated console at OpenPak.
Everything else is upstream, merged as it moves. Builds: `openpak-v*` tags publish a GitHub
Release (`.github/workflows/openpak_release.yml`). Not yet run against a game.

A **Network Service** choice called **OpenPak** next to Nintendo, Pretendo and Custom
(Options → General settings → Account). It points every Wii U service URL (account, eShop
SOAP, NUS, IAS, CCS, IDBE, BOSS, Tagaya, Miiverse discovery) at the `openpak.org` names that
OpenPak's Wii U adapter and content services answer. TLS verification is off for this service
like for Pretendo, because the host's trust store does not hold the OpenPak CA. Saved per
account as service index 4.

Since 2026-09-12 the service URLs are not only compiled in: at launch Cemu fetches the OpenPak
**network profile** (`GET openpak.org/api/v1/network/profile?platform=wiiu`, one conditional
GET, two-second timeout — prds/emulator-network-profile-prd.md) and fills the ten URLs from its
`services[]` by id. A service the profile does not name keeps its compiled-in URL; a profile
that fails validation is rejected whole and the last-known-good (or compiled-in) URLs apply, so
a game starts either way. Settings → Account has a **Refresh network settings** button and a
line showing which profile is in effect (fetched/cached/built-in, and its version).

Server side: the OpenPak network (`account`, `nn-account`, `nn-friends`, `nn-nncs`, `nn-boss`,
`nn-juxtaposition`, `nn-soap` for Wii U and 3DS; `nn-wfc` for Wii and DS) answers both the
Nintendo names and the `openpak.org` names behind one TLS front.
