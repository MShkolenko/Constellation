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
#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
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


    // -----------------------------------------------------------------------------------------
    // §12 — ОБХОДЫ. Частота не здесь: эти три зовутся только из `Value::Calculate`, а
    // тот — только когда истёк интервал или сменилась карта.
    // -----------------------------------------------------------------------------------------

    void WorldView::ForEachCompletedTurnIn(TurnInVisitor visit, void* user) const
    {
        if (!_self || !visit)
            return;

        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const qid = _self->GetQuestSlotQuestId(slot);
            if (!qid || _self->GetQuestStatus(qid) != QUEST_STATUS_COMPLETE)
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(qid);
            if (!quest || !_self->CanRewardQuest(quest, false))
                continue;

            TurnInCandidate cand;
            cand.QuestId = qid;

            // САМОСДАЧА — ПО ФЛАГУ ЯДРА, А НЕ ПО ОТСУТСТВИЮ ПРИНИМАЮЩЕГО.
            //
            // Первая версия модуля объявила самосдаваемым любой квест без принимающего-NPC.
            // Замер по базе: без принимающего и без флага — 19 868 квестов, настоящих
            // самосдаваемых — 4 733. Ошибка вчетверо, и не в безопасную сторону
            // (Constellation.cpp:7546-7561).
            if (quest->HasFlag(QUEST_FLAGS_AUTO_COMPLETE))
            {
                cand.EnderEntry = 0;
                cand.Where      = _self->GetPosition();
                cand.Dist       = 0.0f;
                cand.FromTable  = false;
                visit(user, cand);
                continue;
            }

            // БЛИЖАЙШИЙ ПРИНИМАЮЩИЙ, А НЕ ПЕРВЫЙ И НЕ ВСЕ.
            //
            // `FindTurnIn` возвращался на ПЕРВОМ принимающем в порядке связей ядра — то есть в
            // произвольном. Первая версия этого обхода отдавала ВСЕХ, и Кодекс был прав:
            // тогда потолок списка переставал быть обоснован размером журнала заданий, и
            // кандидаты могли МОЛЧА теряться. Цифры из базы: у 20 107 квестов принимающий
            // один, у 482 их два, у 61 — двенадцать, у одного — шестнадцать.
            //
            // Один кандидат на квест возвращает потолку его обоснование и заодно улучшает
            // поведение: ближайший лучше произвольного. Зернистость та же, что у остального
            // модуля: отсрочка `TurnInBackoff` ведётся по КВЕСТУ, а не по принимающему.
            bool  found    = false;
            float bestDist = 0.0f;
            for (auto const& pair : sObjectMgr->GetCreatureQuestInvolvedRelationReverseBounds(qid))
            {
                uint32 const enderEntry = pair.second;
                Position where;
                float    dist = 0.0f;
                if (!NearestSpawnOf(_self->GetMapId(), enderEntry, _self->GetPosition(), &where, &dist))
                    continue;               // призываемый или на другой карте — идти некуда
                if (found && dist >= bestDist)
                    continue;
                found           = true;
                bestDist        = dist;
                cand.EnderEntry = enderEntry;
                cand.Where      = where;
                cand.Dist       = dist;
                cand.FromTable  = true;
            }
            if (found)
                visit(user, cand);
        }
    }

    void WorldView::ForEachQuestGiverInRange(float range, GiverSightVisitor visit, void* user) const
    {
        if (!_self || !visit || range <= 0.0f)
            return;

        std::list<Creature*> around;
        Trinity::AnyUnitInObjectRangeCheck check(_self, range);
        Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(_self, around, check);
        Cell::VisitGridObjects(_self, searcher, range);

        // МАСКА, А НЕ «НЕ None». Это главная ошибка всей ветки, и она измерена:
        // `GetQuestDialogStatus` возвращает МАСКУ всего, что NPC значит для игрока сейчас —
        // включая `Reward` (у меня есть НЕЗАВЕРШЁННЫЙ квест, который он ПРИНИМАЕТ — серый
        // знак) и `Future` (квест есть, но уровнем не дорос). Проверка `!= None` принимала
        // всё это за «есть что взять»: 56 спутников из 122 шли к ПРИНИМАЮЩЕМУ их же текущего
        // квеста, а за день взят ОДИН квест на весь состав (Constellation.cpp:11547-11576).
        //
        // `Future` исключён СОЗНАТЕЛЬНО: спутник дорастёт и вернётся сам.
        QuestGiverStatus const offers =
              QuestGiverStatus::Quest              | QuestGiverStatus::Trivial
            | QuestGiverStatus::DailyQuest         | QuestGiverStatus::TrivialDailyQuest
            | QuestGiverStatus::RepeatableQuest    | QuestGiverStatus::TrivialRepeatableQuest
            | QuestGiverStatus::MetaQuest          | QuestGiverStatus::TrivialMetaQuest
            | QuestGiverStatus::JourneyQuest       | QuestGiverStatus::TrivialJourneyQuest
            | QuestGiverStatus::LegendaryQuest     | QuestGiverStatus::TrivialLegendaryQuest
            | QuestGiverStatus::ImportantQuest     | QuestGiverStatus::TrivialImportantQuest
            | QuestGiverStatus::CovenantCallingQuest;

        for (Creature* creature : around)
        {
            if (!creature->IsAlive())
                continue;
            // ФЛАГ КВЕСТОДАТЕЛЯ ПЕРЕД ЗНАКОМ, и это не перестраховка.
            //
            // `GetQuestDialogStatus` считает по СВЯЗЯМ существа с квестами и про флаг не спрашивает,
            // а без флага `CanInteractWithQuestGiver` откажет всегда. На живом мире это
            // держало спутника у курицы (620, `npcflag = 0`, скрипт `npc_chicken_cluck`) весь
            // сеанс. Существ со связью и без флага — 248 против 7962 с флагом.
            //
            // Указатель карты модуля эту проверку делал всегда; обзор — нет. Здесь они сравнялись.
            if (!creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
                continue;
            if ((_self->GetQuestDialogStatus(creature) & offers) == QuestGiverStatus::None)
                continue;

            GiverInSight g;
            g.Guid    = creature->GetGUID();
            g.Entry   = creature->GetEntry();
            g.Dist    = _self->GetExactDist2d(creature);
            // ВИДИМОСТЬ — СВЕДЕНИЕ, А НЕ ФИЛЬТР, и это оплачено: у гнома Ноббина
            // единственный предлагающий стоял в восьми ярдах ЗА СТЕНКОЙ мастерской — «не
            // видно 1, выбран никто», и так у семи гномов. Дорога строится по сетке, а не
            // по лучу (Constellation.cpp:11624-11628). Предпочтение видимым оказывает действие.
            g.Visible = _self->IsWithinLOSInMap(creature);
            visit(user, g);
        }
    }

    bool WorldView::CanTalkTo(ObjectGuid unit) const
    {
        if (!_self || unit.IsEmpty())
            return false;
        Creature* creature = ObjectAccessor::GetCreature(*_self, unit);
        return creature && creature->IsAlive() && _self->CanInteractWithQuestGiver(creature);
    }

    bool WorldView::InMeleeRange(ObjectGuid unit) const
    {
        if (!_self || unit.IsEmpty())
            return false;
        Unit* who = ObjectAccessor::GetUnit(*_self, unit);
        return who && who->IsAlive() && _self->IsWithinMeleeRange(who);
    }

    std::optional<Position> WorldView::WhereIs(ObjectGuid unit) const
    {
        if (!_self || unit.IsEmpty())
            return std::nullopt;
        Unit* who = ObjectAccessor::GetUnit(*_self, unit);
        if (!who)
            return std::nullopt;
        return who->GetPosition();
    }

    bool WorldView::StepFor(MoveState& m, Position const& to, float stopAt, float dt,
                            MoveSendFn send, void* user) const
    {
        if (!_self || !send)
            return false;
        return StepAlong(m, _self, send, user, to, stopAt, dt);
    }

    void WorldView::Objectives(FightMemory const& mem, DangerView const& danger,
                               ObjectiveScan* out) const
    {
        if (!_self || !out)
            return;
        ScanObjectivesFor(_self, mem, danger, out);
    }

    bool WorldView::GiverToWalkTo(SeekMemory const& mem, SeekTarget* out) const
    {
        if (!_self || !out)
            return false;
        return FindGiverToWalkTo(_self, mem, out);
    }

    uint32 WorldView::BestQuestOffered(QuestRefusedFn refused, void const* user) const
    {
        return _self ? BestQuestInMenu(_self, refused, user) : 0u;
    }

    std::optional<ObjectGuid> WorldView::NearestQuestGiverOfEntry(uint32 entry, float searchDist) const
    {
        if (!_self || !entry || searchDist <= 0.0f)
            return std::nullopt;

        std::list<Creature*> around;
        Trinity::AnyUnitInObjectRangeCheck check(_self, searchDist);
        Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(_self, around, check);
        Cell::VisitGridObjects(_self, searcher, searchDist);

        Creature* best = nullptr;
        float bestDist = 0.0f;
        for (Creature* creature : around)
        {
            if (creature->GetEntry() != entry || !creature->IsAlive())
                continue;

            // ЯДРО РЕШАЕТ, ДОСТАТОЧНО ЛИ БЛИЗКО, И ЭТО НЕ ВЕЖЛИВОСТЬ. Оно меряет в
            // пространстве и знает свой INTERACTION_DISTANCE; своя мерка вместо его уже
            // дала 916 кругов и ноль сдач (Constellation.cpp:3984-3998). Заодно снимаются
            // условия, которых мы бы и не вспомнили: смерть, полёт, бой, дружественность.
            if (!_self->CanInteractWithQuestGiver(creature))
                continue;

            // В ПРОСТРАНСТВЕ, НЕ ПО ПЛОСКОСТИ. Тот же случай: помост девятью ярдами выше
            // ближе всех по плоскости и дальше всех на самом деле.
            float const d = _self->GetExactDist(creature);
            if (!best || d < bestDist)
                { bestDist = d; best = creature; }
        }
        return best ? std::optional<ObjectGuid>(best->GetGUID()) : std::nullopt;
    }

    void WorldView::ForEachGiverOnMap(float maxDist, GiverIndexVisitor visit, void* user) const
    {
        if (!_self)
            return;
        VisitGiverIndex(_self->GetMapId(), _self->GetPosition(), maxDist, visit, user);
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
