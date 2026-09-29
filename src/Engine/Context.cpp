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
#include "Group.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>

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

    bool WorldView::NeedsRest() const
    {
        return _self && NeedsRestFor(_self);
    }

    bool WorldView::RestedEnough() const
    {
        return _self && RestedEnoughFor(_self);
    }

    bool WorldView::HasAttackers() const
    {
        return _self && !_self->getAttackers().empty();
    }

    bool WorldView::IsInFlight() const
    {
        return _self && _self->IsInFlight();
    }

    ObjectGuid WorldView::PathThreat(std::vector<Position> const& wps, size_t from, float lookahead,
                                     uint32* packOut, uint32* strongOut, PathSkipFn skip, void* skipUser) const
    {
        if (packOut)
            *packOut = 0;
        if (strongOut)
            *strongOut = 0;
        if (!_self || from >= wps.size() || lookahead <= 0.0f)
            return ObjectGuid::Empty;

        // ЛОМАНАЯ ОТ МЕНЯ ПО ТОЧКАМ МАРШРУТА ЯДРА, пока не наберётся `lookahead` ярдов.
        std::vector<Position> line;
        line.reserve(8);
        line.emplace_back(_self->GetPositionX(), _self->GetPositionY(), _self->GetPositionZ());
        float len = 0.0f;
        for (size_t i = from; i < wps.size() && len < lookahead; ++i)
        {
            float const seg = line.back().GetExactDist2d(wps[i]);
            if (len + seg > lookahead && seg > 0.0f)
            {
                // ОБРЕЗАЕМ ПОСЛЕДНИЙ ОТРЕЗОК ДО `lookahead` (Кодекс): длинный отрезок проверялся целиком.
                float const k = (lookahead - len) / seg;
                Position const& a = line.back();
                line.emplace_back(a.GetPositionX() + (wps[i].GetPositionX() - a.GetPositionX()) * k,
                                  a.GetPositionY() + (wps[i].GetPositionY() - a.GetPositionY()) * k,
                                  a.GetPositionZ() + (wps[i].GetPositionZ() - a.GetPositionZ()) * k);
                len = lookahead;
                break;
            }
            len += seg;
            line.push_back(wps[i]);
        }
        if (line.size() < 2)
            return ObjectGuid::Empty;

        float const radius = lookahead + 40.0f;     // зона агра редко шире сорока ярдов
        std::list<Creature*> around;
        Trinity::AnyUnitInObjectRangeCheck check(_self, radius);
        Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(_self, around, check);
        Cell::VisitGridObjects(_self, searcher, radius);

        auto aggressive = [&](Creature const* c)
        {
            return c->IsAlive() && !c->IsInCombat() && c->IsHostileTo(_self)
                && c->HasReactState(REACT_AGGRESSIVE) && !c->IsCivilian();
        };

        Creature* best = nullptr;
        float bestAlong = 1.0e9f;
        for (Creature* c : around)
        {
            if (!aggressive(c) || !_self->IsValidAttackTarget(c))
                continue;
            // ГДЕ ОН ОТНОСИТЕЛЬНО ПУТИ: ближайшее расстояние до отрезков и сколько ярдов пути до этого места.
            float const cx = c->GetPositionX(), cy = c->GetPositionY();
            float minD = 1.0e9f, along = 0.0f, acc = 0.0f, nearZ = 0.0f;
            for (size_t k = 1; k < line.size(); ++k)
            {
                float const ax = line[k - 1].GetPositionX(), ay = line[k - 1].GetPositionY();
                float const bx = line[k].GetPositionX(),     by = line[k].GetPositionY();
                float const dx = bx - ax, dy = by - ay;
                float const seg2 = dx * dx + dy * dy;
                float t = seg2 > 0.0f ? ((cx - ax) * dx + (cy - ay) * dy) / seg2 : 0.0f;
                t = std::max(0.0f, std::min(1.0f, t));
                float const px = ax + t * dx - cx, py = ay + t * dy - cy;
                float const d = std::sqrt(px * px + py * py);
                float const segLen = std::sqrt(seg2);
                if (d < minD)
                {
                    minD = d;
                    along = acc + t * segLen;
                    nearZ = line[k - 1].GetPositionZ() + t * (line[k].GetPositionZ() - line[k - 1].GetPositionZ());
                }
                acc += segLen;
            }
            if (minD > c->GetAttackDistance(_self) + 2.0f)
                continue;                           // по этому пути до него не дотянется
            if (std::fabs(c->GetPositionZ() - nearZ) > 10.0f)
                continue;                           // другой этаж (Кодекс: поиск был плоским)
            if (uint8 const verdict = skip ? skip(skipUser, c->GetGUID(), c->GetEntry(), cx, cy) : 0)
            {
                if (verdict == 2 && strongOut)
                    ++*strongOut;                   // одиночка в своей смертельной клетке - ждёт отряд
                continue;                           // отложен движком или убивал дважды
            }
            if (c->IsElite() || c->GetLevelForTarget(_self) > _self->GetLevel() + 2)
            {
                if (strongOut)
                    ++*strongOut;
                continue;                           // не по силам одному - не выманиваем
            }
            uint32 around10 = 0;
            for (Creature* o : around)
                if (o != c && aggressive(o) && o->GetExactDist2d(c) <= 10.0f)
                    ++around10;
            if (around10 > 2)
            {
                if (strongOut)
                    ++*strongOut;
                continue;                           // пачка больше трёх - не одиночный пул (Кодекс)
            }
            if (along < bestAlong)
                { bestAlong = along; best = c; }
        }
        if (!best)
            return ObjectGuid::Empty;
        if (packOut)
            for (Creature* c : around)
                if (c != best && aggressive(c) && c->GetExactDist2d(best) <= 10.0f)
                    ++*packOut;
        return best->GetGUID();
    }

    ObjectGuid WorldView::SquadAttacker(float range, ObjectGuid* memberOut, PathSkipFn skip, void* skipUser) const
    {
        if (memberOut)
            *memberOut = ObjectGuid::Empty;
        Group const* g = _self ? _self->GetGroup() : nullptr;
        if (!g)
            return ObjectGuid::Empty;
        Unit* best = nullptr;
        float bestD = 1.0e9f;
        for (Group::MemberSlot const& slot : g->GetMemberSlots())
        {
            Player* m = ObjectAccessor::GetPlayer(*_self, slot.guid);
            if (!m || m == _self || !m->IsAlive() || m->GetMapId() != _self->GetMapId()
                || _self->GetExactDist2d(m) > range)
                continue;
            for (Unit* a : m->getAttackers())
            {
                if (!a || !a->IsAlive() || !_self->IsValidAttackTarget(a))
                    continue;
                if (skip && skip(skipUser, a->GetGUID(), a->GetEntry(), a->GetPositionX(), a->GetPositionY()) == 1)
                    continue;                   // отложен движком или убивал дважды (Кодекс, verdict23)
                float const d = _self->GetExactDist(a);
                if (d < bestD)
                {
                    bestD = d;
                    best = a;
                    if (memberOut)
                        *memberOut = m->GetGUID();
                }
            }
        }
        return best ? best->GetGUID() : ObjectGuid::Empty;
    }

    uint32 WorldView::SquadMatesNear(float range) const
    {
        Group const* g = _self ? _self->GetGroup() : nullptr;
        if (!g)
            return 0;
        uint32 n = 0;
        for (Group::MemberSlot const& slot : g->GetMemberSlots())
            if (Player const* m = ObjectAccessor::GetPlayer(*_self, slot.guid))
                if (m != _self && m->IsAlive() && _self->GetExactDist2d(m) <= range
                    && _self->GetPhaseShift().CanSee(m->GetPhaseShift()))    // невидимый - не поддержка (Кодекс)
                    ++n;
        return n;
    }

    ObjectGuid WorldView::SquadLeader() const
    {
        return _self ? SquadLeaderGuidFor(_self) : ObjectGuid::Empty;
    }

    void WorldView::LogSquadAssist(ObjectGuid member, ObjectGuid attacker) const
    {
        if (!_self)
            return;
        Player const* m = ObjectAccessor::GetPlayer(*_self, member);
        Unit const* a = ObjectAccessor::GetUnit(*_self, attacker);
        TC_LOG_INFO("server.worldserver",
            "Constellation ОТРЯД-БОЙ {}: помогаю {} против {} ({}, {:.0f} ярд)",
            _self->GetName(), m ? m->GetName() : std::string("?"), a ? a->GetName() : std::string("?"),
            a ? a->GetEntry() : 0u, a ? _self->GetExactDist(a) : 0.0f);
    }

    void WorldView::LogPathThreat(ObjectGuid unit, uint32 pack, uint32 strong, char const* errand) const
    {
        if (!_self)
            return;
        if (unit.IsEmpty())
        {
            TC_LOG_INFO("server.worldserver",
                "Constellation ЗАЧИСТКА {}: на пути только сильные ({}) - одному не пройти, дело «{}» ждёт три минуты",
                _self->GetName(), strong, errand ? errand : "?");
            return;
        }
        Creature const* c = ObjectAccessor::GetCreature(*_self, unit);
        TC_LOG_INFO("server.worldserver",
            "Constellation ЗАЧИСТКА {}: на пути {} ({}, {:.0f} ярд) - выманиваю; рядом с ним ещё {}, сильных пропущено {}; дело: {}",
            _self->GetName(), c ? c->GetName() : std::string("?"), c ? c->GetEntry() : 0u,
            c ? _self->GetExactDist2d(c) : 0.0f, pack, strong, errand ? errand : "?");
    }

    std::optional<ObjectGuid> WorldView::NearestAttacker() const
    {
        if (!_self)
            return std::nullopt;
        Unit* nearest = nullptr;
        float best = 1000.0f;
        for (Unit* a : _self->getAttackers())
        {
            float const d = _self->GetExactDist(a);
            if (d < best)
                { best = d; nearest = a; }
        }
        if (!nearest)
            nearest = _self->getAttackerForHelper();
        if (!nearest)
            return std::nullopt;
        return nearest->GetGUID();
    }

    uint32 WorldView::BrokenGear() const
    {
        return _self ? BrokenGearFor(_self) : 0;
    }

    bool WorldView::FleePointFrom(ObjectGuid from, Position* out) const
    {
        return _self && out && FleePointFor(_self, from, out);
    }

    bool WorldView::FollowTarget(Position* out) const
    {
        return _self && out && FollowTargetFor(_self, out);
    }

    void WorldView::PlanConsult(ObjectGuid giver) const   { if (_self) PlanConsultFor(_self, giver); }
    void WorldView::PlanMenuRead(ObjectGuid giver) const  { if (_self) PlanMenuReadFor(_self, giver); }
    void WorldView::QuestTaken() const                    { if (_self) QuestTakenFor(_self); }

    char const* WorldView::NameOf(ObjectGuid unit) const
    {
        if (!_self || unit.IsEmpty())
            return "?";
        Unit* who = ObjectAccessor::GetUnit(*_self, unit);
        return who ? who->GetName().c_str() : "?";
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

    uint32 WorldView::QuestSlotsUsed() const
    {
        if (!_self)
            return 0;
        uint32 used = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (_self->GetQuestSlotQuestId(slot))
                ++used;
        return used;
    }

    uint32 WorldView::UnmetObjectives() const
    {
        return _self ? UnmetObjectivesFor(_self) : 0u;
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

    bool WorldView::SquadLeaderSpot(TravelSpot* out) const
    {
        return _self && out && SquadLeaderSpotFor(_self, out);
    }

    bool WorldView::OffersByMenu(uint32 entry, QuestRefusedFn refused, void const* user) const
    {
        return _self && TakeableQuestAtFor(_self, entry, refused, user) != 0;
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

    bool WorldView::CanInteractWithNpc(ObjectGuid unit) const
    {
        if (!_self || unit.IsEmpty())
            return false;
        return _self->GetNPCIfCanInteractWith(unit, UNIT_NPC_FLAG_NONE, UNIT_NPC_FLAG_2_NONE) != nullptr;
    }

    std::optional<ObjectGuid> WorldView::NearestCreatureOfEntry(uint32 entry, float searchDist) const
    {
        if (!_self || !entry || searchDist <= 0.0f)
            return std::nullopt;
        std::list<Creature*> around;
        Trinity::AnyUnitInObjectRangeCheck check(_self, searchDist);
        Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(_self, around, check);
        Cell::VisitGridObjects(_self, searcher, searchDist);
        Creature* best = nullptr; float bestD = searchDist + 1.0f;
        for (Creature* cr : around)
        {
            if (cr->GetEntry() != entry || !cr->IsAlive())
                continue;
            float const d = _self->GetExactDist(cr);
            if (d < bestD)
                { bestD = d; best = cr; }
        }
        if (!best)
            return std::nullopt;
        return best->GetGUID();
    }

    bool WorldView::VendorNeed(VendorMemory const& mem, struct VendorNeed* out) const
    {
        return _self && out && VendorNeedFor(_self, mem, out);
    }

    bool WorldView::GatherSpotOf(GatherSpot* out) const
    {
        return _self && out && GatherSpotFor(_self, out);
    }

    uint32 WorldView::GatherSpawn() const
    {
        return _self ? GatherSpawnFor(_self) : 0;
    }

    char const* WorldView::GatherFocus(ClientAct& act, MoveState& move) const
    {
        return _self ? GatherFocusFor(_self, act, move) : "спутника нет";
    }

    char const* WorldView::GatherOpen(uint32 spawnId, ClientAct& act, MoveState& move) const
    {
        return _self ? GatherOpenFor(_self, spawnId, act, move) : "спутника нет";
    }

    char const* WorldView::GatherArrivedEmpty() const
    {
        return _self ? GatherArrivedEmptyFor(_self) : "спутника нет";
    }

    bool WorldView::ReserveUnit(ObjectGuid unit, uint32 ttlMs) const
    {
        return _self && !unit.IsEmpty() && ReserveUnitFor(_self, unit, ttlMs);
    }

    void WorldView::NoteVisit(uint32 spawnId) const
    {
        if (_self)
            NoteVisitFor(_self, spawnId);
    }

    void WorldView::ClearVisit() const
    {
        if (_self)
            ClearVisitFor(_self);
    }

    bool WorldView::ReserveGather(uint32 spawnId) const
    {
        return _self && spawnId && ReserveGatherFor(_self, spawnId);
    }

    bool WorldView::IsAttackingMe(ObjectGuid unit) const
    {
        return _self && !unit.IsEmpty() && IsAttackingMeFor(_self, unit);
    }

    void WorldView::ReleaseReservation() const
    {
        if (_self)
            ReleaseReservationFor(_self);
    }

    void WorldView::GatherUnreachable(uint32 backoffMs) const
    {
        if (_self)
            GatherUnreachableFor(_self, backoffMs);
    }

    void WorldView::GatherCancel() const
    {
        if (_self)
            GatherCancelFor(_self);
    }

    bool WorldView::HearthWorth(Position const& target) const
    {
        return _self && HearthWorthFor(_self, target);
    }

    bool WorldView::HearthCast(Position const& target, ClientAct& act, MoveState& move) const
    {
        return _self && HearthCastFor(_self, target, act, move);
    }

    bool WorldView::PlanFlight(Position const& target, FlightPlan* out) const
    {
        return _self && out && PlanFlightFor(_self, target, out);
    }

    bool WorldView::FlightPlanned(FlightPlan* out) const
    {
        return _self && FlightPlannedFor(_self, out);
    }

    std::optional<ObjectGuid> WorldView::FlightMasterAt() const
    {
        return _self ? FlightMasterAtFor(_self) : std::nullopt;
    }

    bool WorldView::CanInteractWithFlightMaster(ObjectGuid unit) const
    {
        if (!_self || unit.IsEmpty())
            return false;
        return _self->GetNPCIfCanInteractWith(unit, UNIT_NPC_FLAG_FLIGHTMASTER, UNIT_NPC_FLAG_2_NONE) != nullptr;
    }

    char const* WorldView::TakeFlight(ObjectGuid master, ClientAct& act, MoveState& move) const
    {
        return _self ? TakeFlightFor(_self, master, act, move) : "спутника нет";
    }

    void WorldView::FlightAbort(uint32 cooldownMs) const
    {
        if (_self)
            FlightAbortFor(_self, cooldownMs);
    }

    void WorldView::LearnTaxiNode(ClientAct& act) const
    {
        if (_self)
            LearnTaxiNodeFor(_self, act);
    }

    void WorldView::BindAtInn(ClientAct& act) const
    {
        if (_self)
            BindAtInnFor(_self, act);
    }

    bool WorldView::TradeAt(ObjectGuid vendor, VendorMemory const& mem, VendorSender const& send) const
    {
        return _self && TradeAtFor(_self, vendor, mem, send);
    }

    bool WorldView::InMeleeRange(ObjectGuid unit) const
    {
        if (!_self || unit.IsEmpty())
            return false;
        Unit* who = ObjectAccessor::GetUnit(*_self, unit);
        return who && who->IsAlive() && _self->IsWithinMeleeRange(who);
    }

    std::optional<ObjectGuid> WorldView::UsableObjectAt(ObjectGuid::LowType spawnId) const
    {
        if (!_self || !spawnId)
            return std::nullopt;
        GameObject* go = _self->GetMap()->GetGameObjectBySpawnId(spawnId);
        if (!go || !go->isSpawned() || !_self->GetGameObjectIfCanInteractWith(go->GetGUID()))
            return std::nullopt;
        return go->GetGUID();
    }

    ObjectGuid WorldView::CurrentVictim() const
    {
        Unit* v = _self ? _self->GetVictim() : nullptr;
        return v ? v->GetGUID() : ObjectGuid::Empty;
    }

    // ЖУРНАЛ КВЕСТОВ ОДНИМ ЧИСЛОМ: квест и его статус по слотам, без счётчиков целей — зачёт
    // убийства не должен снимать память разговора, а завершение или снятие квеста — должно.
    uint64 WorldView::QuestLogSignature() const
    {
        if (!_self)
            return 0;
        uint64 sig = 14695981039346656037ull;              // FNV-1a basis; the pair is mixed as one word, not by byte (a stable rolling hash is all that is needed)
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = _self->GetQuestSlotQuestId(slot);
            if (!questId)
                continue;
            uint64 const v = (uint64(questId) << 8) | uint64(uint8(_self->GetQuestStatus(questId)));
            sig ^= v;
            sig *= 1099511628211ull;
        }
        return sig;
    }

    bool WorldView::IsAliveUnit(ObjectGuid unit) const
    {
        if (!_self || unit.IsEmpty())
            return false;
        Unit* who = ObjectAccessor::GetUnit(*_self, unit);
        return who && who->IsAlive();
    }

    uint32 WorldView::EntryOf(ObjectGuid unit) const
    {
        if (!_self || unit.IsEmpty())
            return 0;
        Unit* who = ObjectAccessor::GetUnit(*_self, unit);
        return who ? who->GetEntry() : 0;
    }

    bool WorldView::StillWanted(uint32 entry) const
    {
        return _self && entry && StillWantedFor(_self, entry);
    }

    float WorldView::EngageRangeAgainst(ObjectGuid unit) const
    {
        if (!_self || unit.IsEmpty())
            return 0.0f;
        Unit* who = ObjectAccessor::GetUnit(*_self, unit);
        return who ? EngageRangeFor(_self, who) : 0.0f;
    }

    bool WorldView::LootAllowed() const
    {
        return LootAllowedFor();
    }

    bool WorldView::CloseEnough(ObjectGuid unit, float engageRange) const
    {
        if (!_self || unit.IsEmpty())
            return false;
        Unit* who = ObjectAccessor::GetUnit(*_self, unit);
        if (!who || !who->IsAlive())
            return false;
        return engageRange > 0.0f
            ? _self->IsWithinDistInMap(who, engageRange) && _self->IsWithinLOSInMap(who)
            : _self->IsWithinMeleeRange(who);
    }

    bool WorldView::IsCasting() const
    {
        return _self && _self->IsNonMeleeSpellCast(false);
    }

    bool WorldView::HasPet() const
    {
        return _self && (!_self->GetPetGUID().IsEmpty() || !_self->GetCharmedGUID().IsEmpty());
    }

    uint32 WorldView::PetSummonSpell(uint32 const* skip, size_t skipCount) const
    {
        if (!_self || !_self->IsAlive() || _self->IsInCombat() || _self->IsMounted() || _self->IsInFlight()
            || HasPet() || _self->IsNonMeleeSpellCast(false))
            return 0;
        Difficulty const diff = _self->GetMap()->GetDifficultyID();
        uint32 best = 0, bestLevel = 0;
        for (auto const& [id, ps] : _self->GetSpellMap())
        {
            if (!_self->HasActiveSpell(id))
                continue;
            if (std::find(skip, skip + skipCount, id) != skip + skipCount)
                continue;
            SpellInfo const* si = sSpellMgr->GetSpellInfo(id, diff);
            if (!si || si->IsPassive() || !si->HasEffect(SPELL_EFFECT_SUMMON_PET))
                continue;
            // `IsReady` общего отката не видит (Кодекс 58): под ним ядро отвергает запрос, и
            // нормальный призыв ушёл бы в отказанные на десять минут. Очередь - тот же вопрос.
            if (!_self->GetSpellHistory()->IsReady(si) || _self->GetSpellHistory()->HasGlobalCooldown(si)
                || !_self->CanRequestSpellCast(si, _self))
                continue;
            bool affordable = true;
            for (SpellPowerCost const& cost : si->CalcPowerCost(_self, si->GetSchoolMask()))
                if (cost.Amount > 0 && _self->GetPower(cost.Power) < cost.Amount)
                    { affordable = false; break; }
            if (!affordable)
                continue;
            if (!best || si->SpellLevel > bestLevel)
                { best = id; bestLevel = si->SpellLevel; }
        }
        return best;
    }

    void WorldView::LogPet(uint32 spellId, char const* what) const
    {
        if (!_self)
            return;
        SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        TC_LOG_INFO("server.worldserver", "Constellation ПИТОМЕЦ {} (класс {}, ур {}): {} ({}) - {}",
            _self->GetName(), uint32(_self->GetClass()), uint32(_self->GetLevel()),
            (si && si->SpellName) ? si->SpellName->Str[LOCALE_enUS] : "?", spellId, what);
    }

    float WorldView::KiteYards() const
    {
        return KiteYardsFor();
    }

    // Тело — лестницы (`ApproachingTarget`, «ОТВОД», `:3994-4021`), дословно по решениям.
    bool WorldView::BuildKiteRoute(bool packKnown, Position const& packCenter, ObjectGuid target,
                                   float yards, std::vector<Position>& out, Position* kiteTo) const
    {
        if (!_self || yards <= 0.0f)
            return false;
        Unit* who = ObjectAccessor::GetUnit(*_self, target);
        if (!who)
            return false;
        // ПРОЧЬ ОТ ЦЕНТРА ПАЧКИ, А НЕ ПРОСТО ОТ ЦЕЛИ (разбор), и точку обязан одобрить
        // ПОСТРОИТЕЛЬ МАРШРУТА: шаги по прямой протащили бы сквозь непроходимое.
        float const ang = packKnown
            ? packCenter.GetAbsoluteAngle(_self->GetPositionX(), _self->GetPositionY())
            : _self->GetAbsoluteAngle(who) + float(M_PI);
        float const kx = _self->GetPositionX() + std::cos(ang) * yards;
        float const ky = _self->GetPositionY() + std::sin(ang) * yards;
        if (!MapManager::IsValidMapCoord(_self->GetMapId(), kx, ky))
            return false;
        float const kz = _self->GetMap()->GetHeight(_self->GetPhaseShift(), kx, ky,
                                                    _self->GetPositionZ() + 3.0f);
        PathGenerator back(_self);
        if (!(kz > INVALID_HEIGHT && std::fabs(kz - _self->GetPositionZ()) < 12.0f
              && back.CalculatePath(kx, ky, kz, false)
              && !(back.GetPathType() & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE))))
            return false;
        out.clear();
        for (G3D::Vector3 const& v : back.GetPath())
            out.emplace_back(v.x, v.y, v.z);
        if (kiteTo)
            kiteTo->Relocate(kx, ky, kz);
        return !out.empty();
    }

    // Тело — `StepBackFacing` (`:12276-12291`), без отправки: её делает действие через дверь.
    bool WorldView::BackStepToward(Position const& wp, ObjectGuid face, float dt, Position* next) const
    {
        if (!_self || !next)
            return false;
        Unit* who = ObjectAccessor::GetUnit(*_self, face);
        if (!who)
            return false;
        float const speed = _self->GetSpeed(MOVE_WALK) * 0.9f;   // пятимся медленнее, и это к лучшему
        float const go = std::min(speed * dt, _self->GetExactDist2d(wp.GetPositionX(), wp.GetPositionY()));
        if (go <= 0.05f)
            return false;
        float const ang = _self->GetAbsoluteAngle(wp.GetPositionX(), wp.GetPositionY());
        float const nx = _self->GetPositionX() + std::cos(ang) * go;
        float const ny = _self->GetPositionY() + std::sin(ang) * go;
        float nz = _self->GetMap()->GetHeight(_self->GetPhaseShift(), nx, ny, _self->GetPositionZ() + 2.0f);
        if (nz <= INVALID_HEIGHT)
            nz = _self->GetPositionZ();
        next->Relocate(nx, ny, nz, _self->GetAbsoluteAngle(who));   // ЛИЦОМ К ЦЕЛИ
        return true;
    }

    bool WorldView::CastFor(ObjectGuid victim, CastSender const& send, CastMemory& m) const
    {
        return _self && CastAt(_self, victim, send, m);
    }

    bool WorldView::LootFor(ObjectGuid corpse, LootSender const& send, LootCounters& n) const
    {
        return _self && LootCorpse(_self, corpse, send, n);
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

    bool WorldView::ObjectiveSpotToWalkTo(DangerView const& danger, TravelMemory const& mem,
                                          TravelSpot* out) const
    {
        if (!_self || !out)
            return false;
        return FindTravelSpotFor(_self, danger, mem, out);
    }

    bool WorldView::TalkPlanOf(ObjectGuid who, TalkPlan* out) const
    {
        if (!_self || !out)
            return false;
        Creature* c = ObjectAccessor::GetCreature(*_self, who);
        if (!c || !c->IsAlive())
        {
            *out = TalkPlan();
            return false;
        }
        return TalkPlanFor(_self, c, out);
    }

    bool WorldView::TalkArrivedAt(ObjectGuid who, TalkPlan const& plan) const
    {
        Creature* c = _self ? ObjectAccessor::GetCreature(*_self, who) : nullptr;
        return c && TalkArrivedFor(_self, c, plan);
    }

    TalkOutcome WorldView::TalkEngageAt(ObjectGuid who, TalkPlan const& plan, TalkState& st,
                                        TalkMemory const& mem, TalkSender const& send, uint32 sliceMs) const
    {
        Creature* c = _self ? ObjectAccessor::GetCreature(*_self, who) : nullptr;
        if (!c)
            return TalkOutcome::TalkFailed;
        return TalkEngageFor(_self, c, plan, st, mem, send, sliceMs);
    }

    void WorldView::TouchTriggers(TriggerMemory const& mem, TriggerSendFn send, void* sendUser) const
    {
        if (_self)
            TouchAreaTriggersFor(_self, mem, send, sendUser);
    }

    bool WorldView::TurnInAt(ObjectGuid ender, uint32 questId, TurnInSender const& send) const
    {
        return _self && TurnInFor(_self, ender, questId, send);
    }

    bool WorldView::TalkRefuseAt(ObjectGuid who, TalkPlan const& plan, TalkState& st, TalkMemory const& mem) const
    {
        Creature* c = _self ? ObjectAccessor::GetCreature(*_self, who) : nullptr;
        if (!c)
            return false;
        TalkRefusedFor(_self, c, plan, st, mem);
        return true;
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
