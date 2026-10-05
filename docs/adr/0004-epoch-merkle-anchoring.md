# 0004. Publications are anchored as one Merkle root per epoch

- Status: Accepted (2026-10-05: anchor target = LEZ registry program)
- Date: 2026-10-05

## Context

Priority ('published before T') is the strongest objective theft signal. Per-publication on-chain transactions cost money, need a wallet and leak activity patterns.

## Decision

Every version event is signed off-chain (loam-sync). A hub collects version hashes (or private commitments) per epoch, anchors one root, and hands each creator an inclusion proof. The anchor target sits behind an interface: a LEZ registry program or a Bedrock channel inscription (open question). Creators who care can anchor individually.

## Rejected

Per-version registry transactions only (cost, wallet needed); no anchoring, just signatures (no time evidence).

## Consequences

Hubs can delay or censor an anchor but can't backdate. Anchors are advisory while testnet resets.
