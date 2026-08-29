# Constellation — architecture

## What a companion is

A companion is a **real Player driven by the server**: a character row in the characters
database, logged in through a headless session (a WorldSession with no socket), moved and
acted by module code. It is not a creature dressed as a player.

Why this is the load-bearing decision:

- Everything the core already implements for players — spells, auras, items, movement,
  threat, loot, groups, professions — works for companions for free. A creature-based
  imitation re-implements all of it and still looks wrong (wrong packets, wrong nameplate,
  cannot join a group, cannot trade).
- The companion is visible to real clients exactly as another player, because to the core
  it *is* another player.
- Cost: one WorldSession + one Player per companion. Phase 0 of the old plan — measure
  before scaling — still applies; the measurement target is sessions-per-core-thread on
  the live host.

## Invariants (hold in every phase)

1. **Core stays clean.** The fork carries exactly one guarded hook in
   `Custom/custom_script_loader.cpp`. Everything else lives in this repository and enters
   the build through the `Custom/Constellation` symlink. If a phase ever genuinely needs a
   core change, that change is a separate, named fork commit — never silently mixed in.
2. **Disabled means inert.** With `Constellation.Enable = 0` the module must not touch
   the world: no sessions, no timers, no DB reads beyond config.
3. **Own tables only, own prefix.** Module state lives in tables named `constellation_*`
   in the characters database. The module never alters core tables' schema.
4. **Same shipping discipline as the core:** build and boot on the THRONE rig, DBErrors
   comparison against the live baseline, server-side probe on port 8095, only then swap.

## Phases

| Phase | Deliverable | Proof |
|---|---|---|
| 0 | Skeleton: registration, config, `.constellation status` | startup log line, command answers |
| 1 | Session fabrication: log one existing character in and out by command | character appears online, clean logout, no leaks after repeated cycles |
| 2 | Presence: the companion stands in the world, respawns on server restart | visible to a real client, survives restart |
| 3 | Follow: companion joins the summoner's group on command, follows, teleports when left behind | group invite accepted, MoveFollow holds through zones |
| 4 | Combat: assist mode — attacks the summoner's target with a class-appropriate rotation | target dies, companion survives, no stuck-in-combat |
| 5 | Population: N companions from config, distributed over capital cities | load measured against Phase-0 baseline |

Later (unordered): equipment management, vendor trips, quest execution, auction house.
The old client-side task list (0005–0017 in the homelab repo) is superseded by this plan;
its ideas return as phases here, implemented server-side.

## Phase 1 sketch (next)

- `constellation_characters` table: character GUID + role flags.
- `.constellation summon <name>` / `.constellation dismiss <name>` (GM-only at first).
- Session fabrication path: create WorldSession with null socket, feed it through
  `HandlePlayerLogin` the way the login handler does, drive updates from the module's
  world-update hook. Logout through the normal path, verified by session/player counters.
- The first measurement: memory and update-time delta for 1 vs 10 idle companions.
