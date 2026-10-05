# 0013. Every version carries its fingerprint from the first release

- Status: Accepted
- Date: 2026-10-05

## Context

The theft labels need fingerprints of everything published, including models published before labels ship.

## Decision

At publish, the core computes fp/v1 of the first model file (C++ port of packages/fp, byte-parity tested), inlines the global histograms in the version event and uploads the full fingerprint as a blob. No labels are shown in M1.

## Rejected

Compute fingerprints only when labels ship (old models would need re-downloading by someone).

## Consequences

Publishing costs a fraction of a second more; the event grows by ~1 KB.
