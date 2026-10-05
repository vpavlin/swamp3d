# Proof of concept: proposed scope

Ordered by how much each step de-risks the project. Each ends with something measurable.

1. **Fingerprint benchmark (the go/no-go).**
   - fp/v1 in JS: D2/A3 histogram + point-pair MinHash + LSH keys.
   - A dataset of ~200 real printable models (CC-licensed), plus a scripted attack suite: re-export,
     re-mesh, decimate, scale, mirror, rotate, add base, add text, cut, hollow.
   - Report detection rate per attack, false positives on unrelated pairs and on "trivially similar"
     pairs (boxes, brackets), and the time per model.
   - Then a C++ port with parity vectors.
2. **Anchoring.** An epoch Merkle builder + inclusion proofs, behind an `Anchor` interface, with a
   local mock. Then one real anchor on LEZ testnet (or a Bedrock inscription), read back and
   verified.
3. **Catalogue core.** A loam-sync event log for models/versions/makes/labels with the two-id file
   rule; a headless hub that pins every CID with a long TTL; publish → second peer fetches → verifies.
4. **The theft states end to end.** Two peers: one publishes an original; the other publishes an
   attacked copy without declaring a parent. The second peer's listing shows "Possible copy of X"
   with the original's link, computed locally; then a labeler and a dispute.
5. **Basecamp view.** Browse, model page with thumbnails and labels, "open in slicer", publish.
6. Later: tips over LEZ, then paid models.
