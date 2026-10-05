# 0006. Files live in Logos Storage, kept alive by pinning hubs

- Status: Proposed
- Date: 2026-10-05

## Context

Logos Storage has no durability guarantee, deletes blocks after 30 days by default, and can't move big files between two NAT-ed peers (notes/02).

## Decision

Files and previews are content-addressed blobs (two-id rule, canonical upload name). Reachable pinning hubs with a long block TTL cache every CID they see and honour pin requests. Every model shows 'seeded by N'. Creators' desktops are opportunistic seeders.

## Rejected

Waiting for a Storage marketplace (doesn't exist yet); HTTP mirrors (centralised, but may be an emergency fallback).

## Consequences

A few hubs carry most of the load at first; we say so. Questions for the Storage team are in notes/02.
