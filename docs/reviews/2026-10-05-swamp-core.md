# swamp_core code review - 2026-10-05

Scope: `swamp_core/src/swamp_core_impl.{h,cpp}`, `swamp_catalog.hpp`, `swamp_fp.hpp`, `swamp_thumb.hpp`.
`logos_sync/` was reviewed only for how swamp uses it. Checked against docs/SPEC.md and ADRs 0009-0013.
The existing tests pass (`swamp_core/test/run-tests.sh`: 18 + 20). Every finding marked **verified**
was reproduced with a throwaway program under `/tmp/swamp-review/` (t1 = fold/forgery/crash,
t5 = catch-up crash, t6 = clock skew, e2e_squat = CID squatting over the fake SDK, t3/t4 = timings).

## Status after the fix pass (same day)

Tests: `run-tests.sh` → catalog 18/18, fp parity PASS (JS and C++ both sanitize; golden vectors
unchanged), e2e 30/30 (new checks for C2, H1, M5, M7, M8 and the bogus-CID path). Live two-node
test on the Logos test fleet: PASS (`hub/two-node.sh`, now with the Storage SPR bootstrap).

| # | Status | What changed |
|---|---|---|
| C1 | fixed | `admissible()` requires `e.dev == e.hlc.dev`; envelope field limits (M4, part) |
| C2 | fixed | type-safe getters everywhere untrusted JSON is read; `eventFrom()` no-throw parse; catch-up frames validated before `respond()`; `onFrame`/`tick` wrapped |
| C3 | fixed | download cut off when the part file grows past the signed size; streaming hash (no whole-file read); global (6), preview (2) and hub (3) concurrency caps counted over in-flight fetches |
| H1 | fixed | the owner's CIDs first, one CID per other announcer, ≤ 8; CIDs announced in batches; a refused CID is re-announced at most every 10 min |
| H2 | fixed | two-pass fold (references applied after all models); clock advances on every admitted event, capped at now + 5 min |
| H3 | fixed | signatures checked once at ingest/load, `fold(log, admitted=true)` skips them; received events refold once per tick (dirty flag) and save on a 5 s throttle; dedup before verify; load + fold moved out of `onContextReady` |
| H4 | fixed | served events batched (≤ 48 KB frames), ≤ 200/min; one answer per peer's opening fp per 20 s; catch-up frames older than 2 min (store replays) ignored. Not done: an addressee field |
| M1 | fixed | `sanitize()` drops non-finite or > 1e7 coordinates; bins clamped; same in the rasteriser and the JS reference |
| M2 | fixed | per-blob fetch state: candidate rotation, back-off between rounds (15 s doubling, ≤ 30 min), immediate refusal moves on at once, stale attempts ignored; job fails after 3 rounds with the reason, shows "Retrying …" before; asking again resets |
| M3 | fixed | uploads retried every 60 s until a CID exists (refused, failed, timed out, or never started), for every blob I'm responsible for incl. make photos |
| M4 | partly | envelope limits done; per-author caps and retention not done |
| M5 | fixed | loam callbacks posted to the module loop; status polled until Connected |
| M6 | fixed | copy errors accumulate; duplicate file names refused by `validateVersion` |
| M7 | fixed | the version is validated before `model.create` is authored |
| M8 | fixed | `identity.json` chmod 0600 |
| M9 | open | publish still hashes/fingerprints on the IPC call |
| L1 | fixed | callbacks hold a shared lifetime flag |
| L2 | fixed | write methods use type-safe getters, and refuse until the stored log has loaded |
| L3, L6 | open | |
| L4, L5 | fixed | fp validated; ASCII-only lower-casing |

Found by the live test, fixed in the same pass:
- **Synchronous Storage calls on IPC methods.** `publish` called `uploadUrl` directly; with Storage
  busy in a ~30 s manifest wait, the call outlived the IPC and CLI timeouts. Now every Storage call
  is an `…AsyncResult` from the loop, one at a time (`storageFree()`), and `publish`/`postMake` only
  queue (tick uploads). Announcements wait ≤ 10 s for the rest of a publish's uploads, so they share an event.
- **Done event before the session id.** Storage 3.x uses the CID as the download session id; the
  done event can beat the AsyncResult. Matched by CID too.
- **Stalled transfers.** A holder whose DHT announcement timed out (`Timed out providing cid` in
  the holder's log) leaves a download at 0 bytes indefinitely. No growth for 2 min → `downloadCancel`
  and the next candidate. (Root cause is in Storage; re-`setAdvertise` didn't unstick it.)
- **Store replays.** The fleet store re-delivers old `fp`/`ids`/`need` frames on every connect; a fresh node
  answered all of them (156 events served for a 15-event log). Now ignored when > 2 min old.

Open after the live test: the first fetch of a FRESH upload takes 95-135 s on logos.test (3 runs).
B's log shows `failed to get manifest` twice, then success; A's shows `Timed out providing cid`. The
time is DHT provider propagation in Storage, not swamp's queueing (user downloads now pre-empt
previews and the hub sweep, and the first back-off is 15 s). A hub that prefetches on first sight
hides this for everyone after it.

Severity: **Critical** = a remote peer can take over or crash the network. **High** = a remote peer
can deny service, or correct data is lost in normal use. **Medium** = a real bug with a narrower
trigger. **Low** = hardening or a mismatch with the spec.

---

## Critical

### C1. Anyone can sign as anyone: the fold trusts `e.dev`, but the signature covers `hlc.dev` (verified)
`swamp_catalog.hpp:221-232`; `logos_sync/signing.hpp:111-114, 229-235`.
`verifyEvent` and `canonicalMessage` use `hlc.dev` when it is set and **never check that `e.dev` matches it**.
`e.dev` isn't signed. The fold uses `who = e.dev` for every author rule.
- Scenario A (hijack): Mallory signs a `model.retract` or a `model.version` `{v: latest+1, files: <evil sha>}`
  with her own key (`hlc.dev` = Mallory), then sets `"dev": <Alice>`. `admissible()` returns true, the fold
  sees `who == creator`, and Alice's model is retracted or gets a new version. t1 output:
  `retracted=1 reason=pwned versions=2 v2title=EVIL`. She can also create models and profiles "as" Alice.
- Scenario B (censorship and permanent divergence): Mallory re-sends Alice's *real* `model.create` or
  `model.version` with only `dev` changed. The signature still verifies and the `id` is the same. Nodes that
  get this copy first keep it (dedup by id) and reject the event (`modelIdFor(Mallory, nonce) != id` /
  `creator != who`). Catch-up compares ids only, so those nodes never heal.
- Fix: in `admissible()` require `!e.hlc.dev.empty() && e.dev == e.hlc.dev`, or have the fold use `e.hlc.dev`.
  Better still, enforce this in `logos_sync::verifyEvent` for every app.

### C2. A remote peer can crash any node with one malformed frame (verified)
nlohmann's `.value(key, default)` throws `type_error` when the key holds a different type. `const operator[]`
on a missing key is an assert, or undefined behaviour under NDEBUG. None of the receive path is inside a try/catch.
- `swamp_catalog.hpp:238, 243, 261, 265, 282` (`p.value("modelId", "")`), `:181-185` (title, description,
  summary, licence), `:173-175` (sha256, name), `:194` (parents.modelId).
  A signed `comment.post {"modelId": 7}` passes `admissible()`, is pushed into `m_log`, and then `refold()` throws.
- `logos_sync/event.hpp:65-79` `eventFromJson`: `{"t":"ev","e":"x"}` or `"v":"x"` throws (`swamp_core_impl.cpp:256`).
- `logos_sync/catchup.hpp:130-131, 159, 167`: `{"t":"ids"}` with no `ids`, or `{"t":"fp","fps":["a","b"]}`
  with no `bounds` → assertion abort (or undefined behaviour). `"ids":[1]` and `"bounds":5` → type_error.
- `swamp_core_impl.cpp:254` `f.value("t", "")` with `"t": 1`.
- Hub only: `hubSweep` `:516-518` reads `v["fp"].value("sha256", "")` / `value("size", 0LL)`. `fp` is never
  validated (`validateVersion` ignores it), so a creator's `"fp":{"sha256":5}` throws inside `tick()` on
  every hub, every tick.
- Outcome: e2e_squat injects the comment over the fake bus → `terminate called after throwing ...
  type_error.302` from `onFrame → refold → fold`. If the real runtime catches exceptions at the callback
  boundary, the poison event stays in `m_log` instead. Every later `refold()` (each received event, every
  `author()`) then throws, the catalogue freezes, and `catchup::respond` serves the event on to peers.
- Fix: wrap `onFrame` and `tick()` in try/catch. Replace `.value()` on untrusted JSON with type-checked
  getters (`clip`-style helpers for strings and ints). Validate `fp` in `validateVersion`. Make `respond()`
  check `is_array()` / `is_string()` before indexing.

### C3. Any peer can fill every client's disk and memory (by reasoning; the fake storage can't show it)
`fetchPreviews` `swamp_core_impl.cpp:489-502` (runs on every desktop, not only hubs), `startFetch :365-394`,
`finishFetched :410-412`, `hubSweep :505-524`.
- The size limit (`im.size <= 2 MiB`) uses the size the **event claims**. The download itself has no limit.
  Mallory publishes a model whose thumb says `size: 1000` and announces a `blob.cids` for that sha pointing at
  a multi-GB dataset. Every client that browses eagerly downloads it all into `parts/`. Then `finishFetched`
  reads the whole file into a `std::string` (`readFile` goes through a stringstream, so about 2x the size)
  to hash it → bad_alloc / OOM kills the module. On a hub the same happens for every file of every model,
  and `validBlobRef` allows any size.
- `fetchPreviews` starts up to 4 new fetches per tick but doesn't count those already running. With many
  models (or bogus CIDs, each held for the 10-minute stale timeout) the number of concurrent downloads
  grows without limit.
- Fix: read the manifest first (`datasetSize`) and refuse it unless it equals the declared size and is under
  a cap. Hash while streaming. Cap the bytes and concurrency of preview and hub fetches. Count fetches in flight.

## High

### H1. CID squatting makes any file permanently undownloadable, and the creator then sends a `blob.cids` event every tick (verified)
`swamp_catalog.hpp:251-259` (first 4 by HLC; HLC is chosen by the sender), `swamp_core_impl.cpp:531-536`, `:354-361`.
- `blob.cids` is open to anyone. The fold keeps the **first 4 by HLC**, and the attacker picks the HLC.
  Mallory announces 4 junk CIDs with `wall = 1` for a sha she has seen. Because the whole fold is recomputed,
  they push out the creator's real CID on every node, even after the fact.
- e2e_squat: Bob's download stays `"status":"fetching"` forever, with `cids=4`, all junk. Each junk CID then
  costs a failed attempt, and when all are used up `startFetch` returns early for good (`:372`). No error is
  ever shown.
- Side effect on the creator: `tick()` sees its own CID missing from `m_cat.cids` and calls `announceCids`.
  That authors a new `blob.cids`, which the fold drops again (already 4), so the next tick repeats it.
  Measured: **alice's log grew from 14 to 60 events and tx from 11 to 58 in 3 s** (60 ms tick). At the
  default 2 s tick that is about 1800 events an hour, each persisted, broadcast and charged to the RLN budget
  of the shared node. This also happens without an attacker, as soon as 4 honest mirrors announce different CIDs.
- The spec doesn't match itself here: ADR 0010 says "only the creator's later events count for ... CIDs",
  but SPEC section 3 says anyone may announce.
- Fix: always keep the creator's CIDs (and perhaps the hubs'). Order other candidates by arrival or by a cap
  per author, not by an HLC the sender controls. Rotate candidates: retry, shuffle, drop ones that fail.
  In `tick()`, never re-announce a CID the fold has already refused (remember what was announced).

### H2. Comments, likes and makes from a peer whose clock is behind are silently dropped everywhere (verified)
`swamp_core_impl.cpp:157, 199` call `m_id.clock.receive()` only for **our own** events. `logos_sync/event.hpp:84-87`
says to call it for every event ingested.
- Scenario: Bob's wall clock is 60 s behind Alice's. He comments on her new model. His comment's HLC sorts
  before her `model.create`. The fold finds no model yet and rejects it (`swamp_catalog.hpp:267`). t6:
  `comments=0 likes=0 rejected=2`. The `comment()` call still returns `ok`, and the comment is missing on
  every node, Bob's included.
- Fix: `clock.receive(e.hlc)` for every admitted event. Cap the jump to about now + 5 min, otherwise one
  future-dated event drags every clock forward. Or keep events that refer to a model not seen yet (park them
  until it arrives) instead of rejecting them for good.

### H3. A fold per received event plus a full log rewrite per event: catch-up costs O(N²) (measured)
`swamp_core_impl.cpp:256` (`ingest` → `refold` → `saveLog` for each `ev` frame), `:194`,
`swamp_catalog.hpp:225-229` (`mergeEvents` copies the log; `admissible` runs an ECDSA verify per event, every time).
- Measured: `fold()` of 5000 events takes **2.4 s** (about 0.5 ms per verify). A new peer catching up on
  5000 events refolds 5000 times. That is roughly 5000 × 1.2 s average ≈ 1.7 hours of CPU, all while
  holding `m_mtx`, so every IPC method (`snapshot`, `listModels`, ...) blocks past the 20 s IPC timeout.
  `saveLog` also writes the whole JSON log on every event (O(N²) bytes).
- `onContextReady` runs the same full fold synchronously (`:559`): 10k events ≈ 5 s inside the startup hook.
- Fix: cache verification by event id. Fold incrementally, or debounce `refold` and `saveLog` (for example
  once per tick, or with `onLoop` coalescing). Move the startup fold out of `onContextReady`. Check
  `m_logIds` **before** `admissible()` so duplicates don't pay for a verify.

### H4. One small frame makes every peer re-broadcast its whole log (verified)
`swamp_core_impl.cpp:257-260` with `catchup::respond`.
- `{"t":"ids","ids":[]}` (no lo/hi) → `respond` serves **every** event (t1: `serve 4 of 4`). A
  `need` listing all ids does the same. Every peer on the topic answers every `fp`/`ids`/`need` (there is no
  addressee), and each served event goes out as its own frame. One 30-byte frame turns into N × |log| sends,
  which drains the RLN budget the node shares with all apps (100 messages per 10 minutes).
- Fix: rate-limit replies per `from`, cap `serve` per message, batch served events into one frame, and add
  an addressee so only one peer, or a random subset, answers.

## Medium

### M1. A crafted ASCII STL corrupts memory at publish (verified, segfault)
`swamp_fp.hpp:160, 166, 209-212` (`(int)std::floor(NaN)` → INT_MIN → `F.d2[INT_MIN]`, `hist[tok<0]`), plus the same
pattern in the rasteriser `swamp_thumb.hpp:91-92`.
- `vertex 1e300 0 0 ...`: the area overflows to inf, so `total > 0` passes, and the distances become inf/NaN.
  The cast to bin is undefined behaviour and the array write goes out of bounds. `./t1 fp` → SIGSEGV; with
  `_GLIBCXX_ASSERTIONS` it fails `__n < this->size()`. The `try {} catch (...)` in `publish` doesn't help.
  The trigger is a user publishing (or remixing) a file downloaded from somewhere else.
- Fix: refuse non-finite or huge coordinates in `parseStl`, and check `std::isfinite` before every bin cast.

### M2. Downloads can get stuck with no visible error
`swamp_core_impl.cpp:365-426, 449-457, 460-486`.
- `m_fetchGaveUp` is never reset. Once every candidate has failed once (for example while offline, 10 minutes
  each), `startFetch` returns early for good until restart. The job stays `"fetching"`: `advanceJobs` never
  sets `"failed"` or an error. The hub's "retry for 30 minutes" in the spec isn't implemented.
- If the `downloadToUrlAsyncResult` callback never fires, `m_fetching` holds the sha forever (the stale sweep
  only looks at `m_downSessions`). Wrapping start times around `m_fetching` would cover this.
- The `"?"+sha` fallback (`:384`) is also used when storage clearly refused the request (`ar.ok()` but
  `!r.success`, for example an invalid CID). That turns an immediate failure into a 10-minute wait per candidate.
- `finishFetched :412`: if `readFile` fails, it returns without moving to the next candidate, so the same
  CID is fetched again every tick. `:422` ignores the `rename` error but still counts `m_fetched++`.
- `pollStorage :453` treats `file_size == declared size` as done. If storage preallocates or is still
  writing, the hash check fails and a good CID is blacklisted (risk; depends on storage behaviour).

### M3. Upload sessions are dropped without a retry, and some uploads are never retried at all
`swamp_core_impl.cpp:329-352, 446-447, 543-552, 807`.
- Sessions older than 10 minutes are erased, but a large upload can take longer. Its later
  `storageUploadDone` is then ignored and the manifest poll stops (no sessions left), so the CID is never
  announced until restart.
- Rejected uploads (`!r.success`) and failed ones (`success:false`) are never retried.
- Photos added through `postMake` before storage starts (`if (m_storageStarted)`) are never uploaded.
  Restart doesn't re-upload them either: `startModules` only walks models we created.
- The spec's `uploads.json` isn't implemented.
- Each finished upload authors its own `blob.cids` event. A 64-file version is about 80 events, most of the
  100 per 10 minutes RLN budget. Batch them per tick.

### M4. Event envelope fields have no size limit
`swamp_catalog.hpp:216-223`. Only `payload` is limited to 16 KiB. `id`, `type`, `dev`, `pub`, `sig` and `hlc` are
not. A signed event with a 1 MiB `id` is admissible (t1). That id is then stored, persisted, and copied into
every `ids` catch-up frame. Also, the log, comments, likes and `m_cat.cids` have no cap per author, and
everything is kept forever.

### M5. Transport callbacks do work on the delivery thread and call loam_core from inside its own callbacks
`swamp_core_impl.cpp:278-292`.
- `onReceived` runs fold, disk writes and `sendSealedAsync` on loam_core's callback thread.
- `onStatusChanged` calls `joinAsync` and `catchupRound` inside loam_core's own status callback. This is
  the documented trap in which calling a module from inside its own event callback blocks on the IPC timeout
  and drops the next event. Post both through `onLoop`, as the storage callbacks already do.
- `m_ready` only flips on the exact string `"Connected"`, and the current status is never fetched. If the
  shared loam_core is already connected (another app started it) and doesn't emit a status change again,
  swamp never joins the topic and never sends (unverified risk; compare loam_core 0.4.17 "start() twice").

### M6. `advanceJobs` can report "done" when files failed to copy
`swamp_core_impl.cpp:478-481`. `fs::copy_file(..., ec)` clears `ec` on success, so only the last file's
result counts (and the `create_directories` error is overwritten too). A failed earlier copy (disk full,
permissions) still reports `done`. Two files with the same `name` in one version silently overwrite each other.

### M7. `publish` sends `model.create` before validating the version
`swamp_core_impl.cpp:728-740`. A draft without a licence or title (or one over 16 KiB) still authors and
broadcasts a `model.create`, then returns an error. Each retry adds another orphan model to the permanent log.

### M8. The private key is world-readable
`identity.json` is written mode 0644 and the data dir 0755 (`writeFile` uses the default umask). Create it with 0600 and the dir with 0700.

### M9. Heavy work runs on the IPC call while holding the mutex
`publish` reads, hashes, fingerprints and renders on the call thread with `m_mtx` held (measured 339 ms for a
35 MB, 138k-triangle STL, before hashing and two refolds). It scales linearly and copies the file twice in
memory. Received frames block in the meantime. That is fine for M1, but large files will hit the 20 s IPC timeout.

## Low
- L1. Destructor `:104`: the module callbacks (`onReceived`, `onStatusChanged`, storage events, AsyncResult
  lambdas) capture `this` and are never unregistered. A callback after the module is torn down is a
  use-after-free. Pending `singleShot`s on `m_timer` are cancelled correctly.
- L2. Arguments from the local caller can throw: `listModels` `q.value("limit", 100)` / `"mine"`, and
  `publish`/`setProfile` `d.value("title", "")` when the value has the wrong type. The exception escapes the
  IPC method instead of returning `{"ok":false}`.
- L3. HLC `wall` is whatever the author claims. `published` dates and the "newest" sort can be backdated or
  pinned to the top with a future time. It can't serve as priority evidence for theft labels (that is ADR 0004's job).
- L4. `validateVersion` doesn't validate `fp`, `images[].kind/mime` or `files[].kind`. The fold copies any
  extra payload keys into the version JSON that is served to the view.
- L5. `matches()` uses `::tolower` on `char`, which is undefined behaviour for non-ASCII bytes. Cast to `unsigned char`.
- L6. Mismatches with the spec: only `.stl` is fingerprinted (the spec says "first model file", which
  includes 3mf/obj); the spec says `downloads.json`/`uploads.json` but the code uses `jobs.json`/`mycids.json`
  and has no uploads file; ADR 0010 and SPEC disagree on who may announce CIDs (see H1).

## Checked and OK
- **Paths from untrusted input:** every sha used in a path (`blobPath`, `parts/<sha>`) comes from a
  fold-validated `files`/`images` entry (`isHex(...,64)`, which rejects upper case) or is checked against
  `m_cat.cids` keys, which are also hex-validated. `hubSweep`'s unvalidated `fp.sha256` is gated by
  `m_cat.cids.count(sha)`. Download file names pass `safeName` (no `/`, `\`, control characters, `.` or `..`).
  The download dir is `safeDirName(title)` plus a hex id. I found no traversal.
- **Hash verification:** downloaded bytes are hashed before the rename into `files/`, and failures are counted and deleted.
- **Basecamp 0.3 rules:** all public methods return JSON strings, take at most 2 args, have no default
  arguments, and have no trailing comments on the declaration lines in the header. Module calls are deferred
  1 s out of `onContextReady`. Storage, AsyncResult and the timer are posted to the module thread through
  `onLoop`. The exceptions are the loam_core callbacks (M5) and the synchronous fold in `onContextReady` (H3).
