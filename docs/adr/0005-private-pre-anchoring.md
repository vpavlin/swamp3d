# 0005. Creators may anchor private commitments of drafts

- Status: Proposed
- Date: 2026-10-05

## Context

Front-running: a thief who anchors first wins on priority. A real creator usually has drafts; a thief has only the final mesh.

## Decision

A creator's app can anchor H(salt || fileHash || fp || authorKey) for drafts without revealing them, and reveal later as a draft chain in a dispute. Binding the author key makes a stolen reveal useless.

## Rejected

Public drafts (leaks unreleased work).

## Consequences

Optional and off by default; costs nothing when epoch-batched.
