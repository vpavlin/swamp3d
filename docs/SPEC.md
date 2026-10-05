# Swamp protocol and module specification (M1: the basics)

Status: M1 draft, 2026-10-05. Scope: publish, browse, search, download, comments and makes for
**free public models**, with a pinning hub. Fingerprints are computed and stored at publish so the
theft labels (docs/THEFT.md) can be switched on later. Out of M1: tips, LEZ anchoring, labels,
paid models, Android.

## 1. Roles and identity

- Every installation has a **device identity**: a secp256k1 key in the core's data dir, address
  `0x` + last 20 bytes of `sha256(compressed pubkey)` (loam-sync signing, domain `swamp`).
  Later: a loam identity / Keycard per person (loam-sync delegation certs).
- **Creator** = the author of a model's `model.create`. Only the creator may add versions or
  retract the model. Anyone may announce a CID for a file; the creator's come first (§ 4, ADR 0010).
- **Hub** = the same core running headless with `SWAMP_HUB=1`: it caches every file it sees.

## 2. Transport

- **Topics (ADR 0014, protocol v2):** one topic per category, `/swamp/2/cat/<category>/proto`,
  and one people topic, `/swamp/2/people/proto` for profiles, via `loam_core`. Each event belongs on
  exactly one topic, derived from its content (`topicOf`): profiles → people; a model and
  everything about it (versions, retractions, CIDs, comments, likes, makes) → its category.
  - A model's category is fixed by its `model.create`; unknown or missing = `other`.
  - A node subscribes to some categories: it joins their topics and runs catch-up per topic, over
    that topic's events only.
  - Publishing in a category subscribes you to it.
  - A node with no saved choice follows every category (until index snapshots exist, ADR 0015);
    `SWAMP_CATEGORIES` overrides; a hub follows all.
  - On a topic it only sends to, a node keeps only events about models it already holds.
  - Frames are JSON, base64-encoded once; receive peels up to three base64 layers.
- Frames: `{"t":"ev","e":<event>}` (one event), `{"t":"evs","es":[<event>…]}` (a batch, ≤ 48 KB,
  used when serving catch-up) and loam-sync catch-up frames (`fp` / `ids` / `need`,
  `logos_sync::catchup`), answered by any peer. Every frame from the wire is shape-checked first;
  anything malformed is dropped and counted (`rxBad`).
- Catch-up: once on connect (3/10/25 s ladder), then every 2 minutes (RLN budget:
  logos-rln-budget). Snapshots to Storage are M2.
- Answering is budgeted: a peer's opening `fp` is answered at most once per 20 s; served events ≤
  200 per minute in total; catch-up frames older than 2 minutes (the fleet store replays them on
  every connect) are ignored. Events are ingested whatever their age.
- An event is verified once, when it enters the log; the fold then trusts the log. The device clock
  advances on every admitted event, unless it is more than 5 minutes in the future.
- **Not sealed.** The catalogue is public by intent; integrity comes from signatures (ADR 0010).

## 3. Events

loam-sync envelopes `{v, id, type, hlc, dev, payload, pub, sig}`, signed. The fold drops anything
unsigned, badly signed, or of an unknown type. Payload size limit: 16 KiB per event.

| type | author | payload | fold rule |
|---|---|---|---|
| `profile.put` | anyone | `{name, bio?}` | LWW per author |
| `model.create` | creator | `{modelId, title}` | first event per `modelId` wins (HLC order); `modelId` must equal `hex(sha256(author ‖ nonce))[0..32]` with `nonce` in the payload |
| `model.version` | creator | see 3.1 | `v` must be 1 + the latest accepted version; first writer per `(modelId, v)` wins; immutable |
| `blob.cids` | anyone | `{cids: {sha256: cid}}` | candidate CIDs per hash (first 4 by HLC); a downloader tries them in order and keeps only bytes that hash to `sha256`, so mirrors and hubs can announce CIDs too |
| `model.retract` | creator | `{modelId, reason?}` | tombstone: hidden by default, files stay content-addressed |
| `comment.post` | anyone | `{modelId, text, replyTo?}` | append |
| `make.post` | anyone | `{modelId, v?, text, images: [{sha256, size, mime}]}` | append |
| `like.put` | anyone | `{modelId, on: bool}` | LWW per (author, model) |

### 3.1 `model.version`

```json
{
  "modelId": "…32 hex…", "v": 1,
  "title": "…", "summary": "≤ 280 chars", "description": "markdown, ≤ 8 KiB",
  "tags": ["…"], "licence": "CC-BY-4.0",
  "parents": [{"modelId": "…", "v": 2}],
  "files":  [{"name": "part.stl", "kind": "model|source|profile|other", "size": 123, "sha256": "…"}],
  "images": [{"kind": "thumb|photo", "size": 123, "sha256": "…", "mime": "image/png"}],
  "fp": {"v": 1, "sha256": "…", "d2": [64], "a3": [32]}
}
```

- `licence` is an SPDX id (`CC0-1.0`, `CC-BY-4.0`, `CC-BY-SA-4.0`, `CC-BY-NC-4.0`,
  `CC-BY-NC-SA-4.0`, `CC-BY-ND-4.0`, `CC-BY-NC-ND-4.0`, `MIT`, `GPL-3.0-or-later`, …) or
  `LicenseRef-<name>`.
- `files` / `images` carry the plain `sha256` of the bytes. Free models are public: the hash is
  the identity of the content, and equal hashes are exactly what makes exact copies visible
  (ADR 0011).
- `fp`: the fingerprint of the first model file (fp/v1, `packages/fp`). The full fingerprint
  (including the local-feature histogram) is a JSON blob in Storage referenced by `fp.sha256`; the
  global histograms are inlined, rounded to 4 decimals, so a shortlist can run without a fetch.

## 4. Files (Logos Storage)

- Two ids: `sha256` (known at publish, in the event) and `cid` (announced by a `blob.cids` event
  when the upload completes - by the creator, or by any mirror). Uploads are staged under the file name `<sha256>` so anyone re-uploading the
  same bytes gets the same CID.
- **The host owns Storage** (Basecamp 0.3 / logosctl): the core never calls `init` (a successful
  `init` would replace the host's config for every module) unless `SWAMP_STORAGE_CFG` is set, for a
  host that doesn't run Storage itself. It adopts the host's node and never stops or reconfigures it.
- Upload: `uploadUrl(path, 65536, true)`; done by `storageUploadDone` **or** by `manifests()` listing
  the staged file name. Every blob this node is responsible for (its versions' files, images and
  fingerprint, its makes' photos) is retried every 60 s until it has a CID. CIDs go out batched,
  ≤ 64 per `blob.cids` event.
- Download: `downloadToUrlAsyncResult(cid, path, false, 65536, false, true, cb, 60000)`; completion
  by `storageDownloadDone` **or** by polling (the part file reaches the size the signed version
  declares). A part file that grows past that size is cut off. The bytes are hashed while streaming
  and must equal `sha256`, or the file is rejected and the next candidate CID tried. After a full
  round of candidates: back-off (15 s, doubling, ≤ 30 min); a download job fails after 3 rounds,
  with the reason. In flight at once: 6 overall, 2 for previews, 3 for a hub's sweep.
- A **hub** (`SWAMP_HUB=1`) fetches every file, image and fingerprint it sees, with the same retry
  rules, and keeps them.
- **Finding a holder needs a common DHT.** Two fresh nodes don't find each other's content by
  themselves: the fetching node needs a `bootstrap-node` (the holder's or a hub's `spr`) in its
  host Storage `config.json`, and the holder needs an announced address (`nat: extip:<addr>`).
  Measured on logosctl 0.3.1: without the bootstrap, "failed to get manifest"; with it, the file
  arrived byte-identical. `config.json` keys that libstorage doesn't know (e.g. `disc-port`) make
  the whole file fail to load.
- **Latency of a fresh upload:** on logos.test the first fetch of a just-published file took 95-135 s
  (DHT provider propagation; the first manifest lookups fail). A user download pauses preview and
  hub fetches until it's done. Its Storage config (the host's `config.json`) needs a public `extip`,
  `autonat-server`, `relay-server` and a long `block-ttl` (logos-storage).

## 5. Fold → catalogue state

`models[modelId]` = `{modelId, creator, created, title (latest version), versions[], retracted,
likes, comments[], makes[]}`; `cids[sha256]` = candidate CIDs. Search is local:
case-insensitive substring over title, summary and tags; sort by newest, most liked, most makes.

## 6. Core module API (swamp_core)

All methods return JSON strings (`{"ok":true,…}` / `{"ok":false,"error":"…"}`), at most 4 string
arguments, no default arguments.

| method | does |
|---|---|
| `snapshot()` | status, me (address, profile), counters, storage status |
| `listModels(queryJson)` | `{q, tag, sort, limit}` → model cards (latest version, thumb path when local) |
| `getModel(modelId)` | full model: versions, files with local/remote state, comments, makes |
| `publish(draftJson)` | `{modelId?, title, summary, description, tags, licence, parents, files:[{path, kind}], images:[{path, kind}]}`; new model or next version; hashes, fingerprints, stages + uploads files; returns `{modelId, v}` immediately (local-first) |
| `download(modelId, v)` | fetch every file of that version into `~/Swamp/<title>-<modelId8>-v<v>/`; progress via `getModel` |
| `comment(modelId, text)`, `postMake(modelId, makeJson)`, `like(modelId, on)` | community events |
| `setProfile(profileJson)` | `{name, bio}` |
| `setCategories(listJson)` | the categories to follow: `["tools","toys"]` or `["all"]`; categories you publish in always stay. `snapshot().categories` lists `{id, label, subscribed, models}` |
| `cacheImage(sha, viewDir)` | pictures are fetched lazily: if the node doesn't hold it yet, this returns `{ok:false, pending:true}` and fetches it (ask again in a few seconds). Otherwise: copy a picture the node holds (a listed thumbnail or photo) into `<viewDir>/cache/` and return its path. Basecamp 0.3 sandboxes views: a view may load only `qrc:` and files under its own plugin dir — no `file://` elsewhere, no `data:` URLs. `viewDir` must be a directory named `swamp`; the cache keeps ≤ 300 files |
| `retract(modelId, reason)` | creator only |
| `resync()` | catch-up now |

Event: `stateChanged(summaryJson)` on every change (the view also polls `snapshot`).

## 7. Local state

`$SWAMP_CORE_DATA` or `~/.swamp-core`: `identity.json`, `catalog.json` (the event log), `uploads.json`
(pending uploads: sha256 → session, path), `downloads.json`, `files/` (staged + cached blobs by
sha256), `fp/` (fingerprint blobs). Downloads land in `~/Swamp/`.
