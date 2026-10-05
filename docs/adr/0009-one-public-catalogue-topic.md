# 0009. The catalogue is one public, signed event log on one topic

- Status: Accepted
- Date: 2026-10-05

## Context

Models are public by intent. Our other apps seal every event with a household key (charter rule 6), but a public library has no household.

## Decision

All catalogue events (profiles, models, versions, CIDs, comments, makes, likes) go on /swamp/1/catalog/proto as loam-sync events signed by their author, unsealed. Integrity comes from signatures and the fold's rules (only the creator extends a model). Sharded topics (per tag) and snapshots in Storage come when the log gets big (M2).

## Rejected

Sealing with a key published in the app (no confidentiality, false sense of security); per-model topics (a peer would have to join thousands).

## Consequences

A recorded exception to charter rule 6. Anyone can read the whole catalogue - that is the product. Private sharing (unlisted models) would need its own sealed topics later.
