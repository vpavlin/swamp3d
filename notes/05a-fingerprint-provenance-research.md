# 05a — Shape fingerprints, provenance, and decentralized copy flags

Research date 2026-10-05. The problem: someone re-uploads another creator's model, often lightly
modified, and sells it. We can't delete anything and we won't have a central moderator. What we want
is to **flag likely copies with evidence any client can check**, so users can find the original author.

Claims carry a source where I have one. **[?]** marks things I did not verify or that rest only on
forum or community sources. Paper citations come from the literature and are well known, but I did
not re-read every PDF today.

---

## TL;DR

- **Fingerprint.** Use two deterministic, cheap descriptors that we implement the same way in JS and
  C++. (a) A **D2/A3-style global shape histogram** (Osada et al.). It's naturally invariant to
  rotation, translation and mirroring, and becomes scale-invariant once normalised. (b) A
  **MinHash sketch over quantized, scale-free local point-pair features**. This catches *partial*
  copies: an added base, cut parts, text. Index the MinHash with **banded LSH**. A listing publishes
  its band keys, any client or indexer can find candidates, and **every client re-verifies locally**.
  Keep deep-learning embeddings as *advisory* labeler tooling. They're non-deterministic and too heavy
  to count as evidence.
- **Provenance.** Do OpenTimestamps-style Merkle batching of
  `H(salt ‖ fileHash ‖ fingerprint ‖ authorPubKey)` into LEZ. Commit-reveal proves "I had this at T"
  without publishing it. Priority is **evidence, not proof of authorship**: a thief who steals from
  Printables can anchor before the real author ever joins us. Counter-evidence comes from
  **anchored draft chains** (WIP versions, CAD sources) and **imported external history** (Wayback
  Machine snapshots, zkTLS or Keybase-style profile proofs).
- **Flagging.** Use the AT Protocol labeler model: anyone can run a labeler, labels are signed, and
  clients choose whom to trust. Make labels **evidence-carrying**, so a flag whose similarity the
  client can't reproduce gets dropped automatically. Rate-limit flaggers with **RLN** (we already
  ship RLN) plus optional stake. Bridging-style aggregation (Community Notes) resists brigading.
- **Open problems.** Paid/encrypted models (we must trust buyer attestations or run an expensive zk
  proof), the front-running theft of pre-existing work, adaptive evasion of a public descriptor,
  and false positives on trivial or famous shapes (cubes, Benchy).

---

## 1. Robust 3D shape fingerprints

### 1.1 What the thief does, and what each step breaks

Thieves typically re-export (the vertex order and triangle soup change), convert formats
(STL↔3MF↔OBJ), scale uniformly, rotate or translate, mirror, decimate or subdivide, and make small
edits: add a base or text, cut a part off, hollow it out. Bambu forum users report "add a base and
call it a remix" as a common move, and say image-based detection is "almost impossible"
([Bambu forum: new copyright evasion techniques](https://forum.bambulab.com/t/new-copyright-evasion-techniques-popping-up/110432/3)).
A byte hash (SHA-256 of the file) survives none of this. Even a "canonical mesh hash" (sorted
vertices) fails after one re-mesh.

**Key observation:** *additive* edits (a base, text) move the centroid, the bounding box and the
PCA axes. Any descriptor that first normalises by *global* statistics is therefore fragile against
exactly the edit thieves use most. That pushes us toward (a) features that need no global frame and
(b) set-containment matching instead of whole-object distance.

### 1.2 Survey

| Approach | Invariance | Partial-copy robustness | Cost | Determinism JS↔C++ | Notes |
|---|---|---|---|---|---|
| **Canonical normalisation** (centroid, PCA alignment, unit RMS radius) | R/T/S once normalised. PCA has sign and axis-order ambiguity; mirroring flips handedness | Poor: a base shifts the PCA axes | trivial | good (avoid eigen-solver differences: use closed-form 3×3 or Jacobi with fixed iterations) | A *pre-step* for others, not a fingerprint on its own |
| **D2 shape distribution** — histogram of distances between random surface point pairs ([Osada, Funkhouser, Chazelle, Dobkin, ACM TOG 2002](https://doi.org/10.1145/571647.571648)) | R/T/mirror free. Scale by normalising by mean or median distance | Moderate: small edits shift the histogram a little, big bases a lot | O(N) samples + O(P) pairs, ms | **excellent**: only +,−,×,√ | Weak at telling simple shapes apart (all boxes look alike) |
| **A3 / D1 / D3** (angle, centroid distance, triangle area) — same paper | A3 is scale-free *by construction* | Moderate | cheap | A3 needs angles → use cosines, not `acos` | Combine with D2 for discrimination |
| **Spherical harmonic descriptor** ([Kazhdan, Funkhouser, Rusinkiewicz, SGP 2003](https://dl.acm.org/doi/10.5555/882370.882392)) | Rotation-invariant energies per frequency band | Moderate | voxelise + SH transform, tens of ms | Fragile: needs trig, FFT-like sums, a voxeliser; harder to keep bit-identical | Strong in the [Princeton Shape Benchmark](https://doi.org/10.1109/SMI.2004.1314504) era |
| **Light Field Descriptor** — silhouettes from 10 dodecahedron views × rotations, Zernike + Fourier ([Chen, Tian, Shen, Ouhyoung, Eurographics 2003](https://doi.org/10.1111/1467-8659.00669)) | Rotation by exhaustive view matching | Moderate–good | render ~100 views, heavy | Rasteriser + image transforms are hard to keep identical | Was the top performer in PSB |
| **Multi-view silhouettes + perceptual hash** (pHash/dHash of depth or silhouette renders in a PCA frame) | Depends on the frame | Moderate | a small software rasteriser at 64×64 is fine | dHash (pixel compares) is deterministic. pHash's DCT is mostly fine | Natural fit with listing *photos*. Thieves often reuse the original's renders ([Bambu forum](https://forum.bambulab.com/t/stealing-model-and-photoshopped-photo-from-original/50078)) |
| **Voxel-grid hash** (occupancy at 16³–32³ in the canonical frame, then a hash or SimHash) | Only as good as the frame | Poor with a global frame | cheap | good if the frame is deterministic | Simple, but inherits PCA fragility |
| **MinHash / LSH over local features** (quantized local descriptors as "shape words"; Jaccard and *containment*) | Comes from the local feature | **Good**: containment `|A∩B|/|A|` survives a base being added | moderate | good with integer hashing | Like shingling for text near-duplicates ([Broder 1997](https://doi.org/10.1109/SEQUEN.1997.666900)) |
| **Point-pair features** ([Drost et al., CVPR 2010](https://doi.org/10.1109/CVPR.2010.5540108)) as the local feature: `(‖d‖, ∠(n1,d), ∠(n2,d), ∠(n1,n2))` | Angles are R/T/S/mirror-invariant (unsigned). ‖d‖ needs a scale ref → use *ratios* | Good | moderate | good (cosines) | Our recommended token source |
| **Deep embeddings**: [PointNet (Qi et al. CVPR 2017)](https://arxiv.org/abs/1612.00593), DGCNN, [MVCNN (Su et al. ICCV 2015)](https://arxiv.org/abs/1505.00880), CLIP-aligned OpenShape/ULIP | Learned, approximate | Often the best semantic recall | GPU or heavy CPU, 10–500 MB weights | **Poor**: float and kernel order differ across CPUs and GPUs, weights must be pinned, quantization changes results | Also vulnerable to adversarial point perturbations, and it finds "same category" (two different dragons), which is *wrong* for theft |

Copy detection is a narrower problem than shape retrieval. Retrieval asks "find chairs"; we ask
"find *this* chair". That favours geometry-exact local statistics over semantic embeddings.
Relevant copy-detection and watermarking work:
[robust point-cloud copy detection via registration + distances (arXiv 2110.00972)](https://arxiv.org/pdf/2110.00972),
and [robust mesh watermarking (Praun, Hoppe, Finkelstein, SIGGRAPH 1999)](https://doi.org/10.1145/311535.311540)
along with spectral schemes. Watermarks are complementary: the creator embeds one before
publishing. Re-meshing attacks often remove them, and they prove nothing about *who first*.

### 1.3 Determinism across JS and C++

We don't need bit-identical fingerprints for *verification*. A client recomputes the similarity
and checks it against a threshold with a margin. We do want near-identical results so that LSH band
keys mostly agree. Rules:

1. **Only use IEEE-754 ops that are correctly rounded:** `+ − × ÷ √`. Avoid `acos`, `atan2`, `exp`
   and `log`. ECMAScript doesn't pin their precision, and libm differs. Compare *cosines* against
   fixed bin edges instead of computing angles.
2. **C++:** `double`, `-ffp-contract=off` (no FMA fusing), no `-ffast-math`, SSE2 rather than x87.
   **JS:** Numbers are already double.
3. **PRNG:** integer-only (e.g. PCG32 or xoshiro128** using `Math.imul` and `>>> 0` in JS), seeded
   by a constant plus the version. **Hash** tokens with xxHash32 or Murmur3 in 32-bit integer ops.
4. **Order independence:** after a re-export the triangle order changes, so sample sets will differ
   anyway. Robustness has to come from statistics (large N, coarse bins), not from identical
   samples. Dedupe vertices and drop degenerate triangles before sampling.
5. **Parsing:** STL float32 → double is exact. 3MF/OBJ decimal → double through `strtod` and
   `parseFloat` is correctly rounded in practice. 3MF can hold several objects and build transforms:
   apply the transforms and fingerprint each object *and* the union.
6. **Version every descriptor** (`fp/v1`). Evidence objects name the version, and clients keep old
   implementations.

### 1.4 Recommendation: two descriptors + banded LSH

**F1 — global "D2A3" histogram (~256 B).** Sample N = 16 384 surface points, area-weighted, with
the integer PRNG. Use P = 262 144 random pairs.
- D2: distance divided by the **median** pair distance (more robust than the mean to a thin added
  base), 64 bins over [0, 3].
- A3: cosine of the angle at the middle point of random triples, 32 bins over [−1, 1].
- Also store `surfaceArea/scale²`, `volume/scale³` (signed-tetrahedron volume works even with a
  slightly open mesh) and the number of connected components.

Compare with L1 or Earth-Mover over the 1-D CDFs. This catches whole-model copies through any
rigid transform, scale, mirror, re-mesh or decimation, at a cost of milliseconds.

**F2 — "shape-word" MinHash (128 × 32-bit = 512 B).** For each sampled point p_i, take its k = 8
nearest sampled neighbours (a deterministic grid-hash kNN). For each pair (p_i, p_j) use:
- three cosines quantized to 6 levels: `n_i·d̂`, `n_j·d̂`, `|n_i·n_j|`;
- a **scale-free distance ratio** ‖d_ij‖ / ‖d_i,k‖ (k-th neighbour distance), quantized to 4
  levels;
- plus a coarse local curvature bucket (normal variation over the k neighbours).

Hash each token, then reduce the *multiset* to a 128-permutation MinHash (or b-bit MinHash). Using
unsigned cosines and ratios makes F2 invariant to rotation, translation, scale and mirroring, and it
needs no global frame. Compare with the Jaccard estimate and **asymmetric containment**: a suspect
that *contains* most of the original's shape-words but adds new ones (a base) scores high
containment. Caveat: kNN scale depends on sampling density, which depends on total area. A big
added base dilutes density on the original part. Mitigate with a second sampling pass at fixed
density (points per unit area relative to the median pair distance) **[?]**, and benchmark it.

**LSH.** Split F2 into b = 32 bands of r = 4 rows. Each band hashes to a 32-bit **band key**. At
r = 4, b = 32 the S-curve threshold is about (1/b)^(1/r) ≈ 0.42 Jaccard: high recall, and local
verification removes the noise. Also bucket F1 coarsely (e.g. a SimHash of the histogram, 3 bands
of 16 bits) so whole-object matches with low token overlap are still found.

**Search without a central index.** Every listing (a signed event in the catalog log) carries
`fp/v1 = {F1, F2, bandKeys[]}`, which is about 1 KB.
- **A full client is its own index.** 1 M listings × 35 band keys × 8 B is about 280 MB, which fits
  on a desktop. Mobile clients keep just the band→listing map, or query an indexer.
- **Indexers are untrusted.** Anyone can run one (on the Basecamp headless hub). A client sends band
  keys and gets back candidate listing IDs, then fetches their fingerprints and/or files and
  **verifies locally**. A lying indexer can only *hide* candidates. Asking two independent indexers
  limits that.
- Waku/Logos Messaging content topics sharded by band-key prefix are possible for live "new listing
  near mine?" alerts. Logos Storage's DHT stores CIDs, not arbitrary keys, so it isn't the index.

**False positives on simple shapes.** Cubes, calibration cubes, boxes, plain vases and threads
collide a lot. Weight band keys and tokens by **inverse listing frequency** (IDF), computed by each
client from its own catalog. Compute a **"triviality" score**: low F1 entropy, few components,
F2 dominated by planar tokens. Clients must **never auto-flag** a match between two trivial shapes.
Famous bases (Benchy, Voron parts) produce thousands of legitimate remixes, so the declared-remix
graph matters more than geometry there.

**Evasion.** The descriptor is public, so an adaptive thief can perturb until the score drops:
non-uniform scaling (110 % in Z changes D2), heavy displacement noise, mesh boolean unions with
decoys, splitting into parts. Our goal is to make evasion cost real effort and visible damage, not
to make it impossible. Labelers can run stronger, private or ML detectors as an advisory second
tier. Those labels must still cite reproducible evidence (§4), even when the detector that found the
pair isn't reproducible.

### 1.5 What exists today

- **Thangs (Physna)** runs geometric and "part-in-part" search at internet scale with proprietary
  tech ([TCT](https://www.tctmagazine.com/additive-manufacturing-3d-printing-news/physna-launches-thangs-geometric-search-engine-3d-model-crea/),
  [Geo Week](https://www.geoweeknews.com/articles/thangs-3d-model-search-engine-launches-with-more-than-a-million-searchable-objects-aims-to-be-a-3d-google/)).
  This proves the product idea. It's centralized and closed.
- **MakerWorld/Bambu** launched a "Creator Copyright Support" program: it files takedowns on
  third-party sites for Exclusive models and plans "originality verification" and copy monitoring
  ([3DPI](https://3dprintingindustry.com/news/protecting-3d-printing-designs-makerworld-introduces-creator-copyright-program-248798/),
  [Fabbaloo](https://www.fabbaloo.com/news/design-theft-crisis-in-3d-model-platforms-sparks-legal-battles)).
  MakerWorld claims 4 000+ exclusive designs were re-posted on Creality Cloud, Nexprint and
  MakerOnline, and reports 200+ takedowns by Feb 2026
  ([Fabbaloo](https://www.fabbaloo.com/?p=234766)).
- **Printables / Thingiverse / Cults / Shapeways** rely on report forms, DMCA (or EU equivalents for
  Cults) and manual review. In takedown requests, "links to originals with earlier dates" are the
  de-facto evidence
  ([Bambu forum: theft on Printables](https://forum.bambulab.com/t/theft-on-printables/85384);
  [Cults/DMCA thread](https://forum.bambulab.com/t/cults-appears-to-be-violating-the-dmca-but-im-no-big-city-lawyer/256145)) **[?]**.
  I found no public automated geometric detection on these four sites **[?]**.
- **Open source:** [ascribe/3dmatch (`match3d` on PyPI)](https://github.com/ascribe/3dmatch) does
  "store and search STL files for similar designs" (old, from the ascribe.io provenance startup; I
  didn't check which descriptor it uses **[?]**). [trimesh](https://github.com/mikedh/trimesh) and
  Open3D supply the building blocks (sampling, FPFH features). I don't know of a maintained,
  dedicated STL plagiarism tool.

---

## 2. Provenance and priority

### 2.1 Timestamping

- **Primitive:** linked or anchored timestamps
  ([Haber & Stornetta 1991](https://doi.org/10.1007/BF00196791)). **Batching:**
  [OpenTimestamps](https://opentimestamps.org/) aggregates many hashes into one Merkle root anchored
  in a Bitcoin transaction. A proof is a Merkle path plus the block header, small and verifiable
  offline. The centralized alternative is RFC 3161 TSAs.
- **For us:** a calendar-style aggregator (any hub) batches commitments and writes the root to LEZ
  once per epoch. That costs one tx per batch, not per creator. Per [notes 03](03-lez-chain.md),
  LEZ blocks are inscribed into a Bedrock channel, so inclusion inherits L1 ordering. The client
  keeps `{leaf, path, root, LEZ tx/block ref}`. If the aggregator withholds the path, the creator
  re-submits elsewhere, so we should run 2–3 aggregators.

### 2.2 Commit–reveal

Commitment: `C = H("fp-commit/v1" ‖ salt ‖ sha256(file) ‖ fp/v1 ‖ authorPub)`.
- **Private:** without the salt nobody learns the file, the fingerprint or even the author. A
  creator can anchor WIP drafts privately.
- **Reveal** happens at listing time or during a dispute. Including `authorPub` stops a thief from
  claiming someone else's commitment as their own, because the commitment binds the key.
- The fingerprint is inside the commitment, so a later dispute can show "my draft at T₀ was already
  ≥0.9 similar to the suspect" without revealing the draft file. The file *can* be revealed to a
  chosen labeler.

### 2.3 Signed authorship

Each creator has a pseudonymous signing key (loam-identity; optional Keycard custody). Listings and
commitments are signed. Signatures prove *control of a key*, not creative authorship. They do let a
creator's whole back catalogue and draft history form one consistent identity.

### 2.4 C2PA / Content Credentials

C2PA 2.3 lists embedding for JPEG/PNG/TIFF/WebP/SVG/GIF, BMFF video, audio, PDF and fonts. **I
found no 3D format (glTF, USD, STL, 3MF)** in the spec
([C2PA 2.3](https://spec.c2pa.org/specifications/specifications/2.3/specs/C2PA_Specification.html)).
glTF has XMP metadata (`KHR_xmp_json_ld`) for authorship and licence
([Khronos blog](https://www.khronos.org/blog/pervasive-asset-metadata-in-3dcommerce)). It's
unsigned and stripped by any re-export. **Conclusion:** C2PA is not useful for the meshes
themselves today. We could reuse its *concepts* (a signed manifest chain of ingredients and actions)
for our listing format, and it could sign listing *photos*. A sidecar or detached manifest is the
only workable form anyway, because thieves strip metadata.

### 2.5 Linking off-platform identity (optional)

- **Keybase-style proofs:** post a statement signed by the creator key on the Printables or
  MakerWorld profile, GitHub or a personal site. Weak against later deletion, but archivable.
- **zkTLS / TLSNotary** ([tlsnotary.org](https://tlsnotary.org/)): prove "printables.com/@alice
  showed model X uploaded on date D" without the site cooperating. That makes an *external* upload
  date into evidence. **Wayback Machine** snapshots are a cheaper, weaker version.
- Linking an identity is a privacy cost. It must stay opt-in, and it's per-claim, not per-account.

### 2.6 Attacks

- **Backdating is impossible** once a commitment is anchored. **Front-running is the real threat.**
  A thief scrapes Printables or MakerWorld, anchors everything on our network first, and becomes
  "earliest" for creators who haven't joined. *Mitigations:* (i) priority rules count **external
  evidence** (zkTLS or archive dates earlier than our anchor); (ii) **draft chains**: genuine
  authors usually have earlier, *dissimilar-but-converging* versions, CAD sources and photos of
  prints, while thieves only have the final mesh; (iii) **mass-anchoring heuristics**: a key that
  anchored 5 000 fingerprints matching external popular models is suspicious. Labelers can flag the
  key itself.
- **Squatting:** a thief anchors without listing, then waits. Commitments are private, so we can't
  see squatting before the reveal. The defence is the same as above.
- **Mempool front-running of a reveal:** none to worry about. Revealing doesn't transfer
  priority, because the commitment binds `authorPub`.
- **Aggregator censorship or delay:** use several aggregators. Proofs are self-contained.

---

## 3. Decentralized labeling and flagging

### 3.1 Existing systems

- **AT Protocol labelers / Ozone.** Anyone runs a labeler service. A label is
  `{src (labeler DID), uri, cid?, val, neg?, cts, exp?, sig}` signed over its DAG-CBOR encoding.
  Clients pick labelers through the `atproto-accept-labelers` header, and the AppView attaches those
  labels. Users subscribe to a limited number (about 20 in the app, Bluesky's own is hardcoded) and
  choose per-label behaviour (hide, warn, ignore)
  ([Bluesky docs: labels & moderation](https://docs.bsky.app/docs/advanced-guides/moderation);
  [moderation architecture](https://docs.bsky.app/blog/blueskys-moderation-architecture)).
  [Ozone](https://github.com/bluesky-social/ozone) is the open-source review tool. *Sybil
  resistance is delegated to choice*: a spam labeler only reaches users who subscribed. **This is
  the closest model for us.**
- **Nostr.** [NIP-56](https://github.com/nostr-protocol/nips/blob/master/56.md) reports (kind 1984,
  typed as spam, impersonation, illegal, other…) and
  [NIP-32](https://github.com/nostr-protocol/nips/blob/master/32.md) labels (kind 1985,
  namespaced). Clients are told to only count reports from people the user follows, which is
  web-of-trust filtering. WoT relays and scores (e.g. follow-graph PageRank variants) extend this
  **[?]**. Raw reports are trivially sybilable.
- **Fediverse.** Admin-level domain blocks and shared blocklists (e.g. Oliphant's tiers, The Bad
  Space), and FIRES, a proposal for replicating moderation advisories and recommendations between
  servers **[?]**. Trust sits with server admins, a federated version of centralization. Known
  failure modes: over-blocking and opaque provenance of list entries.
- **Community Notes (X).** Matrix factorisation splits each rating into a viewpoint-factor term and
  a note intercept. A note shows only if raters who usually *disagree* both rate it helpful
  ([Wojcik et al. 2022, arXiv 2210.15723](https://arxiv.org/abs/2210.15723);
  [open-source scorer](https://github.com/twitter/communitynotes)). This resists one-sided
  brigading. It needs a big rater base, raters still need some sybil control (X uses account age
  and a phone number), and it is slow.

### 3.2 Sybil-resistance options

| Option | Strength | Privacy cost | Fit |
|---|---|---|---|
| Web of trust / vouching (follow graph, SybilRank-style) | Moderate. Attack edges are limited ([Douceur 2002](https://doi.org/10.1007/3-540-45748-8_24) on the impossibility without a trusted authority) | Social graph is public | Good for *client-side* filtering |
| **RLN membership** (Rate-Limiting Nullifier, Waku/Vac) | Bounds flags per epoch per membership; double-signalling reveals the key | Good: anonymous within the membership set | **Already in our stack.** Note the budget is per-node (see RLN memory) |
| Stake / bond, slashable on labeler verdicts | Strong economic cost | Payment trail unless LEZ private accounts | Good for *high-weight* flags and disputes |
| Account age / anchored history (has listings, sales, old anchors) | Moderate, cheap | Low | Good as a weight |
| Proof-of-personhood (World ID, BrightID, Proof of Humanity, Idena) | Strong per-human | High, or external dependency | Optional, never required |

---

## 4. Proposed design: verifiable copy flags

### 4.1 Evidence object (signed, content-addressed, stored in Logos Storage, referenced on the wire)

```
CopyFlag/v1 {
  suspect:  { listingId, fileCid, fileSha256, fp/v1, anchorProof? }
  original: { listingId | external{url, zkTLS/archive proof}, fileCid?, fp/v1, anchorProof }
  similarity: { fpVersion, f1_emd, f2_jaccard, f2_containment(o→s), f2_containment(s→o),
                trivialityScore, idfWeightedScore }        // claims, to be recomputed
  priority:  { originalAnchorT, suspectAnchorT, draftChain?: [anchorProof…] }
  remixCheck:{ suspectDeclaredParents: [...], originalInParents: bool }
  licenseCheck:{ originalLicense (e.g. CC-BY-NC-ND), suspectIsPaid, suspectLicense, conflict: enum }
  paidEvidence?: [BuyerAttestation…]
  flagger:   { pubkey | rlnProof, stakeRef? }, sig
}
```

**Labels** (AT-style, signed by a labeler key) point to a CopyFlag and say one of:
`possible-copy:auto`, `likely-copy:reviewed`, `declared-remix`, `licence-conflict`, `disputed`,
`cleared`. The original author can publish a `CopyDispute` that references counter-evidence: earlier
anchors, a draft chain, or external proofs.

### 4.2 Recompute versus trust

| Item | The client… |
|---|---|
| Signatures, anchor Merkle paths vs LEZ roots | **verifies** (light client or a trusted LEZ RPC) |
| fp/v1 of public files | **recomputes** from the files (desktop, ms–s). Mobile may recompute only F1 |
| Similarity scores and thresholds | **recomputes** from fingerprints. Drops the flag if it isn't reproducible within margin |
| Remix declaration and licence | **verifies** from the signed listing metadata |
| External-priority proofs (zkTLS, archive) | verifies the zkTLS proof. Archive snapshots are trusted at a weight |
| "This is theft" (intent, fair use, independent creation) | **trusts** the labelers it subscribed to |
| Paid or encrypted content | **trusts** buyer attestations (below) |

Default UI: show the auto label only when the evidence reproduces, the shapes aren't trivial, the
original's anchor is earlier, *and* no declared-remix covers it. Show the "Original by @X" link
prominently and don't hide anything. Censorship resistance is kept: labels inform, they never
delete.

### 4.3 Paid or encrypted models

The fingerprint can't be computed from ciphertext. Options:
1. **Seller-declared fp at listing.** It's cheap, and a thief can lie: they declare a random fp.
2. **Buyer attestation.** After purchase, the buyer's client recomputes fp from the decrypted file
   and signs `{listingId, ciphertextCid, sha256(plaintext), fp/v1}`. Several independent buyers
   (RLN-gated, purchase proven by the LEZ payment) form a quorum. A mismatch with the declared fp is
   flag-worthy on its own. Risks: the thief's sock-puppet buyers, and colluding competitors.
3. **Key disclosure to a labeler.** A buyer or flagger re-shares the content key with a chosen
   labeler, who recomputes. This leaks the paid file to one more party, which is acceptable for a
   dispute.
4. **zk proof** (RISC0 guest on LEZ): "fp = FP(Dec(k, ciphertext))". Feasible in principle, but
   multi-MB meshes make the proving cost high. Maybe on a decimated canonical mesh. Research item.

### 4.4 False positives and contesting

- Triviality gate plus IDF weighting (§1.4). Never auto-label trivial pairs.
- Declared remixes, and "parent-of-parent" chains for Benchy-like famous bases, suppress
  `possible-copy`. A licence conflict (ND or NC original, sold derivative) produces a separate
  `licence-conflict` label.
- **Independent creation** is real for functional parts (brackets, hinges). Evidence can't settle
  it. The labeler's judgement does.
- A `CopyDispute` by the original key gets equal visibility. Labelers that consistently mislabel
  lose subscribers. That's the AT Protocol market dynamic.

### 4.5 Abuse (false flags against competitors)

- Evidence-gating: flags that don't reproduce are dropped client-side, so a false flag needs a
  *real* similar, earlier listing.
- RLN rate limits per flagger membership. An optional bond is slashed when reviewing labelers mark
  the flag as abusive.
- Bridging-style aggregation of reviewer verdicts (where we have enough reviewers), so a clique of
  competitors can't push `likely-copy` through.
- Flag-the-flagger: labelers can label a key `serial-false-flagger`.

### 4.6 Open problems

1. **Front-running theft of work that predates the network.** Partly solvable with external
   evidence, which itself depends on centralized sites staying up (or on zkTLS adoption).
2. **Adaptive evasion.** The descriptor is public, so a determined thief can beat it. We only raise
   the cost.
3. **Partial copies under heavy additive edits.** The scale/density normalisation for F2 isn't
   benchmarked yet. We need a test set: Thingi10K-style models with scripted attacks (re-mesh,
   decimate 50 %, add a base or text, cut, mirror, scale, non-uniform scale) to tune thresholds and
   measure FP/FN.
4. **Paid content verification** without sharing the file needs either trusted buyers or an
   expensive zk proof.
5. **Labeler bootstrapping.** Who runs the first labelers, and why would anyone trust them? We risk
   rebuilding a de-facto central moderator.
6. **Fingerprint privacy.** F2 tokens leak some shape information for private or paid models.
   That's probably acceptable, and it isn't quantified.
7. **Mobile cost.** Recomputing F2 on 100 MB meshes on phones means mobile likely trusts
   desktop-computed fingerprints or attestations.
8. **Law.** The labels are opinions about IP. Whether a labeler takes on legal exposure depends on
   jurisdiction, and this needs advice.
