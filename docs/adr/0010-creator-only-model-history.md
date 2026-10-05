# 0010. Only a model's creator can extend it; versions are immutable

- Status: Accepted
- Date: 2026-10-05

## Context

Versions are the unit people download, remix and (later) anchor. A history anyone can rewrite is worthless as evidence of who published what first.

## Decision

model.create fixes the creator (first by HLC wins for a modelId; modelId is derived from the creator's address and a nonce, so it can't be squatted). Only the creator's later events count for versions, CIDs and retraction. Version v must be exactly latest+1 and is first-writer-wins, never edited. Retract is a tombstone that hides a model by default but keeps its versions.

## Rejected

Last-writer-wins edits of a model record (rewritable history); free-form forks under the same id (that's a remix: a new model with a parent).

## Consequences

Fixing a typo means a new version. Clients can still show any version.
