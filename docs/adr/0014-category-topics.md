# 0014. Subscribe to categories; nobody mirrors the whole catalogue

- Status: Accepted. Built in 0.2.0 (category topics, subscriptions, per-topic catch-up, lazy pictures); per-model record fetch comes with 0015. Supersedes the "every node holds everything" part of 0009.
- Date: 2026-10-05

## Context

Today every node syncs every catalogue event (0009) and eagerly fetches every picture. That is about
3 KB per model plus ~0.5 KB per like or comment, measured on the test catalogue. 10k models is
30-50 MB and 100k is 300-500 MB.

The wire cost is worse. Each channel message carries ~19-25 KB of Delivery overhead. Under RLN a
node gets ~100 messages per 10 minutes, shared by every app. So a full catch-up of a large
catalogue is impossible with RLN on. And search needs only a ~150 B index entry per model, not
the full history.

## Decision

- **A fixed, versioned list of ~30 categories**, Printables-style, ships with the app. Each model
  version declares one primary category and at most one secondary. Its events go to
  `/swamp/1/cat/<category>/proto`.
- **Tags stay free-form and are never topics.** Anyone could mint a tag, so tag topics would be
  unbounded and spammable. Tags are searchable through the index (0015).
- **A node subscribes to the categories its user picks:**
  - it mirrors them completely: index, records, the live tail;
  - catch-up (RBSR) runs per category topic;
  - browsing and search inside subscribed categories is local, offline and private.
- **Optional creator topics** (`/swamp/1/creator/<address>`) to follow people.
- **Records are fetched on demand.** Opening a model outside your categories fetches that model's
  events: from a peer or hub on its category topic, or from the snapshot (0015). Its comments,
  likes and makes come with it.
- **Pictures are lazy.** A thumbnail is fetched only when its card is shown. A model file is
  fetched only on download.
- **Hubs subscribe to every category.** Receiving costs no RLN; publishing snapshots and answering
  queries (0016) is what they spend it on.

## Rejected

- **Keep the full mirror:** it doesn't scale and dies under RLN.
- **Topics per tag:** unbounded and spammable.
- **Entries stored on LEZ:** no full-text search, and chain cost per entry. LEZ only anchors
  roots (0004).

## Consequences

- Global search outside your categories needs 0015 (snapshots) or 0016 (query hubs).
- The category list is a governance point: adding a category means an app update. "Other" catches the rest.
- Subscriptions reveal interest at category granularity. A light (Edge) client tells its service
  node which topics it wants; full relay nodes leak less.
- **To verify:** Delivery maps content topics onto a fixed number of shards, and a full relay node
  receives its whole shard. So category topics certainly cut disk, catch-up and RLN cost, but cut
  raw bandwidth only if categories land on different shards, or for Edge clients. Check the shard
  mapping on logos.test before promising bandwidth savings.
- Migration: the M1 single topic stays readable. Models without a category file under "Other".
