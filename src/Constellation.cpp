/*
 * Copyright (C) 2026 MShkolenko <montekristo1995@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Constellation — server-side companion module for AlgalonCore.
 *
 * Phase 1-2: a fixed roster of companions (one per playable race, gender-matched
 * names), created server-side on first need and logged in through socketless
 * sessions. Sessions are NEVER handed to the session manager: a null-socket
 * session would be reaped by World::UpdateSessions (Update returns false), so
 * the module owns them and ticks them from the world-update hook itself.
 * Outbound packets are dropped by WorldSession::SendPacket's null-socket guard.
 */

#include "Registration.h"
#include "Roster.h"

#include "AccountMgr.h"
#include "BattlenetAccountMgr.h"
#include "CharacterCache.h"
#include "CharacterPackets.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "GameTime.h"
#include "Log.h"
#include "CellImpl.h"
#include "Creature.h"
#include "Group.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "MovementPackets.h"
#include "GossipDef.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PartyPackets.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSession.h"

#include "StringConvert.h"

#include <cmath>
#include <memory>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

using namespace Trinity::ChatCommands;

namespace Constellation
{
namespace
{
bool IsEnabled()
{
    return sConfigMgr->GetBoolDefault("Constellation.Enable", false);
}

bool AutoSummon()
{
    return sConfigMgr->GetBoolDefault("Constellation.AutoSummon", true);
}

// 0 = весь состав. Постоянные сессии всего состава — это в девять раз больше, чем
// было измерено, и дорожная карта требует мерить нагрузку ДО роста; ключ нужен,
// чтобы оператор мог убавить, не пересобирая ядро.
uint32 MaxActive()
{
    return sConfigMgr->GetIntDefault("Constellation.MaxActive", 0);
}

// квесты целиком: свой выключатель
bool QuestsEnabled()
{
    return sConfigMgr->GetBoolDefault("Constellation.Quests", true);
}

// сколько квестов держать в журнале разом (предел ядра — 35)
uint32 MaxQuestsHeld()
{
    return sConfigMgr->GetIntDefault("Constellation.MaxQuests", 10);
}

// как часто спутник осматривается в поисках квестодателя
uint32 QuestIntervalMs()
{
    return sConfigMgr->GetIntDefault("Constellation.QuestIntervalMs", 5000);
}

// на каком расстоянии он замечает восклицательный знак
float QuestGiverRange()
{
    return sConfigMgr->GetFloatDefault("Constellation.QuestGiverRange", 30.0f);
}

// следование целиком: выключатель, независимый от роспуска спутников
bool FollowEnabled()
{
    return sConfigMgr->GetBoolDefault("Constellation.Follow", true);
}

// как близко держаться за лидером, в ярдах
float FollowDistance()
{
    float d = sConfigMgr->GetFloatDefault("Constellation.FollowDistance", 4.0f);
    return (d > 0.5f && d < 100.0f) ? d : 4.0f;      // защита от нелепой настройки
}

// дальше этого не догоняем: отстал — значит отстал, а не рывок через полкарты
float MaxFollowRange()
{
    return sConfigMgr->GetFloatDefault("Constellation.FollowMaxRange", 60.0f);
}

// перепад высоты, который берут шагом; больше — это обрыв или уступ
float MaxStepUp()
{
    return sConfigMgr->GetFloatDefault("Constellation.MaxStepUp", 2.0f);
}

// ходить за ЖИВЫМ игроком. По умолчанию нет: иначе один человек уводит за собой
// весь состав, и это видят все вокруг.
bool FollowPlayers()
{
    return sConfigMgr->GetBoolDefault("Constellation.FollowPlayers", false);
}

std::string AccountName()
{
    return sConfigMgr->GetStringDefault("Constellation.Account", "CONSTELLATION");
}

// ---------------------------------------------------------------- companions

enum class Stage : uint8
{
    Offline,        // known, no session
    Saving,         // creation committed asynchronously; polling for the row
    EnumQueued,     // session made, char-enum sent (fills _legitCharacters)
    LoginSent,      // player-login sent, holder in flight
    InWorld,        // session->GetPlayer() is live
    Failed,         // gave up after retries; .constellation summon retries
    Dismissed       // operator said stop; ONLY .summon (or restart) wakes it —
                    // plain Offline re-entered the pipeline under AutoSummon (Codex item 4)
};

struct Companion
{
    RosterEntry const* Entry = nullptr;
    std::string PersistentName;         // the name actually stored in DB (fallback-aware)
    uint32 AccountId = 0;               // ITS OWN game account
    uint32 BnetId = 0;                  // ITS OWN battlenet account (warband isolation)
    std::string BnetEmail;
    ObjectGuid Guid;                    // filled once the character exists
    WorldSession* Session = nullptr;    // owned by the module, not the manager
    Stage State = Stage::Offline;
    uint32 TicksInState = 0;
    uint8 Retries = 0;
    bool PendingDismiss = false;        // dismissal requested mid-login; applied when safe
    uint32 MoveMs = 0;                  // накопитель времени между шагами следования
    bool Moving = false;                // мы САМИ считаем, идём ли: флаги ядра могут быть нормализованы
    bool DebugWalk = false;             // только стенд: уходить в точку, а не за лидером
    Position DebugTarget;
    uint32 QuestMs = 0;                 // накопитель между попытками взять квест
    std::set<uint32> QuestRefused;      // не берётся — не долбимся каждые пять секунд
};

class Manager
{
public:
    static Manager* Instance()
    {
        static Manager instance;
        return &instance;
    }

    void OnWorldUpdate(uint32 diff)
    {
        if (!IsEnabled())
            return;

        // gentle start: let the world settle before the first login
        if (_warmupMs < 10000)
        {
            _warmupMs += diff;
            return;
        }

        if (!_bootstrapped)
            Bootstrap();

        // Ступенчатый пуск: не больше N продвижений состояния за окно в 2 с.
        // Раньше было ровно ОДНО, и на составе из 122 спутников по пять состояний
        // каждый это давало около двадцати минут на подъём. Ограничение всё равно
        // нужно — оно разносит во времени создание персонажей и входы, — но одно
        // продвижение на окно было настроено под состав из тринадцати.
        _throttleMs += diff;
        uint32 budget = (_throttleMs >= 2000) ? sConfigMgr->GetIntDefault("Constellation.PerTick", 6) : 0;
        if (budget)
            _throttleMs = 0;

        for (Companion& c : _companions)
        {
            TickSession(c, diff);
            FollowTick(c, diff);        // каждый такт: ограничитель конвейера — про вход, не про ходьбу
            QuestTick(c, diff);
            if (budget && AdvanceOne(c))
                --budget;
        }
        DebugGroupPair();
    }

    void Shutdown()
    {
        for (Companion& c : _companions)
            Dismiss(c, /*final=*/true);
    }

    bool SummonAll(ChatHandler* handler)
    {
        uint32 woken = 0;
        for (Companion& c : _companions)
            if (c.State == Stage::Failed || c.State == Stage::Offline || c.State == Stage::Dismissed)
            {
                c.State = Stage::Offline;
                c.Retries = 0;
                c.PendingDismiss = false;
                ++woken;
            }
        handler->PSendSysMessage("Constellation: {} companions queued (auto pipeline).", woken);
        return true;
    }

    bool DismissAll(ChatHandler* handler)
    {
        uint32 dropped = 0, deferred = 0;
        for (Companion& c : _companions)
        {
            // deleting a session while its login holder is in flight is the
            // unproven-callback hazard (Codex item 1) — defer until it lands
            if (c.Session && c.Session->PlayerLoading())
            {
                c.PendingDismiss = true;
                ++deferred;
                continue;
            }
            if (c.Session)
                ++dropped;
            Dismiss(c, /*final=*/false);
        }
        handler->PSendSysMessage("Constellation: {} dismissed, {} deferred (mid-login).", dropped, deferred);
        return true;
    }

    // rig-only test path (no client on the rig): companion A invites companion B
    // THROUGH A's OWN session handler — the same CMSG_PARTY_INVITE a client sends;
    // B then auto-accepts from the InWorld tick. Driven by Constellation.DebugGroupPair.
    void DebugGroupPair()
    {
        // rig-only: refuses to run without an explicit RigMode flag, so a leaked
        // DebugGroupPair key on the live realm does nothing (Codex item 2)
        if (!sConfigMgr->GetBoolDefault("Constellation.RigMode", false))
            return;
        std::string pair = sConfigMgr->GetStringDefault("Constellation.DebugGroupPair", "");
        if (pair.empty() || _debugPairDone)
            return;
        size_t comma = pair.find(',');
        if (comma == std::string::npos)
            return;
        std::string inviterName = pair.substr(0, comma);
        std::string inviteeName = pair.substr(comma + 1);
        Companion* inviter = FindByName(inviterName);
        Companion* invitee = FindByName(inviteeName);
        if (!inviter || !invitee
            || inviter->State != Stage::InWorld || invitee->State != Stage::InWorld
            || !inviter->Session->GetPlayer() || !invitee->Session->GetPlayer())
            return;
        _debugPairDone = true;
        WorldPacket raw(CMSG_PARTY_INVITE);
        WorldPackets::Party::PartyInviteClient invite(std::move(raw));
        invite.TargetName = invitee->Session->GetPlayer()->GetName();
        invite.TargetGUID = invitee->Session->GetPlayer()->GetGUID();
        inviter->Session->HandlePartyInviteOpcode(invite);
        // и уводим лидера на 40 ярдов, чтобы ведомому было за кем идти
        Player* lp = inviter->Session->GetPlayer();
        inviter->DebugTarget = Position(lp->GetPositionX() + 40.0f, lp->GetPositionY(), lp->GetPositionZ(), 0.0f);
        inviter->DebugWalk = true;
        TC_LOG_INFO("server.worldserver", "Constellation: {} invited {} and walks 40y away", inviterName, inviteeName);
    }

    Companion* FindByName(std::string const& name)
    {
        for (Companion& c : _companions)
            if (name == c.Entry->Name)
                return &c;
        return nullptr;
    }
    // СЛЕДОВАНИЕ ПО-КЛИЕНТСКИ (нулевой инвариант), с оградами по разбору Кодекса.
    //
    // Спутник не отдаётся MotionMaster'у: серверное движение — это то, как ходят
    // СУЩЕСТВА, и бот на нём перестал бы проверять ровно тот путь, ради которого
    // существует. Он делает то же, что живой клиент: считает свой следующий шаг и
    // отправляет CMSG_MOVE_HEARTBEAT в тот же публичный обработчик, куда ядро
    // отдаёт пакет настоящего клиента.
    //
    // Шаг делается ТОЛЬКО в простом наземном состоянии. Всё остальное — вода,
    // полёт, падение, транспорт, обездвиженность — не выражается этим пакетом
    // честно, и притвориться, что выражается, значило бы врать ядру о состоянии.
    // В таких состояниях спутник просто стоит: пусть лучше отстанет, чем поедет
    // сквозь стену или провалится с обрыва.
    void FollowTick(Companion& c, uint32 diff)
    {
        if (!FollowEnabled() || c.State != Stage::InWorld || !c.Session)
            return;
        Player* self = c.Session->GetPlayer();
        if (!self || !self->IsInWorld() || !self->IsAlive())
            return;

        // управляем ли мы сами собой: в транспорте или под контролем пакет с нашим
        // guid относился бы к другому существу (Кодекс, пункт 1)
        if (self->GetUnitBeingMoved() != self)
            return;

        c.MoveMs += diff;
        if (c.MoveMs < 250)                 // 4 Гц, как поток живого клиента
            return;
        float dt = c.MoveMs / 1000.0f;
        c.MoveMs = 0;

        // На стенде лидер УХОДИТ — иначе проверка не отличает «следование
        // работает» от «оба стояли на одной точке», как и вышло в первый раз.
        // Уходит он тем же клиентским пакетом, что и все.
        Position target;
        if (c.DebugWalk)
            target = c.DebugTarget;
        else
        {
            Player* leader = FollowTarget(self);
            if (!leader)
                return;
            target = leader->GetPosition();
        }

        // только простое наземное состояние
        static uint32 const FORBIDDEN = MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FALLING
            | MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_ROOT;
        if ((self->GetUnitMovementFlags() & FORBIDDEN) || self->GetTransport()
            || self->IsInWater() || self->IsFalling() || self->IsFlying())
            return;

        float dist = self->GetExactDist2d(target.GetPositionX(), target.GetPositionY());
        float keep = FollowDistance();
        if (dist < keep)
        {
            if (c.Moving)                   // своё состояние, не догадка по флагам ядра
            {
                SendMove(c, self, self->GetPosition(), 0);
                c.Moving = false;
            }
            return;
        }
        if (dist > MaxFollowRange())        // отстал безнадёжно — не телепортируемся
            return;

        float angle = self->GetAbsoluteAngle(target.GetPositionX(), target.GetPositionY());
        float step = std::min(self->GetSpeed(MOVE_RUN) * dt, dist - keep * 0.5f);
        if (step <= 0.0f)
            return;
        float nx = self->GetPositionX() + std::cos(angle) * step;
        float ny = self->GetPositionY() + std::sin(angle) * step;

        float ground = self->GetMap()->GetHeight(self->GetPhaseShift(), nx, ny, self->GetPositionZ() + 2.0f);
        if (ground <= INVALID_HEIGHT)
            return;                         // рельеф не разрешился — стоим, а не едем вслепую
        // обрыв или уступ: шагом такой перепад не берут, и подменять им падение нельзя
        if (std::fabs(ground - self->GetPositionZ()) > MaxStepUp())
            return;
        // сквозь стену не ходим: то, чего не видно, для шага недостижимо
        if (!self->IsWithinLOS(nx, ny, ground + 2.0f))
            return;

        Position next(nx, ny, ground, angle);
        SendMove(c, self, next, MOVEMENTFLAG_FORWARD);
        c.Moving = true;
    }
    // ВЗЯТИЕ КВЕСТА — цепочкой опкодов, и выбор ТОЖЕ клиентский.
    //
    // Ограничение, которое надо назвать честно: сессия спутника БЕЗ СОКЕТА, поэтому
    // ответы сервера до неё не доходят. Правило то же, что для входа без второго
    // сокета: ДЕЙСТВИЕ идёт опкодом, СВЕДЕНИЯ берутся оттуда же, откуда их берёт
    // сервер, СОБИРАЯ ПАКЕТ КЛИЕНТУ.
    //
    // Первая версия выбирала квест через sObjectMgr->GetCreatureQuestRelations, и
    // Кодекс справедливо назвал это переходом черты: это внутренний перечень связей
    // квестодателя, а не то, что видит игрок. Теперь спутник шлёт Hello и читает
    // МЕНЮ, которое ядро только что построило В ОТВЕТ на этот Hello и попыталось
    // отправить — тот самый список, что нарисовался бы в окне у игрока, в том же
    // порядке. Пакет упал в пустой сокет, но меню осталось.
    void QuestTick(Companion& c, uint32 diff)
    {
        if (!QuestsEnabled() || c.State != Stage::InWorld || !c.Session)
            return;
        Player* self = c.Session->GetPlayer();
        if (!self || !self->IsInWorld() || !self->IsAlive())
            return;

        c.QuestMs += diff;
        if (c.QuestMs < QuestIntervalMs())
            return;
        c.QuestMs = 0;

        // журнал полон — это ВСЕ слоты заняты, а не первые три
        uint32 used = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (self->GetQuestSlotQuestId(slot))
                ++used;
        if (used >= MAX_QUEST_LOG_SIZE || used >= MaxQuestsHeld())
            return;

        Creature* giver = NearestQuestGiver(self);
        if (!giver)
            return;

        // каноническая проверка ядра: расстояние, флаги, враждебность, смерть.
        // Прямой вызов обработчика мог бы обойти то, что клиенту не позволено.
        if (!self->CanInteractWithQuestGiver(giver))
            return;

        // «подойти и заговорить» — тот же опкод, что шлёт клиент по клику
        WorldPacket rawHello(CMSG_QUEST_GIVER_HELLO);
        WorldPackets::Quest::QuestGiverHello hello(std::move(rawHello));
        hello.QuestGiverGUID = giver->GetGUID();
        c.Session->HandleQuestgiverHelloOpcode(hello);

        // читаем то, что ядро только что собрало для клиента, в его же порядке
        QuestMenu const& menu = self->PlayerTalkClass->GetQuestMenu();
        for (uint8 i = 0; i < menu.GetMenuItemCount(); ++i)
        {
            uint32 questId = menu.GetItem(i).QuestId;
            if (c.QuestRefused.count(questId))
                continue;                       // уже пробовали и не вышло
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest || !self->CanTakeQuest(quest, false))
                continue;

            WorldPacket rawAccept(CMSG_QUEST_GIVER_ACCEPT_QUEST);
            WorldPackets::Quest::QuestGiverAcceptQuest accept(std::move(rawAccept));
            accept.QuestGiverGUID = giver->GetGUID();
            accept.QuestID = questId;
            c.Session->HandleQuestgiverAcceptQuestOpcode(accept);

            QuestStatus st = self->GetQuestStatus(questId);
            if (st == QUEST_STATUS_INCOMPLETE || st == QUEST_STATUS_COMPLETE)
            {
                TC_LOG_INFO("server.worldserver", "Constellation: {} took quest {} '{}' from {}",
                    self->GetName(), questId, quest->GetLogTitle(), giver->GetName());
                ++_questsTaken;
            }
            else
            {
                // Не взялся — запоминаем и больше не долбимся. Так ведут себя
                // квесты, требующие подтверждения (общие, сопровождение): их
                // приём — отдельный опкод, и до него дело ещё не дошло.
                c.QuestRefused.insert(questId);
                TC_LOG_DEBUG("server.worldserver", "Constellation: {} was refused quest {} (status {})",
                    self->GetName(), questId, uint32(st));
            }
            return;                             // по одному за раз, как человек
        }
    }

    // Ближайший квестодатель, У КОТОРОГО ЕСТЬ ЧТО ПРЕДЛОЖИТЬ ИМЕННО ЭТОМУ спутнику.
    // Восклицательный знак над головой — это QuestGiverStatus, который ядро считает
    // для клиента; спрашиваем ровно его, а не таблицу связей.
    Creature* NearestQuestGiver(Player* self) const
    {
        std::list<Creature*> around;
        Trinity::AnyUnitInObjectRangeCheck check(self, QuestGiverRange());
        Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, around, check);
        Cell::VisitGridObjects(self, searcher, QuestGiverRange());

        Creature* best = nullptr;
        float bestDist = QuestGiverRange() + 1.0f;
        for (Creature* creature : around)
        {
            if (!creature->IsAlive())
                continue;
            if (self->GetQuestDialogStatus(creature) == QuestGiverStatus::None)
                continue;
            if (!self->IsWithinLOSInMap(creature))
                continue;
            float d = self->GetExactDist2d(creature);
            if (d < bestDist)
            {
                bestDist = d;
                best = creature;
            }
        }
        return best;
    }

    // ровно тот пакет, что шлёт клиент; ядро само решит, принять его или нет
    void SendMove(Companion& c, Player* self, Position const& pos, uint32 flags)
    {
        MovementInfo mi;
        mi.guid = self->GetGUID();
        mi.pos.Relocate(pos);
        mi.flags = flags;
        mi.time = GameTime::GetGameTimeMS();
        c.Session->HandleMovementOpcode(CMSG_MOVE_HEARTBEAT, mi);
    }

    // За кем идти: лидер группы. За ЖИВЫМ игроком — только с явного разрешения:
    // иначе один человек, взявший спутников в группу, потянул бы за собой толпу
    // (Кодекс, пункт 4). По умолчанию спутники ходят только за спутниками.
    Player* FollowTarget(Player* self) const
    {
        Group* group = self->GetGroup();
        if (!group)
            return nullptr;
        ObjectGuid leaderGuid = group->GetLeaderGUID();
        if (leaderGuid == self->GetGUID())
            return nullptr;
        Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
        if (!leader || !leader->IsInWorld() || leader->GetMap() != self->GetMap())
            return nullptr;
        if (!self->GetPhaseShift().CanSee(leader->GetPhaseShift()))   // одна карта — ещё не одно место
            return nullptr;
        if (!IsCompanionAccount(leader->GetSession()->GetAccountId()) && !FollowPlayers())
            return nullptr;
        return leader;
    }

    bool IsCompanionAccount(uint32 accountId) const
    {
        for (Companion const& c : _companions)
            if (c.AccountId == accountId)
                return true;
        return false;
    }



    void Status(ChatHandler* handler)
    {
        uint32 inWorld = 0, failed = 0;
        for (Companion const& c : _companions)
        {
            if (c.State == Stage::InWorld) ++inWorld;
            if (c.State == Stage::Failed)  ++failed;
        }
        handler->PSendSysMessage("Constellation {}: quests taken {}", CONSTELLATION_VERSION, _questsTaken);
        handler->PSendSysMessage("Constellation {}: {} — roster {}, in world {}, failed {}",
            CONSTELLATION_VERSION, IsEnabled() ? "enabled" : "disabled",
            uint32(_companions.size()), inWorld, failed);
        for (Companion const& c : _companions)
            if (c.State == Stage::InWorld && c.Session && c.Session->GetPlayer())
                handler->PSendSysMessage("  {} — {} in world", c.Entry->Name,
                    c.Session->GetPlayer()->GetName());
    }

private:
    // one-time: account + roster rows resolved (created if missing)
    void Bootstrap()
    {
        _bootstrapped = true;

        // ONE BATTLENET ACCOUNT PER COMPANION (operator, 2026-08-29). In 11.x the
        // warband shares appearances, mounts, pets and part of the achievements
        // across a BATTLENET account, so a single shared account would have all
        // thirteen testing the world out of one wardrobe -- a night elf carrying a
        // human's flight points. Each gets its own pair, like thirteen separate
        // people. A game account with bnet id 0 is not an option either: the
        // collection tables' foreign keys fail on every login.
        for (RosterEntry const& entry : Roster)
        {
            Companion c;
            c.Entry = &entry;
            c.PersistentName = entry.Name;
            if (!ResolveAccount(c))
                continue;                   // logged inside; companion not registered
            // OWNERSHIP, NOT NAME. A name-only lookup would bind the companion to a
            // REAL PLAYER's character if they happen to hold a roster name, and the
            // old migration then deleted it. Only a character on this companion's
            // OWN account is ever adopted; a foreign holder of the name is a
            // collision, handled at creation by the "us" fallback.
            for (std::string const& candidate : { std::string(entry.Name), entry.Name + std::string("us") })
            {
                CharacterCacheEntry const* cached = sCharacterCache->GetCharacterCacheByName(candidate);
                if (cached && cached->AccountId == c.AccountId)
                {
                    c.Guid = cached->Guid;
                    c.PersistentName = candidate;
                    break;
                }
            }
            _companions.push_back(c);
        }
        TC_LOG_INFO("server.loading", "Constellation: bootstrap - roster {}, one account each", uint32(_companions.size()));
        if (_companions.size() != Roster.size())
            TC_LOG_ERROR("server.loading", "Constellation: ONLY {} of {} companions provisioned - the roster is short",
                uint32(_companions.size()), uint32(Roster.size()));
    }

    // Its own battlenet + game account, created on first need. Returns false if the
    // pair cannot be established: that companion is then not registered at all,
    // which is preferable to running it on somebody's shared account.
    // A high-entropy password, not a derivable one. The first version built it from
    // the companion name and the clock, and Codex called that an authentication hole
    // with cause: these are ordinary SEC_PLAYER accounts and bnetserver is reachable
    // from the internet, so a guessable password is a way in. Nothing needs to know
    // this value -- the module logs in without it.
    static std::string RandomPassword()
    {
        static char const* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
        std::random_device rd;
        std::uniform_int_distribution<size_t> pick(0, 61);
        std::string out;
        out.reserve(24);
        for (int i = 0; i < 24; ++i)
            out += chars[pick(rd)];
        return out;
    }
    // ONE ACCOUNT PER RACE, shared by that race's classes.
    //
    // The operator's reason for splitting accounts was that a night elf must not
    // inherit a dwarf's flight points, and in 11.x the warband shares appearances,
    // mounts, pets and part of the achievements across a BATTLENET account. That is
    // a RACE-level concern: a human paladin and a human mage sharing a warband is
    // what a real player looks like. So the roster's companions live on 13 accounts,
    // not one each -- well inside CharactersPerAccount (60), since the largest race
    // offers 11 classes. Resolved once per race and cached.
    bool ResolveAccount(Companion& c)
    {
        c.BnetEmail = Trinity::StringFormat("{}-R{}@algalon.local", AccountName(), uint32(c.Entry->Race));
        auto known = _raceAccounts.find(c.Entry->Race);
        if (known != _raceAccounts.end())
        {
            c.BnetId = known->second.first;
            c.AccountId = known->second.second;
            return true;
        }

        c.BnetId = Battlenet::AccountMgr::GetId(c.BnetEmail);
        if (!c.BnetId)
        {
            std::string gameAccountName;
            if (Battlenet::AccountMgr::CreateBattlenetAccount(c.BnetEmail, RandomPassword(), true, &gameAccountName) != AccountOpResult::AOR_OK)
            {
                TC_LOG_ERROR("server.loading", "Constellation: cannot create battlenet account '{}'", c.BnetEmail);
                return false;
            }
            c.BnetId = Battlenet::AccountMgr::GetId(c.BnetEmail);
            TC_LOG_INFO("server.loading", "Constellation: account for race {} created ({}, game '{}')",
                uint32(c.Entry->Race), c.BnetEmail, gameAccountName);
        }
        if (!c.BnetId)
        {
            TC_LOG_ERROR("server.loading", "Constellation: battlenet account '{}' still unresolved", c.BnetEmail);
            return false;
        }
        c.AccountId = AccountMgr::GetId(Trinity::StringFormat("{}#1", c.BnetId));
        if (!c.AccountId)
        {
            std::string gameName = Trinity::StringFormat("{}#1", c.BnetId);
            TC_LOG_WARN("server.loading", "Constellation: race {} has a battlenet account but no game account - repairing",
                uint32(c.Entry->Race));
            if (sAccountMgr->CreateAccount(gameName, RandomPassword(), c.BnetEmail, c.BnetId, 1) != AccountOpResult::AOR_OK)
            {
                TC_LOG_ERROR("server.loading", "Constellation: cannot repair game account for race {}", uint32(c.Entry->Race));
                return false;
            }
            c.AccountId = AccountMgr::GetId(gameName);
        }
        if (!c.AccountId)
        {
            TC_LOG_ERROR("server.loading", "Constellation: game account for race {} still unresolved", uint32(c.Entry->Race));
            return false;
        }
        // the name <bnetId>#1 alone does not prove the link: a stale or mislinked
        // account carrying that name would have been accepted and used (Codex r3).
        uint32 parent = Battlenet::AccountMgr::GetIdByGameAccount(c.AccountId);
        if (parent != c.BnetId)
        {
            TC_LOG_ERROR("server.loading", "Constellation: game account {} is linked to battlenet {}, expected {} - refusing",
                c.AccountId, parent, c.BnetId);
            c.AccountId = 0;
            return false;
        }
        _raceAccounts[c.Entry->Race] = { c.BnetId, c.AccountId };
        return true;
    }



    // our sessions live outside the manager: tick them or their query
    // callbacks (char-enum, login holder) never fire
    void TickSession(Companion& c, uint32 diff)
    {
        if (!c.Session)
            return;
        // Update()'s FIRST statement closes the realm socket when the connection
        // is idle — unguarded, and a socketless session is idle forever. Keep the
        // idle clock reset every tick so that branch never runs (found by gdb:
        // null deref at m_Socket[REALM]->CloseSocket()).
        c.Session->ResetTimeOutTime(false);
        WorldSessionFilter updater(c.Session);
        c.Session->Update(diff, updater);   // returns false for null socket — ignored by design
    }

    bool AdvanceOne(Companion& c)
    {
        ++c.TicksInState;
        switch (c.State)
        {
            case Stage::Offline:
            {
                if (!AutoSummon() && c.Retries == 0 && c.TicksInState < 2)
                    return false;   // waits for .constellation summon
                if (uint32 cap = MaxActive())
                {
                    uint32 live = 0;
                    for (Companion const& o : _companions)
                        if (o.Session)
                            ++live;
                    if (live >= cap)
                        return false;   // предохранитель: не больше cap в мире разом
                }
                if (!c.AccountId)
                    return false;
                // session FIRST: Player's constructor (used by CreateCharacter)
                // dereferences it — creating before the session was the phase-1 segfault
                if (!c.Session)
                    MakeSession(c);
                if (!c.Guid)
                {
                    if (!CreateCharacter(c))
                        { Fail(c, "creation"); return true; }
                    c.State = Stage::Saving;    // commit is async: trust the row, not the call
                    c.TicksInState = 0;
                    return true;
                }
                SendEnum(c);
                return true;
            }
            case Stage::Saving:
            {
                // the character-save statements are async-connection-only, so the
                // commit cannot be made synchronous; poll for the actual row instead
                QueryResult exists = CharacterDatabase.Query(
                    Trinity::StringFormat("SELECT 1 FROM characters WHERE guid = {}", c.Guid.GetCounter()).c_str());
                if (exists)
                {
                    // cache gets the name that actually went to the database
                    sCharacterCache->AddCharacterCacheEntry(c.Guid, c.AccountId, c.PersistentName,
                        c.Entry->Sex, c.Entry->Race, c.Entry->Class, 1, false);
                    TC_LOG_INFO("server.worldserver", "Constellation: created {} ({})", c.PersistentName, c.Guid.ToString());
                    SendEnum(c);
                    return true;
                }
                if (c.TicksInState > 10)
                {
                    c.Guid.Clear();
                    Fail(c, "save-commit");
                    return true;
                }
                return false;
            }
            case Stage::EnumQueued:
            {
                // enum callback needs a few ticks; then declare the login
                if (c.TicksInState < 3)
                    return false;
                WorldPacket raw(CMSG_PLAYER_LOGIN);
                WorldPackets::Character::PlayerLogin login(std::move(raw));
                login.Guid = c.Guid;
                c.Session->HandlePlayerLoginOpcode(login);
                // the opcode handler only sends SMSG_CONNECT_TO and waits for the
                // client to open the second (instance) connection; no client will,
                // so we continue the login ourselves — this is exactly the call the
                // attaching instance socket would have made
                c.Session->HandleContinuePlayerLogin();
                c.State = Stage::LoginSent;
                c.TicksInState = 0;
                return true;
            }
            case Stage::LoginSent:
            {
                if (c.Session->GetPlayer())
                {
                    c.State = Stage::InWorld;
                    c.TicksInState = 0;
                    TC_LOG_INFO("server.worldserver", "Constellation: {} is in the world", c.Entry->Name);
                    return true;
                }
                // never tear down a session whose login holder is still in flight —
                // deleting it mid-callback is the unproven hazard (Codex item 1)
                if (c.Session->PlayerLoading())
                    return false;
                if (c.PendingDismiss)
                    { Dismiss(c, false); return true; }
                if (c.TicksInState > 15)    // ~30 s — enum not ready or rejected; retry
                {
                    if (++c.Retries > 3)
                        { Fail(c, "login"); return true; }
                    TC_LOG_INFO("server.worldserver", "Constellation: {} login retry {}", c.Entry->Name, c.Retries);
                    DropSession(c);
                    c.State = Stage::Offline;
                    c.TicksInState = 0;
                }
                return false;
            }
            case Stage::InWorld:
            {
                Player* player = c.Session->GetPlayer();
                if (!player)                    // died out of world? re-run pipeline
                {
                    DropSession(c);
                    c.State = Stage::Offline;
                    c.TicksInState = 0;
                    return false;
                }
                // invariant 0: a pending group invite is answered the way a real
                // client answers — CMSG_PARTY_INVITE_RESPONSE through the session
                // handler, never Group::AddMember
                if (player->GetGroupInvite())
                {
                    WorldPacket raw(CMSG_PARTY_INVITE_RESPONSE);
                    WorldPackets::Party::PartyInviteResponse response(std::move(raw));
                    response.Accept = true;
                    c.Session->HandlePartyInviteResponseOpcode(response);
                    TC_LOG_INFO("server.worldserver", "Constellation: {} accepted a group invite", c.Entry->Name);
                }
                return false;
            }
            case Stage::Failed:
            default:
                return false;
        }
    }

    void Fail(Companion& c, char const* what)
    {
        TC_LOG_ERROR("server.worldserver", "Constellation: {} failed at {} — parked", c.Entry->Name, what);
        DropSession(c);
        c.State = Stage::Failed;
        c.TicksInState = 0;
    }

    void MakeSession(Companion& c)
    {
        std::string name = Trinity::StringFormat("{}#1", c.BnetId);
        std::string email = c.BnetEmail;
        c.Session = new WorldSession(c.AccountId, std::move(name), c.BnetId, std::move(email),
            nullptr, SEC_PLAYER, uint8(sWorld->getIntConfig(CONFIG_EXPANSION)),
            0, "Win", Minutes(0), 65299 /* frozen client build */, {}, LOCALE_ruRU, 0, false);
    }

    void DropSession(Companion& c)
    {
        if (!c.Session)
            return;
        if (c.Session->GetPlayer())
            c.Session->LogoutPlayer(true);
        delete c.Session;
        c.Session = nullptr;
    }

    void Dismiss(Companion& c, bool final)
    {
        DropSession(c);
        // Dismissed survives AutoSummon: only .summon (or restart) re-enters the
        // pipeline. Shutdown uses it too — the world is going away anyway.
        c.State = Stage::Dismissed;
        c.Retries = 0;
        c.TicksInState = 0;
        c.PendingDismiss = false;
        (void)final;
    }

    // server-side character creation, mirroring the char-create handler but
    // synchronous: build info, default appearance, Create, save, cache
    bool CreateCharacter(Companion& c)
    {
        RosterEntry const& e = *c.Entry;

        std::string name = e.Name;
        if (sObjectMgr->CheckPlayerName(name, LOCALE_ruRU, true) != CHAR_NAME_SUCCESS
            || sCharacterCache->GetCharacterCacheByName(name))
        {
            name += "us";   // deterministic fallback; roster names are curated to not need it
            if (sObjectMgr->CheckPlayerName(name, LOCALE_ruRU, true) != CHAR_NAME_SUCCESS
                || sCharacterCache->GetCharacterCacheByName(name))
                return false;
        }
        c.PersistentName = name;    // what the DB will actually hold

        auto createInfo = std::make_shared<WorldPackets::Character::CharacterCreateInfo>();
        createInfo->Race = e.Race;
        createInfo->Class = e.Class;
        createInfo->Sex = e.Sex;
        createInfo->Name = name;

        // default appearance: first choice of every customization option for
        // this race/gender — the same body the client offers before any slider
        if (std::vector<ChrCustomizationOptionEntry const*> const* options =
                sDB2Manager.GetCustomiztionOptions(e.Race, e.Sex))
        {
            // mirror ValidateAppearance with its own calls: an option enters the set
            // only if its requirement passes, and a choice only if its requirement
            // passes AGAINST THE SET BUILT SO FAR (choice reqs may depend on other
            // selected choices — the trap that kept night elves and gnomes failing)
            std::vector<UF::ChrCustomizationChoice> picked;
            auto pickedRange = [&] {
                return Trinity::IteratorPair<UF::ChrCustomizationChoice const*>(
                    picked.data(), picked.data() + picked.size());
            };
            for (ChrCustomizationOptionEntry const* option : *options)
            {
                if (ChrCustomizationReqEntry const* req = sChrCustomizationReqStore.LookupEntry(option->ChrCustomizationReqID))
                    if (!c.Session->MeetsChrCustomizationReq(req, Races(e.Race), Classes(e.Class), false, pickedRange()))
                        continue;
                std::vector<ChrCustomizationChoiceEntry const*> const* choices =
                    sDB2Manager.GetCustomiztionChoices(option->ID);
                if (!choices || choices->empty())
                    continue;
                for (ChrCustomizationChoiceEntry const* candidate : *choices)
                {
                    if (ChrCustomizationReqEntry const* req = sChrCustomizationReqStore.LookupEntry(candidate->ChrCustomizationReqID))
                        if (!c.Session->MeetsChrCustomizationReq(req, Races(e.Race), Classes(e.Class), true, pickedRange()))
                            continue;
                    UF::ChrCustomizationChoice& choice = picked.emplace_back();
                    choice.ChrCustomizationOptionID = option->ID;
                    choice.ChrCustomizationChoiceID = candidate->ID;
                    break;
                }
            }
            for (UF::ChrCustomizationChoice const& choice : picked)
                createInfo->Customizations.push_back(choice);
        }

        std::shared_ptr<Player> newChar(new Player(c.Session), [](Player* ptr)
        {
            ptr->CleanupsBeforeDelete();
            delete ptr;
        });
        newChar->GetMotionMaster()->Initialize();
        if (!newChar->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), createInfo.get()))
        {
            TC_LOG_ERROR("server.worldserver", "Constellation: Player::Create failed for {} (race {}, class {})",
                name, e.Race, e.Class);
            return false;
        }
        newChar->setCinematic(1);

        CharacterDatabaseTransaction characterTransaction = CharacterDatabase.BeginTransaction();
        LoginDatabaseTransaction loginTransaction = LoginDatabase.BeginTransaction();
        newChar->SaveToDB(loginTransaction, characterTransaction, true);
        // async by NECESSITY: the character-save statements are flagged for the
        // async connection only ("Could not fetch prepared statement ...,
        // connection type: synchronous"). Stage::Saving polls for the row.
        CharacterDatabase.CommitTransaction(characterTransaction);
        LoginDatabase.CommitTransaction(loginTransaction);

        c.Guid = newChar->GetGUID();
        return true;
    }

    void SendEnum(Companion& c)
    {
        WorldPacket raw(CMSG_ENUM_CHARACTERS);
        WorldPackets::Character::EnumCharacters enumChars(std::move(raw));
        c.Session->HandleCharEnumOpcode(enumChars);
        c.State = Stage::EnumQueued;
        c.TicksInState = 0;
    }

    std::vector<Companion> _companions;
    bool _debugPairDone = false;
    uint32 _questsTaken = 0;
    std::unordered_map<uint8, std::pair<uint32, uint32>> _raceAccounts;   // раса -> {bnet, игровая}
    uint32 _warmupMs = 0;
    uint32 _throttleMs = 0;
    bool _bootstrapped = false;
};
}

} // namespace Constellation

class constellation_worldscript : public WorldScript
{
public:
    constellation_worldscript() : WorldScript("constellation_worldscript") { }

    void OnStartup() override
    {
        TC_LOG_INFO("server.loading", "Constellation {} — {}", CONSTELLATION_VERSION,
            Constellation::IsEnabled() ? "enabled" : "present but disabled (Constellation.Enable = 0)");
    }

    void OnUpdate(uint32 diff) override
    {
        Constellation::Manager::Instance()->OnWorldUpdate(diff);
    }

    void OnShutdown() override
    {
        Constellation::Manager::Instance()->Shutdown();
    }
};

class constellation_commandscript : public CommandScript
{
public:
    constellation_commandscript() : CommandScript("constellation_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable constellationTable =
        {
            { "status",  HandleStatus,  rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "summon",  HandleSummon,  rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "dismiss", HandleDismiss, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "constellation", constellationTable },
        };
        return commandTable;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        Constellation::Manager::Instance()->Status(handler);
        return true;
    }

    static bool HandleSummon(ChatHandler* handler)
    {
        return Constellation::Manager::Instance()->SummonAll(handler);
    }

    static bool HandleDismiss(ChatHandler* handler)
    {
        return Constellation::Manager::Instance()->DismissAll(handler);
    }
};

void AddConstellationScripts()
{
    new constellation_worldscript();
    new constellation_commandscript();
}
