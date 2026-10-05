# 0011. Public files are identified by the plain SHA-256 of their bytes

- Status: Accepted
- Date: 2026-10-05

## Context

The charter says hash sealed bytes so a store can't learn content equality. For a public library, content equality is a feature: identical files dedup, and an exact re-upload of someone else's file is instantly visible.

## Decision

files[].sha256 and images[].sha256 are the plain hashes. Uploads are staged under the name <sha256>, so any mirror re-uploading the same bytes produces the same CID. Downloads are verified against the hash.

## Rejected

Hashing sealed bytes (needs a key everyone has anyway).

## Consequences

Paid/private models (later) will use sealed blobs and the charter's rule again.
