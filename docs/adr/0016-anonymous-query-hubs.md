# 0016. Optional anonymous query hubs, kept honest by redundancy, audits and declared exclusions

- Status: Accepted (design; not built yet)
- Date: 2026-10-05

## Context

Snapshots (0015) give private prefix search up to one epoch old. Fuzzy text, the newest models
and, later, search by shape need a node holding everything that answers queries. That node could
forge results, bury models, censor, or learn what people search for.

## Decision

**A query hub** is the same open core running headless with every category subscribed. Anyone
can run one.

**Queries:**
- signed by a throwaway key;
- RLN-limited;
- sent on a hub's query topic, with replies on a one-off reply topic;
- clients ask **2-3 hubs** and merge the answers.

**Answers** are the creators' **signed events** (or index entries naming the signed event's id
and hash), never free-form claims.

**What each threat runs into:**

| threat | counter |
|---|---|
| forge or alter | impossible: clients verify every signature |
| re-rank | clients rank locally from the signed data |
| stale data | answers name the epoch and catalogue root they come from |
| withhold files | files are content-addressed and fetchable from any holder |
| omit (snapshots) | provable, see below |
| omit (live queries) | redundancy + audits, see below |
| learn queries | prefix shards over Mix (0015) for the private path; hub queries are optional and reveal the text, not the sender |

**Omission from snapshots is provable.**
- Indexes are deterministic, so independent indexers' roots must match; a mismatch exposes an omission.
- **Creators audit their own inclusion:** a creator's node fetches the shards for its own terms
  each epoch.
- A signed shard for an epoch after a model's anchored publication that lacks the model is a
  **publicly checkable proof of omission**. Clients drop that indexer automatically, and the
  proof can be shared.

**Live query hubs are audited.**
- Clients hold their subscribed categories in full, so they occasionally send hubs queries whose
  answers they already know.
- Hubs that drop results lose local score and get dropped. Scores can be shared as signed reports.

**Moderation is allowed; silent censorship isn't.**
- A hub may refuse content (illegal material, not its focus), like MuleNet's per-hub content policies.
- It must **publish a signed exclusion list** of what it drops and why.
- Declared exclusion is a policy users choose hubs by. Undeclared omission is provable cheating.

**Hub lists** come from several sources: built-in, user-added, learned from peers. Never one
hardcoded hub.

## Rejected

- **A single official search service:** one point of censorship and surveillance.
- **Trusting hub reputation alone, with no proofs:** can't tell a censor from an outage.

## Consequences

- If every hub colludes, global search can be censored. Discovery still works through category and
  creator topics and direct links, and the fix is to start an honest hub, which should be one command.
- Free-text hub queries reveal the query text, not the sender, and only if the user opts in. The
  default search path is 0015.
- Hubs need: deterministic index builds, shard signing, the exclusion list, a query endpoint. Clients
  need: inclusion checks, audits, hub scoring, and merging answers from several hubs.
- **To verify:** whether Delivery has a mix route for messages. Without one, the hub learns the
  sender's network peer, though not their Swamp identity.
