# 0003. Shape fingerprint fp/v1 = global D2/A3 histogram + MinHash over local point-pair features

- Status: Proposed
- Date: 2026-10-05

## Context

Thieves re-export, re-mesh, scale, mirror, decimate, add bases and text, cut parts. Global alignment (PCA, voxels, rendered views) breaks under an added base. Learned embeddings aren't deterministic across hardware.

## Decision

fp/v1 combines a global shape-distribution histogram (Osada 2002) for overall similarity with a MinHash over quantised, scale-free point-pair features (Drost 2010) for containment (partial copies), with banded LSH keys for search. Integer-friendly arithmetic and seeded integer RNG so JS and C++ agree. Thresholds are set from a scripted attack benchmark before any automatic label ships.

## Rejected

PCA-aligned voxel hashes; multi-view perceptual image hashes; PointNet/CLIP embeddings (kept as possible advisory signals only).

## Consequences

The descriptor is public, so it can be evaded by a determined thief; it raises the cost. Simple shapes need a triviality gate.
