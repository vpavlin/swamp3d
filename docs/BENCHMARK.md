# Fingerprint benchmark: results so far (2026-10-05)

Data: 220 clean single-part models from Thingi10K (real Thingiverse uploads, CC-licensed), 60 of
them put through 12 scripted attacks (720 copies). Negatives: all 24,090 pairs of different
originals. Code: `packages/fp` (fingerprint + verification), `bench/` (attacks, runners, reports).
Raw outputs: `bench/results/*.md`.

## Stage 1: the fingerprint (fp/v1 = F1 global histograms + F3 local multi-rank features)

Detection at a **0.1% false-positive rate** over the 24,090 unrelated pairs, score = F1 + F3:

| attack | detected | notes |
|---|---|---|
| re-export (shuffled faces, vertex order) | 100% | |
| rotate + translate | 100% | |
| uniform scale | 100% | |
| mirror | 100% | |
| subdivide (re-mesh) | 100% | |
| add text-like block | 97% | |
| decimate 50% | 95% | |
| non-uniform stretch (z ×1.25) | 75% | |
| **add base plate** | **10%** | the thieves' favourite edit |
| **combo** (decimate + base + scale + rotate) | **8%** | |
| surface noise (0.2% of size) | 5% | not a realistic thief edit; destroys normals |

Fingerprinting costs ~70-300 ms per model in Node, and the fingerprint is a few KB.

**As a shortlist** (rank the true original among all 220 by score): recall@20 is 100% for every
whole-object edit, but **75% for add-base and 73% for combo**. So a two-stage design caps at ~75%
for those edits until the shortlist improves.

Dead ends, measured and dropped: a global triple-token MinHash (unrelated models share ~70% of
tokens) and IDF-weighted feature-presence containment (big models "contain" everything).

## Stage 2: geometric verification (WIP, not working yet)

Align the suspected original onto the suspect (FPFH descriptors, mutual matches, RANSAC over
similarity transforms with mirror allowed, area-bounded scale), then measure how much of the
original lies on the suspect.

- **Loose coverage** (tolerance ~3% of model size): useless. Hard negatives (each original's 5 most
  similar unrelated models) have median 0.72 and many reach 1.0.
- **Tight coverage** (exact point-to-triangle distance, ICP onto the real mesh, tolerance 0.6% of
  size, normals must agree): separates much better on spot checks (unrelated mostly 0.00-0.26),
  but the alignment isn't reliable yet: even an identical re-export of a symmetric model can
  score 0.29.

## What this means for the theft design

- **Whole-object copies** (re-uploads, re-exports, rescaled, mirrored, re-meshed, decimated,
  lightly edited) are detectable now, cheaply, at a very low false-positive rate. That is most of
  the "free to sell" re-uploads described in notes/01.
- **Copies with added geometry** (a base, a combined kit) are the open problem. They need a
  working stage-2 verifier and a better shortlist.
- **Simple shapes** (flat discs, plates, boxes) genuinely match each other; a triviality gate is
  still needed.

## Next steps

1. Stage 2: seed ICP with PCA alignment hypotheses (24 axis/sign candidates, mirror included) as
   well as RANSAC, ICP each, keep the best tight score; then re-run the full benchmark.
2. Shortlist for partial copies: add per-region fingerprints (e.g. F1/F3 over the largest
   connected surface patches, or over points far from the dominant planes) so a base doesn't
   swamp the signal.
3. A triviality score (how much of the surface is flat or a primitive) to suppress auto-labels.
4. C++ port with parity vectors once v1 stabilises.
