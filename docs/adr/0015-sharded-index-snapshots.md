# 0015. Global search uses sharded, signed index snapshots in Logos Storage

- Status: Accepted. Built in 0.3.0 (indexer, term + record shards, signed manifests, private shard fetch, global search, open-from-index); LEZ anchoring of the roots comes with roadmap item 5
- Date: 2026-10-05

## Context

With 0014, nobody holds the whole catalogue, but people still need to search all of it. A query
must not reveal who is searching for what, must work without trusting one server, and should
cost a few downloads, not a sync.

## Decision

- **Indexers** (hubs, or anyone with the full catalogue) build an index every epoch.
  - **Index entry** (~150 B): modelId, latest v, title, tags, categories, creator address and
    name, licence, counts, thumbnail CID, and the id + hash of the signed version event it came from.
  - **Inverted index:** normalised terms (title words, tags, creator name) → model ids.
- **Shards:**
  - the index is split by term prefix (for example, all terms starting with "bra"), plus one shard
    per category;
  - each shard is a deterministic, sorted file, so the same catalogue always gives the same bytes,
    and it is uploaded to Storage;
  - a **manifest** lists the epoch, the catalogue root and every shard's CID and hash, and is
    signed by the indexer.
- **Anchoring:** the manifest root and the catalogue root are anchored on LEZ (0004), so a
  snapshot can't be backdated and every indexer commits to one specific catalogue.
- **A client searching for "bracelet":**
  1. fetches the newest manifest it trusts;
  2. fetches only the shard(s) for its terms, over Mix (`isPrivate`), so a Storage provider learns
     at most a term prefix and never who asked;
  3. intersects and ranks locally;
  4. fetches the records of the results it opens, and verifies their signatures.
- **Offline:** shards are cached. Repeat searches and recent results work offline.
- **Freshness:** snapshot results are up to one epoch old. Events newer than the snapshot come from
  the live topics (0014) or a query hub (0016).
- **Snapshots double as catch-up:** a new node in a category fetches that category's shard
  instead of replaying its history (loam-sync ADR 0020, the snapshot lever in logos-rln-budget).

## Rejected

- **One big index file:** every search downloads everything.
- **Server-side search only:** the server sees every query and can censor silently (see 0016).
- **Bloom-filter or private-information-retrieval schemes:** too heavy for M2. Revisit if prefix
  leakage turns out to matter.

## Consequences

- Search granularity is whole words and prefixes, re-ranked locally. Fuzzy and long-tail search
  goes to 0016.
- Because shards are deterministic and signed, **omission is provable** (0016 builds on this).
- Indexers spend storage and a few RLN messages per epoch (announcing the manifest). Clients
  spend one manifest fetch plus one or two shard fetches per new term.
- **Verified 2026-10-05, negative:** Storage's private (Mix) fetch did not work on logos.test (every
  Mix lookup proxy failed; a failed private request also poisoned the plain retry). Private fetch is
  therefore opt-in (`SWAMP_PRIVATE_FETCH=1`), and the UI says searches aren't private until it works.
