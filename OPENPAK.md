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

Server side: the OpenPak network (`account`, `nn-account`, `nn-friends`, `nn-nncs`, `nn-boss`,
`nn-juxtaposition`, `nn-soap` for Wii U and 3DS; `nn-wfc` for Wii and DS) answers both the
Nintendo names and the `openpak.org` names behind one TLS front.
