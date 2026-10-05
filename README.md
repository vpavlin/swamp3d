# Research: a decentralized place to share 3D models (working name: Spool)

> Status: **research**, started 2026-10-05. Nothing is built yet. Not endorsed by or affiliated with
> Logos; built on the Logos tech stack.

A Printables / MakerWorld / Thingiverse-style library for 3D models and maker projects that no
company runs: a Basecamp desktop app (plus Android later), files in Logos Storage, the catalogue,
comments and makes as signed events over Logos Messaging, and LEZ for payments and for anchoring
when each version was published.

Core properties: **privacy, pseudonymity, censorship resistance**. Hard requirement from day one:
**stolen copies must be flaggable** with evidence anyone can check, without a central moderator
and without deleting anything.

## Reading order

1. [notes/01-landscape.md](notes/01-landscape.md) - the centralized sites: what a model page holds,
   fees, licences, remixes, moderation, theft, lock-in; prior decentralized attempts; MUST/SHOULD/LATER.
2. [notes/02-storage.md](notes/02-storage.md) - Logos Storage today: API, durability (none
   guaranteed), the 30-day block TTL, pinning hubs, encrypted paid files, large files.
3. [notes/03-lez-chain.md](notes/03-lez-chain.md) - LEZ and Bedrock: payments, proving a payment,
   pay-to-unlock (fair exchange), anchoring options, identity binding, maturity.
4. [notes/04-basecamp-ui-preview.md](notes/04-basecamp-ui-preview.md) - can Basecamp show a 3D
   model? (No Qt Quick 3D; WebEngine is bundled.)
5. notes/05a - 3D fingerprints, provenance and decentralized flagging (prior art).
6. [RESEARCH.md](RESEARCH.md) - the synthesis: architecture, what goes where, open questions.
7. [docs/THEFT.md](docs/THEFT.md) - the design for flagging stolen models.
8. [docs/adr/](docs/adr/) - proposed decisions.

## Name candidates

Spool (filament spool; also a print queue) · Plinth (what a model stands on) · Filament Commons.
