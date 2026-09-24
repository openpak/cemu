# Next session — cemu

Updated 2026-09-23.

Upstream Cemu (Wii U) plus OpenPak: an **OpenPak** entry in the Network Service picker, the
launch-time network profile, sign-in that mints and installs the Wii U identity (no console
dump needed), and the UX-spec surfaces (emulators/prds/openpak-ux-spec.md §8, U1–U7): the
top-level OpenPak menu, the seven-page OpenPak window, the sign-in dialog, connect prompt and
sign-out confirmation, General settings → OpenPak, toasts, and the reused OpenPak Mii account.
Released up to `openpak-v0.3.1`; the UX-spec work is on the `release-prep` branch, waiting for
the coordinated build and release.

## Where things stand

- Network Service picker, profile fetch + refresh (EP-3/EP-5), identity install (E3, NA-1a) —
  OPENPAK.md.
- UX spec U1–U5, U7 in code; U6 toasts for the events Cemu has (sign-in/out/expired, friends,
  requests, invitations). Over a running game toasts go to Cemu's overlay notification.

## Next steps

- Build and release the `release-prep` work (`openpak-v0.4.0`), then run it: menu states,
  sign-in into the reused Mii account, the window's pages against openpak.org.
- S-1 Wii U cloud-save sync and M-1 graphic-pack installs: the Cloud saves and Mods pages are
  read-only until they exist; the *Sync cloud saves* setting is stored but unused.
- *Open Miiverse* on the News page points at `{website}/miiverse`; confirm the real Miiverse
  web address.
- Strings: `_()` with the spec's English; the generated `openpak.po` (L1) is not wired in yet.
- Against-a-game verification: online play on the OpenPak service with a synthesised device
  identity, Cemu against Cemu and against a Wii U.

## Pointers

- [`../prds/`](../prds/README.md) — emulator-wide PRDs (`emulators/prds/` in the workspace):
  emulator-integration-prd.md (E3 done here), emulator-network-profile-prd.md.
- `OPENPAK.md` — this fork's own readme.

## Scratch (research and throwaway work)

Decompiles, Ghidra projects, dumps, exefs/romfs extracts, packet captures,
strace and emulator logs, probe harnesses: put them in
`~/REPOS/Openpak/scratch/<topic>`. That folder is a local mount of the media pool,
outside every repository, so nothing in it is committed. Never use `/tmp` (a
shared 15 GB RAM disk) or elsewhere on `/home` for this. Keys and signing
material never go there. Rule: `docs/playbooks/conventions.md` in the workspace.
