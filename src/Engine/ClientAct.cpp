/*
 * Constellation — the door's implementation. Every body here was MOVED from the call site named
 * in its comment, field for field, because a packet's fields written from memory cost a build
 * cycle to discover.
 *
 * Contract: engine-spec-v1.md §6 as amended by v2 §6′ and v3.
 *
 * Copyright (C) 2026 Constellation contributors. Licensed under the GNU AGPL v3 — see COPYING.
 */
#include "ClientAct.h"

#include "AreaTriggerPackets.h"
#include "CombatPackets.h"
#include "GameTime.h"
#include "Log.h"
#include "MovementPackets.h"
#include "GameObjectPackets.h"
#include "ItemPackets.h"
#include "LootPackets.h"
#include "MiscPackets.h"
#include "NPCPackets.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestPackets.h"
#include "TaxiPackets.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace Constellation::Ai
{
    // ---- talking ---------------------------------------------------------------------------

    bool ClientAct::QuestGiverHello(ObjectGuid giver)               // from Constellation.cpp:7198
    {
        if (!Usable() || giver.IsEmpty())
            return false;
        WorldPacket raw(CMSG_QUEST_GIVER_HELLO);
        WorldPackets::Quest::QuestGiverHello hello(std::move(raw));
        hello.QuestGiverGUID = giver;
        _session->HandleQuestgiverHelloOpcode(hello);
        return true;
    }

    bool ClientAct::GossipHello(ObjectGuid unit)                    // from Constellation.cpp:7439
    {
        if (!Usable() || unit.IsEmpty())
            return false;
        WorldPacket raw(CMSG_TALK_TO_GOSSIP);
        WorldPackets::NPC::Hello hello(std::move(raw));
        hello.Unit = unit;
        _session->HandleGossipHelloOpcode(hello);
        return true;
    }

    bool ClientAct::GossipSelect(ObjectGuid unit, uint32 menuId, uint32 optionId)  // :5113
    {
        if (!Usable() || unit.IsEmpty())
            return false;
        WorldPacket raw(CMSG_GOSSIP_SELECT_OPTION);
        WorldPackets::NPC::GossipSelectOption sel(std::move(raw));
        sel.GossipUnit     = unit;
        sel.GossipID       = menuId;
        sel.GossipOptionID = optionId;
        _session->HandleGossipSelectOptionOpcode(sel);
        return true;
    }

    // ---- quests ----------------------------------------------------------------------------

    bool ClientAct::AcceptQuest(ObjectGuid giver, uint32 questId)   // :7345
    {
        if (!Usable() || giver.IsEmpty() || !questId)
            return false;
        WorldPacket raw(CMSG_QUEST_GIVER_ACCEPT_QUEST);
        WorldPackets::Quest::QuestGiverAcceptQuest accept(std::move(raw));
        accept.QuestGiverGUID = giver;
        accept.QuestID        = questId;
        _session->HandleQuestgiverAcceptQuestOpcode(accept);
        return true;
    }

    bool ClientAct::CompleteQuest(ObjectGuid ender, uint32 questId)  // :7926
    {
        if (!Usable() || !questId)
            return false;
        WorldPacket raw(CMSG_QUEST_GIVER_COMPLETE_QUEST);
        WorldPackets::Quest::QuestGiverCompleteQuest done(std::move(raw));
        // The call site couples these two: `FromScript = (ender == nullptr)`. Taking the flag as
        // a parameter let a caller produce a combination the original never could — a real ender
        // AND FromScript true. Derived, so it cannot be contradicted.
        bool const toSelf   = ender.IsEmpty();
        done.QuestGiverGUID = toSelf ? _self->GetGUID() : ender;
        done.QuestID        = questId;
        done.FromScript     = toSelf;
        _session->HandleQuestgiverCompleteQuest(done);
        return true;
    }

    // ---- combat ----------------------------------------------------------------------------

    bool ClientAct::SetSelection(ObjectGuid target)                 // :7020
    {
        if (!Usable())
            return false;
        WorldPacket raw(CMSG_SET_SELECTION);
        WorldPackets::Misc::SetSelection sel(std::move(raw));
        sel.Selection = target;
        _session->HandleSetSelectionOpcode(sel);
        return true;
    }

    bool ClientAct::StopMoving()                                    // from StopMoving, :12246
    {
        if (!Usable())
            return false;
        // СОСТОЯНИЕ КОПИРУЕТСЯ ЦЕЛИКОМ, как и у поворота: обработчик замещает им всё, что было,
        // и собранный с нуля стёр бы транспорт, падение и тангаж.
        MovementInfo mi = _self->m_movementInfo;
        mi.guid = _self->GetGUID();
        mi.pos.Relocate(_self->GetPosition());
        mi.flags = 0;
        mi.time = GameTime::GetGameTimeMS();
        _session->HandleMovementOpcode(CMSG_MOVE_STOP, mi);
        return true;
    }

    bool ClientAct::Face(ObjectGuid target)                         // from FaceTarget, :12176
    {
        if (!Usable() || target.IsEmpty())
            return false;
        Unit* who = ObjectAccessor::GetUnit(*_self, target);
        if (!who)
            return false;

        // ЗНАКОВЫЙ УГОЛ, А НЕ НОРМАЛИЗОВАННЫЙ (правка Кодекса у лестницы, проход 9):
        // `NormalizeOrientation` превращает -0.01 в 6.27, и полградуса отклонения читались бы
        // как «повёрнут неверно» — с одной стороны цели пакеты без конца, с другой ни одного.
        float const ang  = _self->GetAbsoluteAngle(who);
        float diff = ang - _self->GetOrientation();
        while (diff >  float(M_PI)) diff -= 2.0f * float(M_PI);
        while (diff < -float(M_PI)) diff += 2.0f * float(M_PI);
        if (std::fabs(diff) < 0.05f)
            return true;                // уже смотрим куда надо — успех, и пакетами не сорим

        // СОСТОЯНИЕ ДВИЖЕНИЯ КОПИРУЕТСЯ ЦЕЛИКОМ: обработчик замещает им всё, что было.
        MovementInfo mi = _self->m_movementInfo;
        mi.guid = _self->GetGUID();
        Position pos = _self->GetPosition();
        pos.SetOrientation(ang);
        mi.pos.Relocate(pos);
        mi.time = GameTime::GetGameTimeMS();
        _session->HandleMovementOpcode(CMSG_MOVE_SET_FACING, mi);
        return true;
    }

    bool ClientAct::AttackSwing(ObjectGuid victim)                  // :7026
    {
        if (!Usable() || victim.IsEmpty())
            return false;
        WorldPacket raw(CMSG_ATTACK_SWING);
        WorldPackets::Combat::AttackSwing swing(std::move(raw));
        swing.Victim = victim;
        _session->HandleAttackSwingOpcode(swing);
        return true;
    }

    bool ClientAct::AttackStop()                                    // :6935
    {
        if (!Usable())
            return false;
        WorldPacket raw(CMSG_ATTACK_STOP);
        WorldPackets::Combat::AttackStop stop(std::move(raw));
        _session->HandleAttackStopOpcode(stop);
        return true;
    }

    // ---- loot ------------------------------------------------------------------------------
    //
    // Four opcodes for one protocol. The bodies are the ladder's, moved from the named lines of
    // LootFromCorpse/TakeOpenLoot; the ORDER and the policy (what to take, how much fits, count
    // what landed) stay in `LootFromCorpseCore`, which reaches these through a `LootSender`.

    bool ClientAct::LootUnit(ObjectGuid unit)                       // :6979
    {
        if (!Usable() || unit.IsEmpty())
            return false;
        WorldPacket raw(CMSG_LOOT_UNIT);
        WorldPackets::Loot::LootUnit open(std::move(raw));
        open.Unit = unit;
        _session->HandleLootOpcode(open);
        return true;
    }

    bool ClientAct::LootMoney()                                     // :6833
    {
        if (!Usable())
            return false;
        // No GUID: the handler takes from the whole open loot view at once, so it is sent once.
        WorldPacket raw(CMSG_LOOT_MONEY);
        WorldPackets::Loot::LootMoney money(std::move(raw));
        _session->HandleLootMoneyOpcode(money);
        return true;
    }

    bool ClientAct::LootItems(LootPick const* picks, uint32 count)  // :6845
    {
        // THE PACKET'S OWN CAP IS ENFORCED HERE, not trusted to the caller: `Array<LootRequest,
        // 100>` does not refuse an overflow gracefully, and a refused send is a counted defect.
        if (!Usable() || !picks || count == 0 || count > LOOT_PICK_CAP)
            return false;
        WorldPacket raw(CMSG_LOOT_ITEM);
        WorldPackets::Loot::LootItem take(std::move(raw));
        for (uint32 i = 0; i < count; ++i)
        {
            WorldPackets::Loot::LootRequest& req = take.Loot.emplace_back();
            req.Object     = picks[i].Object;       // the view's KEY is the loot object's GUID
            req.LootListID = picks[i].LootListId;   // NOT the index in the list
        }
        _session->HandleAutostoreLootItemOpcode(take);
        return true;
    }

    bool ClientAct::LootRelease(ObjectGuid unit)                    // :6939
    {
        if (!Usable() || unit.IsEmpty())
            return false;
        WorldPacket raw(CMSG_LOOT_RELEASE);
        WorldPackets::Loot::LootRelease done(std::move(raw));
        done.Unit = unit;
        _session->HandleLootReleaseOpcode(done);
        return true;
    }

    // ---- the world -------------------------------------------------------------------------

    bool ClientAct::UseGameObject(ObjectGuid go)                    // :4470
    {
        if (!Usable() || go.IsEmpty())
            return false;
        WorldPacket raw(CMSG_GAME_OBJ_USE);
        WorldPackets::GameObject::GameObjUse use(std::move(raw));
        use.Guid = go;
        _session->HandleGameObjectUseOpcode(use);
        return true;
    }

    bool ClientAct::EnterAreaTrigger(int32 areaTriggerId)           // :9046
    {
        if (!Usable())
            return false;
        WorldPacket raw(CMSG_AREA_TRIGGER);
        WorldPackets::AreaTrigger::AreaTrigger pkt(std::move(raw));
        pkt.AreaTriggerID = areaTriggerId;
        pkt.Entered       = true;
        pkt.FromClient    = true;
        _session->HandleAreaTriggerOpcode(pkt);
        return true;
    }

    // ---- trade -----------------------------------------------------------------------------

    bool ClientAct::ListInventory(ObjectGuid vendor)                // :3218
    {
        if (!Usable() || vendor.IsEmpty())
            return false;
        WorldPacket raw(CMSG_LIST_INVENTORY);
        WorldPackets::NPC::Hello list(std::move(raw));
        list.Unit = vendor;
        _session->HandleListInventoryOpcode(list);
        return true;
    }

    bool ClientAct::SellItem(ObjectGuid vendor, ObjectGuid item, uint32 amount)   // :6328
    {
        if (!Usable() || vendor.IsEmpty() || item.IsEmpty())
            return false;
        WorldPacket raw(CMSG_SELL_ITEM);
        WorldPackets::Item::SellItem sell(std::move(raw));
        sell.VendorGUID = vendor;
        sell.ItemGUID   = item;
        sell.Amount     = amount;
        _session->HandleSellItemOpcode(sell);
        return true;
    }

    bool ClientAct::RepairAll(ObjectGuid npc)                       // :6388
    {
        if (!Usable() || npc.IsEmpty())
            return false;
        WorldPacket raw(CMSG_REPAIR_ITEM);
        WorldPackets::Item::RepairItem fix(std::move(raw));
        fix.NpcGUID      = npc;
        fix.ItemGUID     = ObjectGuid::Empty;   // empty = "repair everything", as at the call site
        fix.UseGuildBank = false;
        _session->HandleRepairItemOpcode(fix);
        return true;
    }

    // ---- movement --------------------------------------------------------------------------

    // A generous slice. The module ticks movement at 4 Hz, so a real step is around a yard and
    // a half; a whole second of run speed leaves room for a loaded world thread without leaving
    // room for a teleport.
    // ПРЕЖНЯЯ КОНСТАНТА СТАЛА ПОТОЛКОМ, А НЕ НОРМОЙ. Здесь стояла целая секунда бега при
    // такте в 250 мс, с обоснованием «запас на загруженный поток мира». От телепорта это
    // защищало, от учетверённой скорости — нет: семь ярдов бюджета против полутора ярдов
    // настоящего шага разрешают четыре шага за такт. Теперь бюджет от ИЗМЕРЕННОГО такта, а
    // секунда — верхняя граница: загруженный поток по-прежнему не отвергнет законный шаг.
    static constexpr float  STEP_SLICE_CAP_SECONDS = 1.0f;

    // Первый такт спутника мерить не с чем. Берём заявленную частоту модуля (4 Гц) — ровно
    // один раз за жизнь состояния, дальше только измерение.
    static constexpr uint32 STEP_NOMINAL_SLICE_MS  = 250;
    static constexpr float STEP_TOLERANCE_YARDS = 2.0f;

    float ClientAct::MaxStepYards() const
    {
        if (!_self)
            return 0.0f;
        // THE MEASURE FOLLOWS THE MOVEMENT, not the other way round. Asking MOVE_RUN while the
        // companion is swimming both over-permits (run is faster than swim) and, on a mount over
        // water, under-permits. Ask the core which speed actually governs right now.
        UnitMoveType const type = _self->IsFlying()   ? MOVE_FLIGHT
                                : _self->IsInWater()  ? MOVE_SWIM
                                                      : MOVE_RUN;
        return _self->GetSpeed(type) * _sliceSeconds + STEP_TOLERANCE_YARDS;
    }

    void ClientAct::ResetTick(uint32 sliceMs, uint32* refusedSink)
    {
        _refusedSink = refusedSink;
        // ИЗМЕРЕНИЕ, А НЕ КОНСТАНТА — И С ПОТОЛКОМ, ПОТОМУ ЧТО ИЗМЕРЕНИЕ ТОЖЕ ВРЁТ. Если поток
        // мира встал на десять секунд, честная разность выдала бы бюджет на семьдесят ярдов —
        // ровно тот телепорт, ради запрета которого дверь и существует.
        uint32 const slice   = sliceMs ? sliceMs : STEP_NOMINAL_SLICE_MS;
        float const  seconds = float(slice) / 1000.0f;
        _sliceSeconds    = seconds < STEP_SLICE_CAP_SECONDS ? seconds : STEP_SLICE_CAP_SECONDS;
        _stepBudgetYards = MaxStepYards();
        _tickOpen        = true;
    }

    bool ClientAct::Step(Position const& next, uint32 movementFlags)  // from SendMove, :11598
    {
        if (!Usable())
            return false;

        // NOTHING MOVES BEFORE THE TICK IS OPENED. Without this an action reached outside the
        // engine's tick — or after it — would move on a budget nobody refilled or accounted for.
        if (!_tickOpen)
        {
            TC_LOG_ERROR("server.worldserver",
                "Constellation ШАГ {}: отказ — такт не открыт", _self->GetName());
            if (_refusedSink) ++*_refusedSink;
            return false;
        }

        // THE BOUND IS THE POINT. Without it this method is a teleport wearing a movement
        // packet: the handler will accept whatever position it is given.
        float const allowed = MaxStepYards();
        float const moved   = _self->GetExactDist2d(next.GetPositionX(), next.GetPositionY());
        if (moved > allowed)
        {
            TC_LOG_ERROR("server.worldserver",
                "Constellation ШАГ {}: отказ — {:.1f} ярдов за такт при разрешённых {:.1f}."
                " Это попытка переместиться, а не пройти",
                _self->GetName(), moved, allowed);
            if (_refusedSink) ++*_refusedSink;
            return false;
        }

        // AND ONE BOUNDED STEP IS NOT A BOUND. A hundred legal steps in one tick covers the same
        // ground as one illegal one, so the budget is per tick and this is what spends it.
        if (moved > _stepBudgetYards)
        {
            TC_LOG_ERROR("server.worldserver",
                "Constellation ШАГ {}: отказ — за такт уже пройдено, осталось {:.1f} ярдов из"
                " {:.1f}, а просят {:.1f}",
                _self->GetName(), _stepBudgetYards, allowed, moved);
            if (_refusedSink) ++*_refusedSink;
            return false;
        }

        // Upward is climbing and is bounded the same way. Downward is falling, which a client
        // does for free — and two of the module's senders exist precisely to drop a companion
        // back onto the ground after it got stuck above it (Constellation.cpp:1259, :1509).
        float const climbed = next.GetPositionZ() - _self->GetPositionZ();
        if (climbed > allowed)
        {
            TC_LOG_ERROR("server.worldserver",
                "Constellation ШАГ {}: отказ — вверх на {:.1f} ярдов при разрешённых {:.1f}",
                _self->GetName(), climbed, allowed);
            if (_refusedSink) ++*_refusedSink;
            return false;
        }

        MovementInfo mi;
        mi.guid  = _self->GetGUID();
        mi.pos.Relocate(next);
        mi.flags = movementFlags;
        mi.time  = GameTime::GetGameTimeMS();
        _session->HandleMovementOpcode(CMSG_MOVE_HEARTBEAT, mi);
        _stepBudgetYards -= moved;
        return true;
    }

    // ---- travel ----------------------------------------------------------------------------

    bool ClientAct::ActivateTaxi(ObjectGuid master, uint32 node)    // :3387
    {
        if (!Usable() || master.IsEmpty())
            return false;
        WorldPacket raw(CMSG_ACTIVATE_TAXI);
        WorldPackets::Taxi::ActivateTaxi taxi(std::move(raw));
        taxi.Vendor = master;
        taxi.Node   = node;
        _session->HandleActivateTaxiOpcode(taxi);
        return true;
    }

    bool ClientAct::BinderActivate(ObjectGuid innkeeper)            // from BindAtInn, :9359
    {
        if (!Usable() || innkeeper.IsEmpty())
            return false;
        // Note the packet type: CMSG_BINDER_ACTIVATE travels as NPC::Hello, same as the vendor
        // list. Copied from the call site rather than guessed, like every other body here.
        WorldPacket raw(CMSG_BINDER_ACTIVATE);
        WorldPackets::NPC::Hello bind(std::move(raw));
        bind.Unit = innkeeper;
        _session->HandleBinderActivateOpcode(bind);
        return true;
    }

    bool ClientAct::EnableTaxiNode(ObjectGuid master)               // :9478
    {
        if (!Usable() || master.IsEmpty())
            return false;
        WorldPacket raw(CMSG_ENABLE_TAXI_NODE);
        WorldPackets::Taxi::EnableTaxiNode enable(std::move(raw));
        enable.Unit = master;
        _session->HandleEnableTaxiNodeOpcode(enable);
        return true;
    }
}
