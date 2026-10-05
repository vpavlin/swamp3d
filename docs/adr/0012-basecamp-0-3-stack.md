# 0012. Built on the Basecamp 0.3 stack from day one

- Status: Accepted
- Date: 2026-10-05

## Context

vpavlin chose Basecamp 0.3. The 0.3 runtime changed startup rules, Storage ownership and the transport stack (logos-basecamp-0.3-port).

## Decision

swamp_core and the swamp view build on logos-module-builder 0.3.1, depend on loam_core 0.5.x (port/0.3, upstream delivery 0.3.0) and the host-owned storage_module 3.x. Startup calls to other modules are deferred ~1 s; no default arguments; 256x256 icons; the view lists every module it calls. Headless tests use logosctl.

## Rejected

Start on 0.2 and port later (double work).

## Consequences

Testing needs the 0.3 packages; the 0.3 test repo on the crib isn't reachable from every box, so we can build loam_core port/0.3 ourselves.
