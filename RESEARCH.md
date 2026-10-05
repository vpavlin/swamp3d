# Synthesis

Date: 2026-10-05. Sources: notes/01-05a (each cites its own). This is the "what we'd build and
why" view; the decisions are in docs/adr/ (all *Proposed*).

## 1. What we're building

A library of 3D models and maker projects with no company in the middle:

- **Creators** publish a *model*: files (STL, 3MF, STEP, OBJ, source), photos, a description or
  instructions, a licence, optional print profiles, optional price. Each edit is a new immutable
  **version**; the model id stays stable.
- **Makers** browse, download, open in their slicer, post *makes* (photos of prints), comment, rate,
  remix (with a declared parent), collect and follow.
- **Everyone** can see, for any model, who published it first and whether it looks like a copy of
  something else, with evidence their own app checks.

Non-negotiables: **pseudonymity** (a key, not a name), **privacy** (no tracking, private
downloads possible), **censorship resistance** (no global delete; things are labelled, not
removed), **local-first** (works offline, syncs later), **no lock-in** (open formats, slicer
handoff, exportable).

## 2. What the centralized sites teach us (01)

- Fees are 10-20% plus payment processing; reward points are opaque and tied to exclusivity
  (MakerWorld). Creators want ownership and portability.
- Lock-in is technical: Bambu-flavoured 3MF profiles, cloud printing, firmware gating.
- **Theft is the #1 creator complaint**: re-uploads, resold as "free to sell", handled by hand
  (MakerWorld ~4,000 cases). The de facto evidence is "here is the earlier original".
- Takedown pressure is real (gun files, Games Workshop, LEGO). Without a host there's nobody to
  send a notice to, so the pressure lands on hub operators and list curators.
- Prior decentralized attempts failed on discovery (Manyfold: no global index) or because the token
  pointed at a centralized file (NFT marketplaces).

## 3. How the Logos tech stack maps

| need | Logos piece | status / caveat |
|---|---|---|
| catalogue, versions, comments, makes, ratings, follows, flags | **Messaging** (Delivery via Loam) + **loam-sync** signed event log, RBSR catch-up | proven in our apps; RLN budget: 100 msgs / 10 min per node, so the catalogue must come from snapshots, not message replay |
| model files, photos, previews | **Logos Storage** (CIDs, DHT, direct dials) | best-effort only: **no durability**, 30-day default block TTL, NAT-to-NAT can't move big files, phone is fetch-only → **pinning hubs required** |
| catalogue snapshots | Storage (a sealed or public snapshot CID) + tail sync over Messaging | pattern from loam-sync ADR 0020 |
| payments | **LEZ** native token via `lez_core`; a small `purchase` program to say *which* model was paid for | testnet only, resets; public payments cheap; private payments ~2 min proving, no mobile |
| "published before T" | **anchoring**: one Merkle root per epoch, by a hub, into LEZ (registry program) or a **Bedrock channel inscription** | cheap; anchors advisory while testnet resets; inscriptions need tooling the Basecamp module lacks |
| creator identity | secp256k1 key (loam-sync, Keycard-capable) bound to a LEZ account | LEZ wallet can't sign messages; bind both ways |
| UI | Basecamp module (C++ core + QML view); Android later | no Qt Quick 3D in Basecamp; thumbnails + slicer handoff first; WebEngine 3D viewer is a spike |

## 4. Architecture (proposed)

```
 Basecamp view (QML) ── mulenet-style pure QML, thumbnails, "open in slicer"
        │ callModuleAsync
 core module (C++) ── catalogue fold, fingerprints, search index, purchase/key flow, label checks
   ├─ loam_core ── Messaging: catalogue events, comments, makes, flags, key delivery
   ├─ storage_module ── files, previews, snapshots (sealed for paid files)
   └─ lez_core ── purchase transactions, anchor proofs lookup
 pinning hub (same core, headless) ── caches every CID it sees, long TTL, anchors epoch roots,
                                       optional labeler, optional indexer
```

**Catalogue events** (signed by the creator): `model.create {modelId}`, `model.version {modelId,
v, files[], previews[], licence, parents[], price?, fp}`, `model.retract` (a creator's own
tombstone; the files stay content-addressed), `make.post`, `comment.post`, `rating.put`,
`collection.put`, `follow.put`. Labels and flags: `label.put` by labelers (docs/THEFT.md).

**Files**: two-id rule from notes/02: `blobId = sha256(stored bytes)` known at once, `cid` added
later; uploaded under the canonical name `<blobId>` so any mirror reproduces the same CID. Paid
files: chunked encryption with a random per-file key.

**Paid models (phase 1)**: buyer pays through `purchase` on LEZ (receipt + chained transfer in one
transaction); the seller's core *or their hub* sees the receipt and sends the file key sealed to
the buyer over Messaging. Trust: the seller must deliver. Refund/escrow is phase 2 (a program can't
hold the key; best it can do is deliver-or-refund with a deadline). A buyer can re-share the key,
same as on every platform today.

## 5. The hard problems, and where we stand

| problem | stance |
|---|---|
| **theft / stolen copies** | evidence-carrying labels: shape fingerprints + anchored priority + declared remix + licence checks; clients recompute; labels inform, never delete → docs/THEFT.md |
| durability | pinning hubs (anyone can run one), "seeded by N" shown on every model, pin requests; no paid persistence exists in Logos Storage today |
| moderation / legal | no global delete; subscribable labelers and blocklists; hub operators choose what they cache; default lists for clearly illegal content; needs legal advice before any public launch |
| search & ranking | local index over the synced catalogue; ranking by makes/ratings from accounts with anchored history; spam is the open risk |
| payments & payouts | LEZ tokens only; no fiat, no off-ramp; tips first, sales second |
| cold start | import from Printables/Thingiverse/Manyfold by the creator (their own models), keeping the original date as external evidence |

## 6. Open questions

1. **Which UI target**: Basecamp 0.2 (what you run) or 0.3 (host-owned Storage, logosctl)? The 0.3
   port changes Storage config ownership (the app can't set the TTL); pinning hubs could stay on 0.2
   or run under logosctl with their own config.
2. **Anchoring target**: LEZ registry program (raw lee_core v0.3 until SPEL is ported) or a Bedrock
   channel inscription (better fit, needs a signer sidecar)? Both are testnet-only.
3. **Who runs the first hubs and labelers?** Probably us plus a few friends, which is a bootstrap
   centralisation we should say out loud.
4. **Free-only MVP?** Payments add the most complexity and the least to the theft story. Suggest:
   free models + tips first, paid models second.
5. Name: Spool / Plinth / Filament Commons.
