# Roadmap

Status 2026-10-05. The PoC scope this grew from is docs/POC.md.

## Done

- **Fingerprint benchmark** (docs/BENCHMARK.md), stage 1: whole-object copies are detected 95-100%
  at 0.1% FPR. Add-base copies 10%, combo 8%. The C++ port is bit-identical to JS.
- **M1 basics** (docs/SPEC.md, ADRs 0009-0013): publish, browse, search, verified download,
  versions, remixes, makes, comments, likes, pinning-hub mode.
  - Reviewed and fixed (docs/reviews/2026-10-05-swamp-core.md).
  - Live two-node test on logos.test passes.
  - Basecamp 0.3.1 GUI test passes, including a GUI download.

## Next

1. **Basecamp, properly.** Every flow is clicked through in the real GUI and polished:
   - publish, new version, remix, makes with photos, retract, profile, restart;
   - error and empty states, progress, sync status;
   - the sandbox-safe pictures (`cacheImage`).
2. **Hub.** Swamp's catch-up and file fetches need a reachable node, so there will be a hub. The
   only way to avoid one would be NAT-ed desktops serving files to each other directly, which needs
   hole punching through a relay anyway. The first hub is ours: bootstrap centralisation, said
   openly. It needs:
   - `SWAMP_HUB=1`;
   - Storage with a public extip, AutoNAT and relay servers, and a long `block-ttl`;
   - clients that bootstrap from it.
3. **Theft labels v1: whole-object copies.**
   - Matches are computed locally on sync: "Possible copy of X by Y", with the evidence.
   - Declared remixes are exempt.
   - Labels inform; they never hide or delete.
4. **Priority anchoring on LEZ.**
   - Epoch Merkle roots of the catalogue, with inclusion proofs on labels.
   - Private pre-anchoring of drafts.
5. **Edited copies.** Stage-2 geometric verification and a triviality gate. This is research; it
   ships when the benchmark numbers justify it.
6. **Labelers and disputes.** Subscribable labelers, RLN-limited flaggers, an "I'm the original"
   dispute with anchored proof.
7. **Tips over LEZ.** Paid models later, if ever.

## Decisions (vpavlin, 2026-10-05)

- Basecamp first; Android later.
- A hub is accepted.
- The repo stays local until 2026-10-06, then it is published.
- No legal review up front.
