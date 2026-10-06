# 0017. Print to Bambu Lab printers over the LAN; slice locally with the printer's default profile

- Status: Accepted. Built in 0.5.0. Tested against a fake printer (bambu_mock.py) and with the real
  OrcaSlicer 2.4.2. Not yet tested on a real printer.
- Date: 2026-10-06

## Context

Fun, and part of "download → print" without a company in the middle. vpavlin has a Bambu Lab A1,
currently set up for cloud printing.

Since the 2025-01 firmware, third-party control in Bambu's normal LAN mode needs Bambu's
authorization (Bambu Connect). LAN-only + **Developer Mode** keeps local MQTT and FTPS open
without it. The cloud API needs an account and is unofficial.

## Decision

- **Two paths.**
  - **Open in slicer** (0.4.1): hand the verified files to the user's slicer; their cloud setup is
    untouched.
  - **Print on <printer>** (0.5.0): direct LAN printing, for printers in LAN-only + Developer Mode.
- **Slice locally and geometry only.**
  - The OrcaSlicer (or Bambu Studio) CLI slices with the printer's default profile: A1 = "Bambu Lab
    A1 0.4 nozzle" + "0.20mm Standard @BBL A1" + "Bambu PLA Basic @BBL A1"; A1 mini likewise.
  - STL/OBJ/STEP/AMF go in as they are. A 3MF is first reduced to an STL (`--export-stl`), so
    settings and custom G-code embedded by whoever published it never reach the printer.
  - **Swamp never sends G-code from the network.**
- **The printer protocol** (swamp_bambu.hpp, OpenSSL):
  - SSDP discovery on UDP 2021;
  - FTPS with implicit TLS on 990 (`bblp` / access code; the data channel reuses the control TLS
    session) to the SD root;
  - MQTT 3.1.1 over TLS on 8883: `device/<serial>/request` with `project_file`
    (`param: Metadata/plate_1.gcode`, `url: file:///sdcard/<file>`), status from `device/<serial>/report`.
  - The printer's certificate isn't verified: it's signed by Bambu's CA, which we don't ship; the
    access code authenticates.
- **A person confirms.** `preparePrint` stops at `ready` with the slicer's estimate. Only
  `startPrint("yes")`, behind a confirm dialog, sends anything.
- **Secrets:** `printer.json` is owner-only, and the access code is never returned by the API.

## Rejected

- **Linking libslic3r:** a huge dependency tree, and AGPL would bind Swamp; the CLI keeps them separate.
- **Bambu cloud API, or extracted Bambu Connect keys:** an account, against the terms, or both.
- **Printing a downloaded `.gcode` or `.gcode.3mf` as is:** a stranger's G-code would run on your printer.

## Consequences

- LAN-only + Developer Mode turns off Bambu's cloud printing while it's on. It's reversible on the
  printer.
- Only the A1 and A1 mini are mapped so far. Others need their profile names (P1/X1 use
  `file:///mnt/sdcard` or `ftp:///` URLs: to verify).
- Needs OrcaSlicer or Bambu Studio installed. The Bambu Studio CLI is assumed to accept the same
  options as Orca's (Orca descends from it); unverified.
- Bambu can change the rules with a firmware update again.
