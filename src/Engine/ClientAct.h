/*
 * Constellation — the one door through which an action may touch the world.
 *
 * Contract: homelab/.agent/design/constellation-engine/engine-spec-v1.md §6, as amended by v2
 * §6′ and v3. Three Codex passes, the last one BUILD THE FIRST CUT AS WRITTEN.
 *
 * INVARIANT 0, MADE STRUCTURAL. Until now "companions act only through client-equivalent
 * opcodes" was a rule people follow. Here it is a type: an Action receives a ClientAct& and has
 * no other way to affect the world. Reading core state stays free — a real client learns the
 * same facts through SMSG — but WRITING has exactly one door, and Player::TeleportTo, AddQuest,
 * RewardQuest and KilledMonsterCredit are not on it and will not be added. If a behaviour cannot
 * be expressed here, that is a finding to report, not a reason to widen the door.
 *
 * Every method names the opcode it sends. The bodies are MOVED from Constellation.cpp, not
 * invented: each field name below was read at the call site it came from, because writing a
 * packet's fields from memory is how a build cycle gets wasted.
 *
 * The audit `F:\core\constellation\audit_writes.py` is the gate: it enumerates every non-read
 * call on a world object, and anything outside ClientAct.cpp is a finding. Its measured baseline
 * today is 41 opcode dispatches, 44 CMSG packets and exactly three internal writes —
 * SetFacingToObject twice, DurabilityRepairAll once, and CombatStop once, the last being the
 * only one in the whole module with no client packet at all.
 *
 * Copyright (C) 2026 Constellation contributors. Licensed under the GNU AGPL v3 — see COPYING.
 */
#ifndef CONSTELLATION_ENGINE_CLIENTACT_H
#define CONSTELLATION_ENGINE_CLIENTACT_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

class Player;
class WorldSession;

namespace Constellation::Ai
{
    class ClientAct
    {
    public:
        ClientAct(Player* self, WorldSession* session) : _self(self), _session(session) { }

        // NO ACCESSOR FOR THE PLAYER, AND THE REASON IS SHARP.
        //
        // This class first exposed `Player const* Me()`, on the assumption that const-correctness
        // carried the rule. The review showed it does not: `Player.h:2214` declares
        // `WorldSession* GetSession() const { return m_session; }` — a const method handing back
        // a MUTABLE session. So `Me()` was a straight route from an action to every packet
        // handler in the core, and the door this class exists to be was standing open.
        //
        // Identity and state reach an action through Ctx's read-only views instead (§6′).
        bool Usable() const { return _self && _session; }

        // -- talking -----------------------------------------------------------------------
        bool QuestGiverHello(ObjectGuid giver);                     // CMSG_QUEST_GIVER_HELLO
        bool GossipSelect(ObjectGuid unit, uint32 menuId, uint32 optionId); // CMSG_GOSSIP_SELECT_OPTION

        // -- quests ------------------------------------------------------------------------
        bool AcceptQuest(ObjectGuid giver, uint32 questId);         // CMSG_QUEST_GIVER_ACCEPT_QUEST
        // An empty ender means "hand in to myself", and THAT is what makes it a script turn-in.
        // The call site couples the two — `FromScript = (ender == nullptr)` — so the coupling is
        // derived here rather than offered as a parameter a caller could contradict.
        bool CompleteQuest(ObjectGuid ender, uint32 questId);       // CMSG_QUEST_GIVER_COMPLETE_QUEST

        // -- combat ------------------------------------------------------------------------
        bool SetSelection(ObjectGuid target);                       // CMSG_SET_SELECTION
        bool AttackSwing(ObjectGuid victim);                        // CMSG_ATTACK_SWING
        bool AttackStop();                                          // CMSG_ATTACK_STOP

        // -- the world ---------------------------------------------------------------------
        bool UseGameObject(ObjectGuid go);                          // CMSG_GAME_OBJ_USE
        bool EnterAreaTrigger(int32 areaTriggerId);                 // CMSG_AREA_TRIGGER

        // -- trade -------------------------------------------------------------------------
        bool ListInventory(ObjectGuid vendor);                      // CMSG_LIST_INVENTORY
        bool SellItem(ObjectGuid vendor, ObjectGuid item, uint32 amount);   // CMSG_SELL_ITEM
        bool RepairAll(ObjectGuid npc);                             // CMSG_REPAIR_ITEM

        // -- travel. §7 and the operator's ruling of 2026-09-07: «ходим ногами», flights are
        //    ordinary work, and there is NO teleport here because a client cannot send one.
        bool ActivateTaxi(ObjectGuid master, uint32 node);          // CMSG_ACTIVATE_TAXI
        bool EnableTaxiNode(ObjectGuid master);                     // CMSG_ENABLE_TAXI_NODE
        // Found by reading BindAtInn (Constellation.cpp:9359) for the innkeeper trigger: the
        // sixteenth method, and the hearthstone is worthless without it.
        bool BinderActivate(ObjectGuid innkeeper);                  // CMSG_BINDER_ACTIVATE

        // -- movement ----------------------------------------------------------------------
        //
        // This was first left out with the excuse that "the module moves through the motion
        // master, not a packet". The review checked and that was wrong: `SendMove`
        // (Constellation.cpp:11598) builds a MovementInfo and dispatches CMSG_MOVE_HEARTBEAT
        // through the real handler, exactly as a client does. The excuse was mine.
        //
        // Then it was added as `Move(Position, flags)` and the review caught the worse mistake:
        // an arbitrary position is a TELEPORT. Every one of the module's six senders supplies a
        // position within one step of the current one — `go = std::min(step, legLen)`
        // (Constellation.cpp:1554) — and the operator's ruling of 2026-09-07 is «ходим ногами».
        // A door that accepts any position hands back the very thing the door exists to forbid.
        //
        // So it is a STEP, and the bound is enforced here rather than trusted to callers.
        // A refused step is a defect to find, like a swept reservation in §10⁗, not a
        // condition to tolerate — it means an action tried to move somewhere it could not walk.
        bool Step(Position const& next, uint32 movementFlags);      // CMSG_MOVE_HEARTBEAT

        // How far this companion could walk in one slice, by the speed of the movement it is
        // actually doing. `MOVE_RUN` alone was wrong: a swimming companion is capped by
        // `MOVE_SWIM` and a flying one by `MOVE_FLIGHT`, and using the run speed for all three
        // both over- and under-permits depending on the state.
        float MaxStepYards() const;

        // §11 and the review: bounding ONE step is not bounding movement. A hundred legal steps
        // in a single tick is a teleport spelled slowly. The engine calls this once per tick and
        // the budget is what a companion could actually have covered in it.
        void ResetTick();

        // NOT YET HERE, and deliberately listed so the gap is visible rather than discovered:
        //   UseItem     — CMSG_USE_ITEM needs the cast id built at the call site; moved when
        //                 the hearthstone action is written.
        //   ChooseReward— CMSG_QUEST_GIVER_CHOOSE_REWARD carries a LootItemType and an index
        //                 chosen by the gear rules; moved with the turn-in action.
        //   Loot*       — four opcodes that always travel together; moved as one group.

    private:
        Player*       _self;
        WorldSession* _session;
        float         _stepBudgetYards = 0.0f;   // spent by Step, refilled by ResetTick
        bool          _tickOpen        = false;  // no movement before the engine opens the tick
    };
}

#endif // CONSTELLATION_ENGINE_CLIENTACT_H
