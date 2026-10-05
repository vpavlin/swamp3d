# 0002. Copy flags carry evidence that every client re-checks

- Status: Proposed
- Date: 2026-10-05

## Context

Opinions can be faked by sockpuppets; evidence can be checked. notes/05a.

## Decision

A CopyFlag is an evidence object: both fingerprints, both anchor proofs, similarity scores, the declared-lineage and licence checks. Clients recompute similarity from the fingerprints (and from the files, when public), verify the anchors, and drop flags they can't reproduce. Only intent ('is this theft or independent creation?') is left to labelers.

## Rejected

Report counts as a signal (brigading); centrally computed similarity (Thangs/Physna-style, closed).

## Consequences

Flags are bigger than a report (a blob in Storage, a small event on Messaging). Fingerprints must be deterministic across JS and C++ (ADR 0003).
