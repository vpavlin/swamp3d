# 0003. Shape fingerprint fp/v1 = global D2/A3 histograms + local multi-rank point-pair histogram

- Status: Accepted, revised 2026-10-05 after the benchmark (docs/BENCHMARK.md)
- Date: 2026-10-05

## Context

Thieves re-export, re-mesh, scale, mirror, decimate, add bases and text, cut parts. Global alignment (PCA, voxels, rendered views) breaks under an added base. Learned embeddings aren't deterministic across hardware.

## Decision

fp/v1 = F1 (D2 + A3 shape histograms, Osada 2002) + F3 (a histogram of density-normalised point-pair features at neighbour ranks 2/6/16/40). The originally proposed MinHash over global triple tokens was measured and dropped: unrelated models shared ~70% of tokens. F1+F3 detects whole-object copies at 95-100% for a 0.1% false-positive rate; copies with added geometry need a geometric verification stage (in progress). Integer-friendly arithmetic and seeded integer RNG so JS and C++ agree. Thresholds are set from a scripted attack benchmark before any automatic label ships.

## Rejected

PCA-aligned voxel hashes; multi-view perceptual image hashes; PointNet/CLIP embeddings (kept as possible advisory signals only).

## Consequences

The descriptor is public, so it can be evaded by a determined thief; it raises the cost. Simple shapes need a triviality gate.
