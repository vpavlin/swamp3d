# 0008. Basecamp core module + pure-QML view; thumbnails and slicer handoff before a 3D viewer

- Status: Accepted (2026-10-05: target Basecamp 0.3)
- Date: 2026-10-05

## Context

Basecamp bundles no Qt Quick 3D; WebEngine is present but untested in a ui_qml sandbox (notes/04).

## Decision

Same shape as our other apps: a C++ core (catalogue fold, fingerprints, index, purchase flow) and a pure-QML view. Previews are thumbnails generated at publish time; 'open in slicer' hands files to the OS. A WebEngine-based 3D viewer is a separate spike. Android follows, as a browse-and-download client first.

## Rejected

Electron/web app (not Basecamp); waiting for Qt Quick 3D in the host.

## Consequences

Fingerprinting and thumbnails run in the core in C++, with a JS twin for Android (parity vectors, as in MuleNet).
