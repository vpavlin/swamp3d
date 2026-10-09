# Swamp

A decentralized place to share 3D models.

> Status: **working prototype** (0.5.x). It runs in Logos Basecamp 0.3 and has been tested headless
> and in the GUI on the Logos test network. It is not audited, the test network resets, and LAN
> printing has only been tested against a fake printer. Not endorsed by or affiliated with Logos;
> built on the Logos tech stack.

A Printables / MakerWorld / Thingiverse-style library for 3D models and maker projects that no
company runs:
- a Basecamp desktop app;
- files in Logos Storage;
- the catalogue, comments and makes as signed events over Logos Messaging;
- LEZ, later, for tips and for anchoring when each version was published.

Core properties: **privacy, pseudonymity, censorship resistance**. Hard requirement from day one:
**stolen copies must be flaggable** with evidence anyone can check, without a central moderator
and without deleting anything.

## What works

- **Publishing:** files are hashed, then get a thumbnail and a shape fingerprint, and upload in the
  background.
- **Browsing** the categories you follow (an offline copy per category).
- **Global search** over signed, deterministic index snapshots in Storage.
- **Model pages:** versions, remixes, makes with photos, comments, likes, retract.
- **Verified downloads**, and "Open in slicer" (OrcaSlicer, Bambu Studio, PrusaSlicer).
- **One-click slicer setup:** if no usable OrcaSlicer is installed, Swamp downloads a pinned,
  hash-checked OrcaSlicer 2.4.2 and uses it.
- **Experimental, switched off: LAN printing** to a Bambu Lab A1 / A1 mini (`SWAMP_EXPERIMENTAL_PRINT=1`).
  In its first real test (2026-10-08) the printer drove its head into the top of its frame; the cause
  isn't known yet. Don't enable it unless you can watch the printer and stop it.
- **Indexer audits:** declared exclusions, plus the creator's own inclusion checks.

## Layout

- `swamp_core/`: the core module (C++, universal Basecamp 0.3 module).
  - `src/swamp_core_impl.*`: the module;
  - `src/swamp_catalog.hpp`: catalogue rules;
  - `src/swamp_index.hpp`: the search index;
  - `src/swamp_bambu.hpp`: the printer client;
  - `src/swamp_fp.hpp`, `src/swamp_thumb.hpp`: fingerprints and thumbnails.
- `module/`: the QML view.
- `packages/fp/`: the JS reference fingerprint; `bench/`: the fingerprint benchmark (its data set is
  git-ignored).
- `hub/`: two-node scripts that run against the live Logos test network.
- `site/`: the website.
- `docs/`: SPEC, ADRs 0001-0017, roadmap, reviews.

## Building and testing

- **Packages:** `nix build .#lgx-portable` in `swamp_core/` and in `module/` (logos-module-builder 0.3.1).
  Linux arm64: push a tag `arm64-*`; `.github/workflows/arm64.yml` builds both on a GitHub ARM runner
  (and checks for ARMv8.0-unsafe LSE atomics) and attaches them to a release. Merge them into the
  x86-64 packages with `lgx merge` (published packages carry both platforms).
- **Tests:** `swamp_core/test/run-tests.sh` runs catalogue, index, fingerprint parity (C and cs_CZ
  locale), printer client (against a fake Bambu printer) and end-to-end checks.
  - Needs g++, OpenSSL, Qt6Core, nlohmann-json (`json-devel` on Fedora), python3 and node.
  - Test meshes are committed under `swamp_core/test/meshes/` (`gen-test-meshes.py`).
- **Live tests:** `hub/two-node.sh` and `hub/search-live.sh`, with `LOGOSCTL` and `LGX_DIR` set
  (see the script headers).

## Running a hub

A hub is the same core, headless, with every category followed: it keeps a copy of every model's
files, answers catch-up for people who were offline, and builds the search index. Without one, a
model is only visible while someone who holds it is online. Anyone can run one on a server with a
public IP:

    sudo PUBLIC_IP=<ip> LOGOSCTL_APPIMAGE=logosctl-x86_64.AppImage LGX_DIR=<dir with the .lgx> hub/vps-hub.sh

It runs as `swamp-hub.service` (logosctl 0.3.1, `SWAMP_HUB=1`), with Storage reachable on TCP 8399
as an AutoNAT + relay server.

## Background reading

Start with `notes/01`–`05a`, `RESEARCH.md` and `docs/THEFT.md`. The decisions live in `docs/adr/`,
the protocol in `docs/SPEC.md`, and the plan in `docs/ROADMAP.md`.

## Licence

Dual-licensed under either of, at your option:
- the Apache License, Version 2.0 (`LICENSE-APACHE-v2`);
- the MIT licence (`LICENSE-MIT`).

This is the same as the Logos projects.
