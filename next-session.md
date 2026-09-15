# Next session — cemu

Updated 2026-09-15.

Upstream Cemu (Wii U) plus one OpenPak addition: an **OpenPak** entry in the Network Service
picker, the launch-time network profile, sign-in that mints and installs the Wii U identity
(no console dump needed), and the friends window. Everything is tagged: `openpak-v0.2.0` is
HEAD, nothing unreleased.

## Where things stand

- Network Service picker, profile fetch + refresh (EP-3/EP-5), account box with identity
  install (E3, NA-1a) — OPENPAK.md.
- Tools → OpenPak friends (E3): Friends / Requests / Invitations notebook, thirty-second
  refresh, add by friend code (99e09f2, a1763a2) — in `openpak-v0.2.0`; OPENPAK.md documents
  it since this session.

## Next steps

- Against-a-game verification: online play on the OpenPak service with a synthesised device
  identity, Cemu against Cemu and against a Wii U.
- PRD polish: mods as graphic-pack installs, first-run prompt, keychain edge cases.
- Keep OPENPAK.md in step with each tagged surface.

## Pointers

- [`../prds/`](../prds/README.md) — emulator-wide PRDs (`emulators/prds/` in the workspace):
  emulator-integration-prd.md (E3 done here), emulator-network-profile-prd.md.
- `OPENPAK.md` — this fork's own readme.
