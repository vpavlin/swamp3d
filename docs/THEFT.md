# Flagging stolen models without a moderator

Goal (vpavlin, 2026-10-05): we can't *prevent* theft without giving up privacy, anonymity or
censorship resistance, but we must be able to **flag** a model that is an unfaithful copy, so that
people can see it and go find the original author. Prior art and numbers: notes/05a.

## Principles

1. **Label, never delete.** A flag changes what people see next to a model, not whether it exists.
2. **Evidence, not authority.** A flag carries the evidence; every client re-checks it and ignores
   flags it can't reproduce. A labeler's opinion is shown as an opinion, under its name.
3. **Pseudonymous all the way.** Creators, flaggers and labelers are keys. Proving you're the
   original author never requires a real name (it can, optionally, link an external account).
4. **Point to the original.** The most useful output is "Earlier original: <model> by <key>,
   published <date>", one click away.
5. **Make honest behaviour cheap and lying expensive**, rather than impossible.

## The four signals

| signal | what it says | how a client checks it |
|---|---|---|
| **Shape match** | the meshes are near-identical or one contains the other | recomputes two fingerprints from the files: a global shape histogram (D2/A3) and a MinHash over local point-pair features, which survives added bases, cuts and text. Exact copies match by hash. |
| **Priority** | the original was published (anchored) earlier | verifies the Merkle inclusion proof against the epoch root anchored on LEZ/Bedrock |
| **Declared lineage** | the suspect does / doesn't name the original as a parent | reads the signed version metadata |
| **Licence** | e.g. a non-commercial or no-derivatives original, sold as a paid derivative | reads both licences (SPDX/CC ids) |

Combined, client-side, into states shown next to the model:

- **Original** (no earlier match) · **Declared remix of X** (match + parent declared) ·
- **Possible copy of X** (match + X anchored earlier + no declared parent; automatic, shown only
  if the shape isn't trivial) · **Licence conflict with X** ·
- **Reviewed: likely copy / cleared** (a labeler you subscribe to says so, with its name) ·
- **Disputed** (the author published counter-evidence).

## How it works

1. **At publish**, the creator's app computes the fingerprint (`fp/v1`, about 1 KB) and LSH keys,
   and puts them in the signed version event. The version is included in the next epoch anchor;
   the creator gets an inclusion proof: a portable "published before T" certificate.
   Optional **private pre-anchoring**: commit `H(salt ‖ fileHash ‖ fp ‖ authorKey)` while the model
   is still a draft; reveal later. A thief who only has the final mesh can't produce a draft chain.
2. **Search for earlier matches**: every desktop client keeps an LSH index over the catalogue it
   syncs (~280 MB per million listings). Optional indexers can serve lookups, but clients re-verify
   every candidate, so a lying indexer can only hide results, not invent them.
3. **Automatic labels** appear without anyone filing anything: when your client sees a match with
   an earlier anchored model and no declared parent, it shows "Possible copy of X".
4. **Flags** (`CopyFlag/v1`, notes/05a section 4.1): anyone can file one. It's an evidence object,
   stored as a blob and referenced by a small signed event. Flaggers are rate-limited by RLN
   membership (one rate per member), optionally with a bond.
5. **Labelers** (AT Protocol style): anyone can run one; users choose which they subscribe to (the
   app ships with none or a small default set, and says so). A labeler reviews flags and publishes
   `likely-copy`, `cleared`, `serial-false-flagger` and so on, signed under its key.
6. **Disputes**: the accused author publishes a `CopyDispute` with counter-evidence (earlier
   anchors, a draft chain, an external proof). It's shown with equal weight.

## Paid (encrypted) models

The fingerprint can't be computed from ciphertext, and a thief can declare a fake one. So:

1. The seller declares `fp` in the listing (cheap, can lie).
2. **Buyers attest.** After purchase, the buyer's client recomputes `fp` from the decrypted file and
   can publish a signed attestation, tied to their LEZ payment. A declared-vs-attested mismatch is a
   flag on its own; a match against an earlier original is a normal flag.
3. In a dispute, a buyer can share the file key with one labeler to re-check.
4. Later research: a RISC Zero proof "fp = FP(decrypt(k, file))" on a decimated mesh.

## Attacks and limits (honest list)

- **Front-running**: a thief scrapes Printables and anchors popular models before the real
  creators join. Priority alone then points the wrong way. Mitigations: creators importing their
  own back catalogue attach external evidence (an archived page, later zkTLS proofs of the
  original site's upload date); flag keys that anchor thousands of external-looking models.
- **Adaptive evasion**: the descriptor is public; a determined thief can beat it. We raise the
  cost, we don't stop it.
- **Common shapes** (cubes, boxes, brackets, Benchy remixes): never auto-labelled; a triviality
  gate and down-weighting of frequent shapes.
- **Independent creation** of functional parts is real; only a labeler's judgement can say.
- **False flags against competitors**: a flag needs a real, earlier, similar model to reproduce;
  RLN limits volume; labelers can mark serial false flaggers.
- **Labeler bootstrap**: someone has to run the first ones; that's a soft centralisation we accept
  and disclose.
- **Legal**: labels are opinions about IP. Labeler operators need advice for their jurisdiction.

## What to prove first (PoC)

1. `fp/v1` in JS and C++, byte-identical, on real models (Thingi10K-style set).
2. A scripted attack suite: re-export, re-mesh, decimate 50%, scale, mirror, rotate, add a base,
   add text, cut in half, hollow. Measure detection vs false positives on unrelated and on
   trivially similar models. **This decides whether the approach works at all.**
3. Epoch anchoring with inclusion proofs (anchor target behind an interface; local mock first).
4. The client-side state machine (Original / Remix / Possible copy / Disputed) over a synthetic
   catalogue, then in a Basecamp view.
