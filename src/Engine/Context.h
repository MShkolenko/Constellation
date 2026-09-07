/*
 * Constellation — what an action is allowed to know, and how it is handed the world.
 *
 * Contract: homelab/.agent/design/constellation-engine/engine-spec-v2.md §6′ ("Ctx does NOT hand
 * an action a mutable Player* or Companion& — otherwise the type proves nothing").
 *
 * WHY A READ FACADE AND NOT A `Player const*`. The obvious design is to pass a const pointer and
 * let const-correctness carry the rule. It does not: `Player.h:2214` declares
 * `WorldSession* GetSession() const` — a const method returning a MUTABLE session — so a const
 * pointer is a straight route to every packet handler in the core. That is not a hypothetical;
 * it is how the first version of ClientAct was still wide open while claiming to be a door.
 *
 * So reads get a facade too. WorldView holds the Player privately and answers questions; it never
 * hands the object out. The set of questions grows on demand, and that is deliberate — each one
 * added is a decision someone made, not a capability that leaked in.
 *
 * Copyright (C) 2026 Constellation contributors. Licensed under the GNU AGPL v3 — see COPYING.
 */
#ifndef CONSTELLATION_ENGINE_CONTEXT_H
#define CONSTELLATION_ENGINE_CONTEXT_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "QuestDef.h"
#include <optional>

class Player;

namespace Constellation::Ai
{
    class ClientAct;

    // Everything an action may ask about the world, and nothing else.
    class WorldView
    {
    public:
        explicit WorldView(Player const* self) : _self(self) { }

        // -- identity ----------------------------------------------------------------------
        ObjectGuid  Guid() const;
        char const* Name() const;
        uint8       Level() const;
        uint8       Class() const;
        uint8       Race() const;

        // -- where ------------------------------------------------------------------------
        Position Where() const;
        uint32   MapId() const;
        uint32   ZoneId() const;
        uint32   AreaId() const;
        float    DistanceTo(Position const& pos) const;
        float    DistanceTo2d(Position const& pos) const;
        // NOT a float: "not found" is not "right here". An unresolvable guid used to come back
        // as 0.0f, which satisfies every range and arrival check a caller could write.
        std::optional<float> DistanceTo(ObjectGuid guid) const;
        bool     CanSee(ObjectGuid guid) const;

        // -- condition ---------------------------------------------------------------------
        bool  IsAlive() const;
        bool  IsInCombat() const;
        bool  IsMounted() const;
        bool  IsFlying() const;
        bool  IsInWater() const;
        float HealthPct() const;
        bool  HasAttackers() const;

        // -- the quest log -----------------------------------------------------------------
        QuestStatus StatusOf(uint32 questId) const;
        bool        IsRewarded(uint32 questId) const;
        uint8       FreeQuestSlots() const;
        // TWO DIFFERENT QUESTIONS, and conflating them is a defect. Eligibility is what a
        // giver's menu is built from; permission additionally needs log space and bag space.
        bool        IsEligibleFor(uint32 questId) const;   // CanTakeQuest — would it be offered
        bool        MayAccept(uint32 questId) const;       // + SatisfyQuestLog + CanAddQuest

        // -- the bags ----------------------------------------------------------------------
        uint32 FreeBagSlots() const;

        // NOT HERE, and not by omission:
        //   Player const* / Player& — see the header comment; this is the whole point.
        //   Anything that scans the world (nearest hostile, nearest giver, gather points) —
        //     those are Values with intervals, because a per-tick grid sweep is what once took
        //     the world thread to 99 % of a core (Constellation.cpp:2786).

    private:
        Player const* _self;
    };

    // §6′ — what an action receives. One timestamp for the whole tick so two values cannot
    // disagree about "now"; one read facade; one write door; nothing else.
    struct Ctx
    {
        WorldView const& World;
        ClientAct&       Act;
        uint32           NowMs;
    };
}

#endif // CONSTELLATION_ENGINE_CONTEXT_H
