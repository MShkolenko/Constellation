/*
 * Constellation — the read facade's implementation.
 *
 * Every method here is a question answered by the core, never a rule re-derived here. That is
 * this repo's own standing lesson — «ask the core for its own measures» — and the reason
 * CanTake() below calls Player::CanTakeQuest rather than checking level, class and prerequisites
 * itself, which is exactly how a companion once walked to the ender of a quest it was already
 * carrying.
 *
 * Contract: engine-spec-v2.md §6′.
 *
 * Copyright (C) 2026 Constellation contributors. Licensed under the GNU AGPL v3 — see COPYING.
 */
#include "Context.h"

#include "Bag.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"

namespace Constellation::Ai
{
    ObjectGuid  WorldView::Guid()  const { return _self ? _self->GetGUID() : ObjectGuid::Empty; }
    char const* WorldView::Name()  const { return _self ? _self->GetName().c_str() : "?"; }
    uint8       WorldView::Level() const { return _self ? _self->GetLevel() : uint8(0); }
    uint8       WorldView::Class() const { return _self ? _self->GetClass() : uint8(0); }
    uint8       WorldView::Race()  const { return _self ? _self->GetRace()  : uint8(0); }

    Position WorldView::Where() const { return _self ? _self->GetPosition() : Position(); }
    uint32   WorldView::MapId()  const { return _self ? _self->GetMapId()  : uint32(0); }
    uint32   WorldView::ZoneId() const { return _self ? _self->GetZoneId() : uint32(0); }
    uint32   WorldView::AreaId() const { return _self ? _self->GetAreaId() : uint32(0); }

    float WorldView::DistanceTo(Position const& pos) const
    {
        return _self ? _self->GetExactDist(&pos) : 0.0f;
    }

    float WorldView::DistanceTo2d(Position const& pos) const
    {
        return _self ? _self->GetExactDist2d(pos.GetPositionX(), pos.GetPositionY()) : 0.0f;
    }

    std::optional<float> WorldView::DistanceTo(ObjectGuid guid) const
    {
        if (!_self || guid.IsEmpty())
            return std::nullopt;
        // Resolved through the accessor rather than cached: a stale pointer across ticks is the
        // failure mode §10′ forbids outright — actions hold GUIDs, never raw pointers.
        //
        // AND "NOT FOUND" IS NOT "RIGHT HERE". This returned 0.0f when the object could not be
        // resolved, which is indistinguishable from standing on top of it — so every range and
        // arrival check would have been satisfied by a target that had just despawned. An
        // optional makes the caller say what it means.
        if (WorldObject const* who = ObjectAccessor::GetWorldObject(*_self, guid))
            return _self->GetExactDist(who);
        return std::nullopt;
    }

    bool WorldView::CanSee(ObjectGuid guid) const
    {
        if (!_self || guid.IsEmpty())
            return false;
        return ObjectAccessor::GetWorldObject(*_self, guid) != nullptr;
    }

    bool  WorldView::IsAlive()    const { return _self && _self->IsAlive(); }
    bool  WorldView::IsInCombat() const { return _self && _self->IsInCombat(); }
    bool  WorldView::IsMounted()  const { return _self && _self->IsMounted(); }
    bool  WorldView::IsFlying()   const { return _self && _self->IsFlying(); }
    bool  WorldView::IsInWater()  const { return _self && _self->IsInWater(); }

    float WorldView::HealthPct() const
    {
        return _self ? _self->GetHealthPct() : 0.0f;
    }

    bool WorldView::HasAttackers() const
    {
        return _self && !_self->getAttackers().empty();
    }

    QuestStatus WorldView::StatusOf(uint32 questId) const
    {
        return _self ? _self->GetQuestStatus(questId) : QUEST_STATUS_NONE;
    }

    bool WorldView::IsRewarded(uint32 questId) const
    {
        return _self && _self->IsQuestRewarded(questId);
    }

    uint8 WorldView::FreeQuestSlots() const
    {
        if (!_self)
            return 0;
        uint8 used = 0;
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (_self->GetQuestSlotQuestId(slot))
                ++used;
        return uint8(MAX_QUEST_LOG_SIZE - used);
    }

    bool WorldView::IsEligibleFor(uint32 questId) const
    {
        if (!_self)
            return false;
        // ASK THE CORE. CanTakeQuest folds in level, class, race, prerequisites and exclusivity
        // — and NOTHING ELSE. It is what PrepareQuestMenu uses, which is why it is the right
        // question for "would this giver offer it to me".
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        return quest && _self->CanTakeQuest(quest, false);
    }

    bool WorldView::MayAccept(uint32 questId) const
    {
        if (!_self)
            return false;
        // THREE QUESTIONS, NOT ONE. My first version called CanTakeQuest alone and the comment
        // claimed it covered log space. It does not, and the real accept handler proves it:
        // `WorldSession::HandleQuestgiverAcceptQuestOpcode` requires CanTakeQuest AND
        // CanAddQuest, with SatisfyQuestLog between them. The reference module agrees —
        // `QuestAction::AcceptQuest` reports "Can't take", "Quest log is full" and "Bags are
        // full" as three separate refusals.
        //
        // The distinction is not pedantry: an action that treats eligibility as permission will
        // walk a companion to a giver and then fail at the last step, forever, with a full log.
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        return quest
            && _self->CanTakeQuest(quest, false)
            && _self->SatisfyQuestLog(false)
            && _self->CanAddQuest(quest, false);
    }

    uint32 WorldView::FreeBagSlots() const
    {
        // COUNTED BY THE CORE, NOT BY ME. My first version walked the backpack from
        // INVENTORY_SLOT_ITEM_START to the fixed INVENTORY_SLOT_ITEM_END, which ignores the
        // player's actual `GetInventorySlotCount()` — so it over-counted for anyone whose
        // backpack is not the maximum size. `GetFreeInventorySlotCount` (Player.h:1414) already
        // honours it, and writing the loop again was rung 3 of the ladder skipped.
        return _self ? _self->GetFreeInventorySlotCount() : 0;
    }
}
