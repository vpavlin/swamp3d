# 0001. Stolen and harmful models are labelled, never deleted

- Status: Proposed
- Date: 2026-10-05

## Context

Censorship resistance and pseudonymity are core properties, and there is no central host to delete from. Theft (re-uploaded, resold models) is still the creators' biggest problem on every existing platform (notes/01).

## Decision

Nothing is deleted at the protocol level. Clients show labels next to a model: automatic ones computed from evidence (possible copy, licence conflict) and reviewed ones from labelers the user subscribes to. Hubs choose what they cache; users choose their blocklists.

## Rejected

A global moderator or allowlist (central point of control and of legal pressure); token-weighted voting (plutocratic, sybil-prone).

## Consequences

Moderation becomes a market of labelers and lists. The app must show clearly whose label is whose. Clearly illegal content still needs default blocklists and hub policies, which we must design with legal advice.
