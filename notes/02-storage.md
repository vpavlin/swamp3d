# 02 — Logos Storage for a decentralized 3D-model library

Date: 2026-10-05. Scope: hosting STL/3MF (1–200 MB) + photos for a Printables/Thingiverse-like app
on the Logos stack, Basecamp desktop UI first. Sources: local skills/memories (authoritative for our
ecosystem), `logos-storage-module` v3.0.0-rc1 header (`/home/vpavlin/logos-storage-module/src/storage_module_plugin.h`,
commit e679a87), upstream `logos-storage-nim` master 6256f06 (2026-10-03, read from a temp clone),
scala's production usage (`/home/vpavlin/scala/src/scala_impl.cpp`, ADR `docs/adr/0017-attachments-via-logos-storage.md`).

## TL;DR

Logos Storage today is a **best-effort, content-addressed p2p file-sharing layer** (libp2p Kademlia
DHT + direct block exchange, 64 KiB blocks, optional Mix-tunnelled private downloads). It has **no
durability guarantee, no marketplace, no erasure coding, no storage proofs** in the current code. A
file is available exactly as long as some *reachable* node holds and advertises it — and, by default,
**every node deletes blocks 30 days after it stored them** (`block-ttl`). For a model library this
means: run **pinning hubs** (reachable, AutoNAT+relay servers, big quota, long TTL, cache-on-see),
treat creators' desktops as opportunistic seeders, seal everything, and ship small previews as
separate CIDs.

## 1. What we get today, and the API

**Model** (`~/.claude/skills/logos-storage/SKILL.md`): upload → node chunks into 64 KiB blocks
(`DefaultBlockSize`, `storage/storagetypes.nim:33`), builds a Merkle tree + a manifest
(`treeCid, datasetSize, blockSize, filename, mimetype, protected`), returns a **manifest CID minted
after upload**. The CID includes the **filename** → identical bytes under another name = different CID.
Fetch = DHT `GetProviders` → direct dial → `/storage/manifest/1.0.0` + `/storage/blockexc/1.0.0`.
There is **no relay fleet** like Delivery has: an undialable provider is no provider.

**Desktop module API** (storage_module 3.0 / Basecamp 0.3; 2.1.x = same minus the bool flags):
- lifecycle: `init(cfgJson)`, `start()` → ev `storageStart`, `stop()` → `storageStop`, `destroy()`
  (fails while starting/stopping), `isRunning()`, `loadConfigOrDefault()`.
- upload: `uploadUrl(path, chunk, advertise)` → sessionId; ev `storageUploadProgress` (≤1/percent),
  `storageUploadDone{sessionId,cid}`. Streaming: `uploadInit/uploadChunk/uploadFinalize(→CID)/uploadCancel`.
- download: `downloadToUrl(cid, path, local, chunk, isPrivate, advertise)` → ev `storageDownloadProgress`,
  `storageDownloadDone{sessionId}` (no path — map it yourself); `downloadChunks(...)` (base64 per
  chunk — avoid for big files); `downloadCancel`; `downloadManifest` → `storageDownloadManifestDone`.
- cache/pin: `fetch(cid, isPrivate, advertise)` (background prefetch, **no completion event** —
  poll `exists(cid)`), `exists`, `remove` → `storageRemoveDone`, **new in 3.0: `getAdvertise/setAdvertise(cid, bool)`**.
- introspection: `manifests()`, `space()` (`quotaMaxBytes/UsedBytes/ReservedBytes`), `spr()`,
  `peerId()`, `debug()` (`connections` is the field to check, not just `table.nodes`), `collectMetrics()`.
- Underlying C FFI (`/tmp` clone, `library/libstorage.h`): `storage_upload_*`, `storage_download_{init,stream,chunk,manifest,cancel}`,
  `storage_fetch`, `storage_exists`, `storage_delete`, `storage_list`, `storage_space`, `storage_{get,set}_advertise`. **No range/offset read.**

**Platform realities / limitations**
- **Basecamp 0.3 owns the node**: host inits storage_module from `~/.logos_storage/config.json`
  (defaults: public `logos.test`); an app's `init()` is refused. Adopt it, never stop/reconfigure it
  (`logos-basecamp-0.3-port` SKILL § Storage, scala `port/0.3` `m_storageHostOwned`). **Consequence: the
  app cannot set extip, bootstrap, quota or TTL** on 0.3 — open question for the platform team.
  On 0.2 the app inits its own node (scala `storageConfig()`, `scala_impl.cpp:1037`).
- `fetch`/`downloadToUrl` block up to **~30 s** waiting for a manifest on 3.0 (> default IPC timeout):
  use `…AsyncResult(…, cb, 60000)`; never call from inside a storage event callback.
- **Completion events can be lost** (Basecamp 0.2.3) → poll while pending: upload done = `manifests()`
  lists the file; download done = file size == `datasetSize` (memory `storage-nat-hub-cache`, scala 0.9.39).
- **NAT**: a NAT-ed node only advertises if AutoNAT says reachable, it has `nat=extip:`, or holds a
  relay-circuit addr. Relayed links are capped (nim-libp2p 128 KB / 120 s) → **NAT↔NAT transfer of a
  10 MB STL is impossible**; only a reachable hub fetching over the uploader's own outbound connection works.
- **Address traps**: no extip = nothing announced; listen on `::` if announcing IPv6; no-extip nodes
  can't dial IPv6 (EINVAL; our patch `scala/mobile/native/logosstorage/patches/nim-libp2p/0001-…`,
  memory `libstorage-ipv6-dial-bind-bug`); ULA/mesh addrs dropped on public networks; DHT version skew
  (discv5→Kademlia, upstream #1522) once broke the public `logos.test` bootstrap (memory `codex-fetch-needs-dht-discovery`).
- **Hub DHT bug**: nim-libp2p `getProviders` ignores locally stored provider records → a hub that is
  the only DHT server can't find content; patch `0002-kad-getproviders-include-local-records.patch`, not upstream.
- **Mobile**: no Android storage_module and no REST in libstorage. Our fork builds a **fetch-only**
  arm64 client (memory `storage-android-fetch-client`, scala `mobile/native/logosstorage/build-libstorage.sh`);
  upstream logos-storage-nim#1221 is open, untouched since 2025-05. Phones = browse/download only.

## 2. Durability: who serves a file when the creator is offline?

**Upstream status (source-verified 2026-10-05):** the current tree (`storage/`) contains no
marketplace, sales, slots, validator, erasure-coding or proof code; the README calls it a *"filesharing
client that allows sharing data privately in p2p networks … pre-alpha"*
(https://github.com/logos-storage/logos-storage-nim). Old marketplace issues remain open but stale
(#575, #1296 "several active smart contracts", #1223 "Dataset health"). The Codex durability engine
(erasure coding + ZK remote auditing + marketplace, https://rfc.vac.dev/codex/raw/codex-marketplace,
https://free.technology/codex) is **not shipped**; "altruistic mode" durability is an open research
list (https://hackmd.io/xzxmkqgKQ6mq55NDL449yA). The project's own framing (DWeb Camp 2026,
https://talx.dod.ngo/dwebcamp-2026/talk/MRYKHP/): *"Content persists as long as participating devices
continue to pin it."* **Do not plan on paid persistence.**

**Trap found while reading the source — block TTL GC.** `RepoStore.putBlock` stamps every block,
uploaded or fetched, with `expiry = now + blockTtl` (`storage/stores/repostore/store.nim:223`, default
`DefaultBlockTtl = 30.days`, `types.nim:24`); `BlockMaintainer` deletes expired blocks every 10 min
(`stores/maintenance.nim`). `updateExpiry` exists in `node.nim:156` but **nothing in the FFI or module
calls it** (`onExpiryUpdate` is dead code). Re-storing a block keeps `max(old, new)` expiry
(`repostore/operations.nim`), so re-upload/re-fetch extends it. Scala sets no `block-ttl`, so its VPS
hub's cached attachments probably vanish after 30 days (unverified on a live node — verify).
Implications: hubs must set `block-ttl` very high (e.g. `"3650d"`), and on Basecamp 0.3 that is a
host-config change; desktop seeders lose files after 30 days unless re-pinned.

**Options, in order of realism**
1. **Pinning hubs (do this).** Scala's VPS model (memories `scala-vps-hub`, `storage-nat-hub-cache`):
   public IP, `nat=extip:<public>`, `autonat-server`+`relay-server` on, `no-bootstrap-node` root (or a
   node on `logos.test`), patched getProviders, cache-on-see (`fetch` on first sight of a CID in a
   listing, retry every 60 s for 30 min, sweep on restart; `scala_impl.cpp:1336-1380`), big
   `storage-quota` (default 20 GiB), long `block-ttl`, persisted `data-dir` (stable peer id).
   A hub pins only **ciphertext** it is told about — it needs no keys.
2. **Community mirrors.** Anyone can run the same hub profile headless (`logos-headless-logosctl`) and
   subscribe to the catalog topic; a "pin this collection" toggle in the desktop app turns users into
   seeders (subject to their NAT). A **pin-request** message (CID + size, no key) lets creators ask hubs
   to cache — this is what scala considered (memory `storage-nat-hub-cache`).
3. **Paid persistence** — not available; if wanted, build it at app level (pay a hub operator; hub
   attests periodically by serving random blocks to an auditor). Track upstream.

## 3. Paid / private files

- **Encrypt-then-upload, always** (charter rule; Storage has no at-rest encryption; `protected` in the
  manifest is not encryption). Per-file random content key `Kf`, AES-256-GCM (or chunked AEAD, see §4),
  `blobId = sha256(sealed)`.
- **Public free models:** still seal? Optional. For dedup + mirroring of free content, plain upload is
  fine and lets anyone verify; sealing with a key published in the listing gives nothing.
- **Paid models:** listing carries `cid`, `blobId`, size, previews (unsealed). After payment (LEZ/other
  — separate note), the creator's node or a key-escrow agent delivers `Kf` to the buyer encrypted to
  their identity key over Delivery (1 message; fits the RLN budget). Hubs cache the ciphertext safely;
  leakage = CID, size, timing, popularity. Downloads with `isPrivate=true, advertise=false` hide the
  fetcher via Mix (`docs/mix-downloads.md` upstream).
- **Limits:** no revocation — a leaked `Kf` decrypts forever; a buyer can re-share plaintext. Avoid
  convergent (content-derived) keys for private files (confirms "who has file X"). The creator's
  key-delivery agent must be online or delegated → a hub that holds `Kf` is a trusted escrow; a
  threshold/escrow design is an open question.

## 4. Large files, resumability, previews

- 200 MB = ~3 200 blocks + Merkle proofs; no protocol size cap beyond quota. Upload via `uploadUrl`
  (file on disk), download via `downloadToUrl` (file on disk) — never `downloadChunks` (base64 over IPC).
- **Resume is implicit and block-level:** downloaded blocks land in the local repo; a restarted download
  of the same CID skips blocks already stored (download workers check local storage first). There is
  **no byte-range/offset read**, so no partial "open the first N MB"; plan whole-file fetch.
- Blocks are fetched from any advertising provider (block exchange), so several hubs share load.
- Sealing: one AEAD over 200 MB forces whole-file buffering; prefer a **chunked AEAD** (e.g. 1 MiB
  segments, STREAM-style nonce = counter) so seal/open stream with bounded memory.
- **Previews as separate small CIDs**, never inside messages (Delivery ~150 KB/message, RLN budget
  100 msgs/10 min per node, `logos-rln-budget`): `thumb.webp` (≤100 KB), 2–6 photos (≤500 KB each,
  re-encoded, EXIF stripped), optional decimated preview mesh (`preview.glb`/simplified STL ≤2 MB) for a
  3D viewer. Fetch previews eagerly (cache-on-see by hubs and browsers), full model on demand.
- Multi-file "things" (several STLs + 3MF + README): upload each part (dedup across remixes) **plus**
  an optional zip bundle; the listing's `files[]` references each part's CID.

## 5. Recommended design

**Listing event** (append-only, signed, on Delivery; one message):
```
model.put { modelId, rev, title, license, tags, creator, price?,
  files: [{ name, mime, size, sha256Plain?, blobId, cid, sealed: bool, sealScheme }],
  previews: [{ kind: thumb|photo|mesh, blobId, cid, size, mime }],
  hubs: [spr...] }          // where it was last seen pinned (hint, not authority)
```
- **Two-id rule**: `blobId = sha256(stored bytes)` known at capture (local-first; the listing is
  authored immediately, status "uploading"); `cid` arrives later in a `model.cid` update event (LWW
  per file). Readers fetch by `cid`, verify `sha256 == blobId` before opening/rendering. Upload under a
  **canonical filename** (`<blobId>`) so any mirror re-uploading the same bytes reproduces the same CID.
- Upload is never awaited on the UI path (charter rule 3); progress via events + poll fallback.
- **Catalog/backfill via Storage snapshots** (sealed or public catalog snapshot CID + tail
  reconciliation over Delivery) instead of replaying thousands of listings (RLN).
- Hub profile: `extip`, AutoNAT+relay servers, patched libp2p, `block-ttl` ≥ years, quota sized to
  the corpus, cache-on-see for `files` + `previews`, honour `pin-request`, periodic `exists` audit +
  re-fetch, and an ops metric "CIDs with 0 reachable providers".
- Clients bootstrap from the hub SPR(s) (runtime `connect` was not enough).

## Risks

- No durability guarantee upstream; GC TTL 30 d by default (verify on a live node).
- Basecamp 0.3 host-owned config: the app can't set bootstrap/extip/TTL/quota → NAT-ed desktops may
  never advertise; hubs must be reachable via the host's default network config.
- NAT↔NAT transfers impossible (relay caps) → effective centralisation on a few hubs.
- Our libp2p patches (getProviders, IPv6 dial) not upstream; Android client is a private fork.
- Pre-alpha protocol churn (discv5→Kademlia broke interop once; storage 2.x→3.0 API break).
- Throughput of a single hub for 200 MB downloads by many users: untested (we proved 1–2 MB).

## Open questions (Logos Storage team)

1. Is a durability layer (erasure coding / proofs / marketplace or altruistic repair) on the 2026–27
   roadmap, or is "pinning" the long-term model?
2. Will `updateExpiry`/a pin API (no-expiry blocks for uploads) be exposed in the FFI and module?
3. How should a Basecamp 0.3 app influence the host's Storage config (bootstrap, TTL, quota)?
4. Upstreaming getProviders-local-records and IPv6 dial-bind fixes; status of #1221 (mobile).
5. Byte-range reads in the FFI? Recommended max dataset size / block size for 100+ MB files?
6. Is the public `logos.test` Storage network stable enough to rely on, or should apps run their own DHT root?
