/*
 * Constellation — the inventory of cached values, and the buffers they fill.
 *
 * Contract: homelab/.agent/design/constellation-engine/values-tiers.md (second edition, after an
 * adversarial Codex pass on the design BEFORE anything was built), plan v2 amendments 9 and 10.
 *
 * WHAT A VALUE IS. Something expensive to compute and foolish to compute every tick. The module
 * proved it needs them the hard way: `c.VendScanMs = 5000` exists because a 100-yard grid sweep
 * per tick at four ticks a second and 122 companions took the world thread to 99 % of a core.
 * That throttle was hand-rolled at ONE site; this is the same idea, declared once and applied
 * everywhere.
 *
 * ONE TIER, AND THAT IS A DECISION. Plan v2 amendment 10 asked for a second tier keyed by map,
 * living on `Manager`. It is not built, for two reasons that were read out of the code rather
 * than argued:
 *
 *   1. The map facts ALREADY exist, computed once at load — `Manager::_givers`
 *      (Constellation.cpp:7786-7797), `_menders` (:7760), `_flightMasters` (:7777), `_spawns`
 *      (:7701). A value tier recomputing them would be a second tool for a job that has one.
 *   2. What the plan called `GiversByMap` is not a map fact at all: `FindGiverByMap` (:10372)
 *      sorts by distance FROM THE PLAYER (:10388), filters by HIS faction (:10378) and HIS
 *      per-spawn backoff (:10397). Three personal filters out of four.
 *
 * So every value here is per companion, its slot lives in `EngineState`, and `Discard` therefore
 * covers it with no third lifecycle policy. Decision recorded in homelab decisions.md, 2026-09-08.
 *
 * The payload types are NOT declared here. They live in `Context.h`, because they are what the
 * world hands back from a sweep; declaring them here as well would be the same struct written
 * twice, once as "what the world returns" and once as "what the buffer holds".
 *
 * Copyright (C) 2026 Constellation contributors. Licensed under the GNU AGPL v3 — see COPYING.
 */
#ifndef CONSTELLATION_ENGINE_VALUES_H
#define CONSTELLATION_ENGINE_VALUES_H

#include "Primitives.h"     // pulls Context.h, where the payloads and the visitors live
#include "QuestDef.h"       // MAX_QUEST_LOG_SIZE — the core's own bound, not a chosen one
#include <vector>

namespace Constellation::Ai
{
    // -------------------------------------------------------------------------------------
    // A collection that reserves once, refills in place and REPORTS what it had to drop.
    //
    // This is `BidSink`'s rule applied to values, and for the same reason: on a host that has
    // been OOM-killed seven times, a silent reallocation inside a per-tick path is worse than a
    // lost element. `Clear()` keeps the capacity, so a recompute allocates nothing at all.
    //
    // An overflow is a DEFECT to find, not a cost to absorb: the cap is chosen from a measured
    // number (see the table below), so hitting it means the number was wrong.
    // -------------------------------------------------------------------------------------
    template <class T, size_t Cap>
    class CappedList
    {
    public:
        CappedList() { _items.reserve(Cap); }

        void Clear() { _items.clear(); }        // capacity survives; that is the whole point

        bool Add(T const& v)
        {
            if (_items.size() >= Cap)
            {
                ++_dropped;
                return false;
            }
            _items.push_back(v);
            return true;
        }

        bool     Empty()   const { return _items.empty(); }
        size_t   Size()    const { return _items.size(); }
        uint32   Dropped() const { return _dropped; }
        T const& operator[](size_t i) const { return _items[i]; }

        typename std::vector<T>::const_iterator begin() const { return _items.begin(); }
        typename std::vector<T>::const_iterator end()   const { return _items.end(); }

        static constexpr size_t Capacity = Cap;

    private:
        std::vector<T> _items;
        uint32         _dropped = 0;    // never reset: a defect that happened once still happened
    };

    // Caps, and where each number comes from. They are named here so they can be argued with.
    //
    //   turn-ins   — the quest log cannot hold more, so this cap can never bind. It exists to
    //                give the container a reservation, not as a policy. Asked of the core
    //                (MAX_QUEST_LOG_SIZE) rather than chosen, which is the standing rule.
    //   in sight   — the live log's own «проверено N» lines top out at 24 within
    //                QuestGiverRange; 32 leaves headroom without doubling the buffer.
    //   by index   — only the nearest is ever walked to; sixteen is enough that per-spawn
    //                backoff cannot empty the list.
    using TurnInList     = CappedList<TurnInCandidate, MAX_QUEST_LOG_SIZE>;
    using GiverSightList = CappedList<GiverInSight, 32>;
    using GiverIndexList = CappedList<GiverOnMap, 16>;

    // -------------------------------------------------------------------------------------
    // The per-companion slots, generated from the SAME list as the enum and the name table, so
    // a declared value without storage is impossible rather than merely unlikely.
    // -------------------------------------------------------------------------------------
    struct ValueSlots
    {
#define CONSTELLATION_VALUE_SLOT(name, text, type) Slot<type> name;
        CONSTELLATION_VALUES(CONSTELLATION_VALUE_SLOT)
#undef CONSTELLATION_VALUE_SLOT

        // §10′ — Reset gives up the work, so every cached answer about that work is stale.
        void InvalidateAll()
        {
#define CONSTELLATION_VALUE_INVALIDATE(name, text, type) name.Invalidate();
            CONSTELLATION_VALUES(CONSTELLATION_VALUE_INVALIDATE)
#undef CONSTELLATION_VALUE_INVALIDATE
        }

        // A cap that binds is a defect, and a defect nobody prints is a defect nobody fixes.
        uint32 DroppedTotal() const
        {
            uint32 n = 0;
#define CONSTELLATION_VALUE_DROPPED(name, text, type) n += name.Buffer.Dropped();
            CONSTELLATION_VALUES(CONSTELLATION_VALUE_DROPPED)
#undef CONSTELLATION_VALUE_DROPPED
            return n;
        }
    };

    // -------------------------------------------------------------------------------------
    // id -> type, and id -> slot. This is what makes `Engine::Val<ValueId::X>()` type-safe:
    // the caller names an id, and the buffer type it gets back is decided by the same line of
    // the same list that declared the id. A provider registered under the wrong id is a
    // COMPILE error, not a bad cast at runtime.
    // -------------------------------------------------------------------------------------
    template <ValueId Id> struct ValueTraits;

#define CONSTELLATION_VALUE_TRAIT(name, text, type)                                   \
    template <> struct ValueTraits<ValueId::name>                                     \
    {                                                                                 \
        using Type = type;                                                            \
        static Slot<type>& SlotOf(ValueSlots& s) { return s.name; }                   \
    };
    CONSTELLATION_VALUES(CONSTELLATION_VALUE_TRAIT)
#undef CONSTELLATION_VALUE_TRAIT

    // Registration of the three quest values, in one place so the inventory of what exists is
    // readable without grepping. Called once at load beside the action registrations.
    class Engine;
    void RegisterQuestValues(Engine& engine);
}

#endif // CONSTELLATION_ENGINE_VALUES_H
