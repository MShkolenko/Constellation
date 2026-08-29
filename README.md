# Constellation

Server-side companion module for AlgalonCore (11.2.7). Constellation populates the realm
with server-driven companions — "living constellations" — implemented entirely on the
server: no client automation, no injected addons, no third-party code.

This is an independent, from-scratch implementation. It links against the core's script
API and nothing else.

## Layout

```
src/    module sources (picked up by the core build as a Custom script subtree)
conf/   constellation.conf.dist — documented configuration keys
docs/   ARCHITECTURE.md — design, phases, invariants
tools/  integrate.sh — wires the module into a core checkout
```

## Integration (one symlink + one guarded hook)

The core build already compiles every source under `src/server/scripts/Custom/`.
Integration is:

1. `src/server/scripts/Custom/Constellation` → symlink to this repo's `src/`.
2. The fork carries a single 6-line commit in `Custom/custom_script_loader.cpp`:
   the hook is wrapped in `#if __has_include("Constellation/Registration.h")`,
   so the core builds identically whether or not the module is checked out.

`tools/integrate.sh <core-src-dir>` performs step 1 and verifies step 2.

## Status

Phase 0 — skeleton: registration, config surface, `.constellation status` command.
See `docs/ARCHITECTURE.md` for the phase plan.
