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

#include "BuildStamp.h"
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
#include "CombatPackets.h"
#include "GossipDef.h"
#include "MiscPackets.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
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
// НАСТРОЙКА ЧИТАЕТСЯ ОДИН РАЗ, а не на каждом обращении.
//
// Первая версия звала sConfigMgr->Get*Default прямо из тиковых функций. Ядро пишет
// предупреждение «Missing name X in config file» КАЖДЫЙ раз, когда ключа нет в файле,
// и на 122 спутниках это дало 31 343 238 строк в журнале за один десятиминутный
// прогон — дисковый поток и синхронная запись прямо в такте мира.
//
// Значения обновляются в OnConfigLoad, поэтому `.reload config` по-прежнему меняет их
// без пересборки — требование оператора выполнено.
struct Settings
{
    bool  Enable          = false;
    bool  AutoSummon      = true;
    bool  RigMode         = false;
    bool  Follow          = true;
    bool  Quests          = true;
    bool  Fight           = true;
    uint32 MaxActive      = 0;
    uint32 PerTick        = 6;
    uint32 MaxQuests      = 10;
    uint32 QuestIntervalMs = 5000;
    float FollowDistance  = 4.0f;
    float FollowMaxRange  = 60.0f;
    float MaxStepUp       = 2.0f;
    float QuestGiverRange = 30.0f;
    float FightRange      = 120.0f;
    std::string Account   = "CONSTELLATION";
    std::string DebugGroupPair;

    void Load()
    {
        Enable          = sConfigMgr->GetBoolDefault("Constellation.Enable", false);
        AutoSummon      = sConfigMgr->GetBoolDefault("Constellation.AutoSummon", true);
        RigMode         = sConfigMgr->GetBoolDefault("Constellation.RigMode", false);
        Follow          = sConfigMgr->GetBoolDefault("Constellation.Follow", true);
        Quests          = sConfigMgr->GetBoolDefault("Constellation.Quests", true);
        Fight           = sConfigMgr->GetBoolDefault("Constellation.Fight", true);
        MaxActive       = sConfigMgr->GetIntDefault("Constellation.MaxActive", 0);
        PerTick         = sConfigMgr->GetIntDefault("Constellation.PerTick", 6);
        MaxQuests       = sConfigMgr->GetIntDefault("Constellation.MaxQuests", 10);
        QuestIntervalMs = sConfigMgr->GetIntDefault("Constellation.QuestIntervalMs", 5000);
        float d         = sConfigMgr->GetFloatDefault("Constellation.FollowDistance", 4.0f);
        FollowDistance  = (d > 0.5f && d < 100.0f) ? d : 4.0f;    // защита от нелепой настройки
        FollowMaxRange  = sConfigMgr->GetFloatDefault("Constellation.FollowMaxRange", 60.0f);
        MaxStepUp       = sConfigMgr->GetFloatDefault("Constellation.MaxStepUp", 2.0f);
        QuestGiverRange = sConfigMgr->GetFloatDefault("Constellation.QuestGiverRange", 30.0f);
        // широко: цель ищется по округе, а не на длину руки — к ней ходят
    FightRange      = sConfigMgr->GetFloatDefault("Constellation.FightRange", 120.0f);
        Account         = sConfigMgr->GetStringDefault("Constellation.Account", "CONSTELLATION");
        DebugGroupPair  = sConfigMgr->GetStringDefault("Constellation.DebugGroupPair", "");
    }
};

inline Settings& Cfg()
{
    static Settings s;
    return s;
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

enum class Behavior : uint8 { Idle, FollowingOwner, ApproachingTarget, Attacking, TurningIn };

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
    uint32 FightMs = 0;                 // накопитель между решениями в бою
    Behavior Mode = Behavior::Idle;     // ровно одно намерение за раз
    ObjectGuid TargetGuid;              // цель, которая не меняется по дороге
    uint32 ModeMs = 0;                  // сколько в этом состоянии — для сроков
    float LastDist = 0.0f;              // для проверки, что мы вообще приближаемся
    std::vector<Position> Waypoints;    // маршрут, построенный ядром
    size_t WaypointIndex = 0;
    float PathTargetX = 0.0f, PathTargetY = 0.0f;
    uint32 TurnInQuest = 0;             // что сдаём
    uint32 TurnInEntry = 0;             // кому
    Position TurnInPos;                 // и где он стоит
    ObjectGuid Owner;                   // кто позвал; пусто = не идти ни за кем
    std::set<ObjectGuid> Refused;       // цели, до которых не дойти или не ударить
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
        if (!Cfg().Enable)
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
        uint32 budget = (_throttleMs >= 2000) ? Cfg().PerTick : 0;
        if (budget)
            _throttleMs = 0;

        for (Companion& c : _companions)
        {
            TickSession(c, diff);
            BehaveTick(c, diff);        // автомат поведения: одно намерение за раз
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

    // Стендовая проверка следования. Спутник НИКОГДА никого не приглашает — правило
    // оператора, — поэтому на стенде, где живого игрока нет, хозяин назначается
    // напрямую, без группы вовсе. Это честнее прежней спайки: та заставляла бота
    // выдать приглашение, чего в жизни не будет никогда.
    void DebugGroupPair()
    {
        if (!Cfg().RigMode)
            return;
        std::string const& pair = Cfg().DebugGroupPair;
        if (pair.empty() || _debugPairDone)
            return;
        size_t comma = pair.find(',');
        if (comma == std::string::npos)
            return;
        Companion* leader = FindByName(pair.substr(0, comma));
        Companion* follower = FindByName(pair.substr(comma + 1));
        if (!leader || !follower
            || leader->State != Stage::InWorld || follower->State != Stage::InWorld
            || !leader->Session->GetPlayer() || !follower->Session->GetPlayer())
            return;
        _debugPairDone = true;
        Player* lp = leader->Session->GetPlayer();
        follower->Owner = lp->GetGUID();
        leader->DebugTarget = Position(lp->GetPositionX() + 40.0f, lp->GetPositionY(), lp->GetPositionZ(), 0.0f);
        leader->DebugWalk = true;
        TC_LOG_INFO("server.worldserver", "Constellation: стенд — {} ведёт {}, уходит на 40 ярдов",
            lp->GetName(), follower->Session->GetPlayer()->GetName());
    }

    Companion* FindByName(std::string const& name)
    {
        for (Companion& c : _companions)
            if (name == c.Entry->Name)
                return &c;
        return nullptr;
    }
    // ОДИН ШАГ К ТОЧКЕ, клиентским пакетом. Общий для следования и подхода к цели.
    //
    // Маршрут строит ЯДРО. Путь к этому был длинный и поучительный:
    //   1. три самодельные ограды (высота, перепад, прямая видимость) — последняя
    //      отвергала шаг в 1.8 ярда на ровном месте: доходило 4 подхода из 52;
    //   2. штатный GetFirstCollisionPosition — луч по навигационной сетке. Лучше,
    //      но луч ПРЯМОЙ: спутники стоят в аббатстве, цели снаружи, и луч упирается
    //      в стену в двадцати сантиметрах. Дошло 2 из 55.
    //   3. PathGenerator — тот же построитель, которым ядро водит существ. Он умеет
    //      ОБХОДИТЬ: строит маршрут по сетке и возвращает точки. Спутник идёт по
    //      ним, как игрок, кликнувший в нужную сторону.
    //
    // Инвариант цел: путь — это решение «куда бежать», а само движение по-прежнему
    // уходит клиентским пакетом и проверяется ядром.
    bool StepToward(Companion& c, Player* self, float tx, float ty, float stopAt, float dt)
    {
        static uint32 const FORBIDDEN = MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FALLING
            | MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_ROOT;
        if ((self->GetUnitMovementFlags() & FORBIDDEN) || self->GetTransport()
            || self->IsInWater() || self->IsFalling() || self->IsFlying())
            return false;

        float dist = self->GetExactDist2d(tx, ty);
        if (dist < stopAt)
        {
            if (c.Moving)
            {
                SendMove(c, self, self->GetPosition(), 0);
                c.Moving = false;
            }
            return false;
        }

        float step = std::min(self->GetSpeed(MOVE_RUN) * dt, dist - stopAt * 0.5f);
        if (step <= 0.0f)
            return false;

        // маршрут пересчитывается не каждый шаг: он нужен, только пока мы далеко
        // от следующей его точки
        if (c.Waypoints.empty() || c.WaypointIndex >= c.Waypoints.size()
            || self->GetExactDist2d(c.PathTargetX, c.PathTargetY) > 5.0f
                && (std::fabs(c.PathTargetX - tx) > 3.0f || std::fabs(c.PathTargetY - ty) > 3.0f))
        {
            PathGenerator path(self);
            float groundZ = self->GetMap()->GetHeight(self->GetPhaseShift(), tx, ty, self->GetPositionZ() + 5.0f);
            if (groundZ <= INVALID_HEIGHT)
                groundZ = self->GetPositionZ();
            if (!path.CalculatePath(tx, ty, groundZ, false)
                || (path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_SHORTCUT)))
            {
                if (!_stepDiagDone)
                {
                    _stepDiagDone = true;
                    TC_LOG_INFO("server.worldserver", "Constellation STEP {}: маршрута нет, тип {:X}, точек {}",
                        self->GetName(), uint32(path.GetPathType()), uint32(path.GetPath().size()));
                }
                return false;
            }
            c.Waypoints.clear();
            for (G3D::Vector3 const& v : path.GetPath())
                c.Waypoints.emplace_back(v.x, v.y, v.z);
            c.WaypointIndex = 0;
            c.PathTargetX = tx;
            c.PathTargetY = ty;
        }

        // идём к текущей точке маршрута; дошли — берём следующую
        while (c.WaypointIndex < c.Waypoints.size()
            && self->GetExactDist2d(c.Waypoints[c.WaypointIndex].GetPositionX(),
                                    c.Waypoints[c.WaypointIndex].GetPositionY()) < 1.5f)
            ++c.WaypointIndex;
        if (c.WaypointIndex >= c.Waypoints.size())
        {
            c.Waypoints.clear();
            return false;                   // маршрут пройден
        }

        Position const& wp = c.Waypoints[c.WaypointIndex];
        float angle = self->GetAbsoluteAngle(wp.GetPositionX(), wp.GetPositionY());
        float legLen = self->GetExactDist2d(wp.GetPositionX(), wp.GetPositionY());
        float go = std::min(step, legLen);
        Position next(self->GetPositionX() + std::cos(angle) * go,
                      self->GetPositionY() + std::sin(angle) * go,
                      wp.GetPositionZ(), angle);
        SendMove(c, self, next, MOVEMENTFLAG_FORWARD);
        c.Moving = true;
        return true;
    }
    // ==================== АВТОМАТ ПОВЕДЕНИЯ ====================
    //
    // Предложение Кодекса, принятое оператором 2026-08-30: перестать наращивать
    // поведения и свести их в ЯВНЫЙ автомат с жёсткими правилами. Причина — вся
    // цепочка сегодняшних ошибок родилась не из сложных мест, а из двух подсистем,
    // обменивавшихся булевым флагом на разных часах: подход, запертый за чужим
    // выключателем; цель, переискиваемая каждую секунду; удар, отложенный на такт;
    // подход без конца. Каждую чинили отдельно. Автомат гасит их класс.
    //
    // ПРАВИЛА, которые он держит:
    //   * у спутника РОВНО ОДНО намерение за раз — Mode, и оно решает, куда он идёт;
    //   * цель не меняется, пока идём: только смерть, недоступность или срок;
    //   * КАЖДЫЙ подход обязан кончиться ударом, отменой или сроком — молча повиснуть
    //     нельзя, у каждого состояния есть предельное время;
    //   * пишется только ПЕРЕХОД, с причиной, — а не событие на каждом такте: прежняя
    //     потактовая запись дала 31 миллион строк за прогон.
    void BehaveTick(Companion& c, uint32 diff)
    {
        if (c.State != Stage::InWorld || !c.Session)
            return;
        Player* self = c.Session->GetPlayer();
        if (!self || !self->IsInWorld())
            return;

        c.ModeMs += diff;
        c.MoveMs += diff;
        if (c.MoveMs < 250)                 // 4 Гц, как поток живого клиента
            return;
        float dt = c.MoveMs / 1000.0f;
        c.MoveMs = 0;

        // мёртвый спутник ничего не делает — но и не висит в бою
        if (!self->IsAlive())
        {
            if (c.Mode != Behavior::Idle)
                Switch(c, self, Behavior::Idle, "погиб");
            return;
        }
        // не управляем собой (транспорт, контроль) — пакет с нашим guid относился бы
        // к другому существу
        if (self->GetUnitBeingMoved() != self)
            return;

        switch (c.Mode)
        {
            case Behavior::Idle:
            {
                // ВЫПОЛНЕННОЕ СДАЁМ ПЕРВЫМ ДЕЛОМ: висящий в журнале готовый квест
                // занимает место и не даёт взять следующий, а награда — это опыт,
                // без которого спутник останется первого уровня навсегда.
                if (Cfg().Quests && FindTurnIn(c, self))
                {
                    Switch(c, self, Behavior::TurningIn, "есть что сдать");
                    return;
                }
                if (Creature* target = Cfg().Fight ? FindObjectiveTarget(c, self) : nullptr)
                {
                    c.TargetGuid = target->GetGUID();
                    c.LastDist = self->GetExactDist2d(target);
                    Switch(c, self, Behavior::ApproachingTarget, "нашлась цель квеста");
                    return;
                }
                // за хозяином идём, только если он ДОСТИЖИМ. Иначе спутник метался
                // «пошёл — далеко — стою» по нескольку раз в секунду: 420 холостых
                // переходов за прогон, из которых ни один ничего не менял.
                Position owner;
                if (Cfg().Follow && FollowTargetPos(c, self, &owner)
                    && self->GetExactDist2d(owner.GetPositionX(), owner.GetPositionY()) <= Cfg().FollowMaxRange)
                    Switch(c, self, Behavior::FollowingOwner, "есть за кем идти");
                return;
            }

            case Behavior::FollowingOwner:
            {
                // дело важнее сопровождения: цель квеста перебивает следование
                if (Creature* target = Cfg().Fight ? FindObjectiveTarget(c, self) : nullptr)
                {
                    c.TargetGuid = target->GetGUID();
                    c.LastDist = self->GetExactDist2d(target);
                    Switch(c, self, Behavior::ApproachingTarget, "нашлась цель квеста");
                    return;
                }
                Position owner;
                if (!Cfg().Follow || !FollowTargetPos(c, self, &owner))
                {
                    Switch(c, self, Behavior::Idle, "идти не за кем");
                    return;
                }
                if (self->GetExactDist2d(owner.GetPositionX(), owner.GetPositionY()) > Cfg().FollowMaxRange)
                {
                    Switch(c, self, Behavior::Idle, "хозяин слишком далеко");
                    return;
                }
                StepToward(c, self, owner.GetPositionX(), owner.GetPositionY(), Cfg().FollowDistance, dt);
                return;
            }

            case Behavior::ApproachingTarget:
            {
                Creature* target = ObjectAccessor::GetCreature(*self, c.TargetGuid);
                if (!target || !target->IsAlive())
                {
                    Switch(c, self, Behavior::Idle, "цель мертва или исчезла");
                    return;
                }
                if (!self->IsValidAttackTarget(target)
                    || !self->GetPhaseShift().CanSee(target->GetPhaseShift()))
                {
                    c.Refused.insert(c.TargetGuid);
                    Switch(c, self, Behavior::Idle, "цель стала недоступной");
                    return;
                }
                float dist = self->GetExactDist2d(target);
                if (dist <= 5.0f)
                {
                    if (TryAttack(c, self, target))
                        Switch(c, self, Behavior::Attacking, "дошёл и ударил");
                    else
                    {
                        c.Refused.insert(c.TargetGuid);
                        Switch(c, self, Behavior::Idle, "удар не принят ядром");
                    }
                    return;
                }
                StepToward(c, self, target->GetPositionX(), target->GetPositionY(), 4.0f, dt);
                if (dist < c.LastDist - 0.5f)
                {
                    c.LastDist = dist;      // продвинулись — срок отсчитывается заново
                    c.ModeMs = 0;
                }
                // СРОК: подход не может длиться вечно
                else if (c.ModeMs > 20000)
                {
                    c.Refused.insert(c.TargetGuid);
                    Switch(c, self, Behavior::Idle, "20 с без продвижения к цели");
                }
                return;
            }

            case Behavior::TurningIn:
            {
                // идём к точке принимающего; дойдя — ищем его в двух шагах
                float d = self->GetExactDist2d(c.TurnInPos.GetPositionX(), c.TurnInPos.GetPositionY());
                if (d > 6.0f)
                {
                    StepToward(c, self, c.TurnInPos.GetPositionX(), c.TurnInPos.GetPositionY(), 5.0f, dt);
                    if (c.ModeMs > 60000)
                        Switch(c, self, Behavior::Idle, "минуту не дойти до принимающего");
                    return;
                }
                std::list<Creature*> near;
                Trinity::AnyUnitInObjectRangeCheck check(self, 12.0f);
                Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, near, check);
                Cell::VisitGridObjects(self, searcher, 12.0f);
                for (Creature* creature : near)
                {
                    if (creature->GetEntry() != c.TurnInEntry || !creature->IsAlive())
                        continue;
                    if (!self->CanInteractWithQuestGiver(creature))
                        continue;
                    TurnInAt(c, self, creature);
                    Switch(c, self, Behavior::Idle, "сдал");
                    return;
                }
                Switch(c, self, Behavior::Idle, "у точки принимающего нет");
                return;
            }

            case Behavior::Attacking:
            {
                Unit* victim = self->GetVictim();
                if (!victim || !victim->IsAlive())
                {
                    Switch(c, self, Behavior::Idle, victim ? "цель убита" : "бой прекратился");
                    return;
                }
                if (self->GetExactDist2d(victim) > 6.0f)
                {
                    // отошли или цель убежала — догоняем, оставаясь на той же цели
                    StepToward(c, self, victim->GetPositionX(), victim->GetPositionY(), 4.0f, dt);
                }
                // СРОК: бой, который не кончается, — это находка, а не норма
                if (c.ModeMs > 120000)
                    Switch(c, self, Behavior::Idle, "две минуты боя без исхода");
                return;
            }
        }
    }

    // Переход — ЕДИНСТВЕННОЕ место, где пишется строка. Потактовая запись однажды
    // дала 31 миллион строк; переходов у спутника единицы в минуту.
    void Switch(Companion& c, Player* self, Behavior to, char const* why)
    {
        if (c.Mode == to)
            return;
        TC_LOG_INFO("server.worldserver", "Constellation FSM {}: {} -> {} ({})",
            self->GetName(), ModeName(c.Mode), ModeName(to), why);
        c.Mode = to;
        c.ModeMs = 0;
        if (to != Behavior::ApproachingTarget && to != Behavior::Attacking)
            c.TargetGuid.Clear();
        ++_transitions;
    }

    static char const* ModeName(Behavior b)
    {
        switch (b)
        {
            case Behavior::Idle:              return "стою";
            case Behavior::FollowingOwner:    return "иду за хозяином";
            case Behavior::ApproachingTarget: return "иду к цели";
            case Behavior::Attacking:         return "бью";
            case Behavior::TurningIn:         return "сдаю квест";
        }
        return "?";
    }

    // позиция хозяина, если за кем идти; заодно отвечает на вопрос «есть ли кто»
    bool FollowTargetPos(Companion const& c, Player* self, Position* out = nullptr) const
    {
        if (c.DebugWalk)
        {
            if (out)
                *out = c.DebugTarget;
            return true;
        }
        Player* owner = FollowTarget(self);
        if (!owner)
            return false;
        if (out)
            *out = owner->GetPosition();
        return true;
    }

    // выбрать цель и ударить — ровно то, что делает игрок мышью
    bool TryAttack(Companion& c, Player* self, Creature* target)
    {
        WorldPacket rawSel(CMSG_SET_SELECTION);
        WorldPackets::Misc::SetSelection sel(std::move(rawSel));
        sel.Selection = target->GetGUID();
        c.Session->HandleSetSelectionOpcode(sel);

        WorldPacket rawSwing(CMSG_ATTACK_SWING);
        WorldPackets::Combat::AttackSwing swing(std::move(rawSwing));
        swing.Victim = target->GetGUID();
        c.Session->HandleAttackSwingOpcode(swing);

        // проверяем ПОСЛЕДСТВИЕ, а не факт вызова: сокета нет, ответа не будет
        if (self->GetVictim() == target)
        {
            ++_fightsStarted;
            return true;
        }
        TC_LOG_INFO("server.worldserver", "Constellation: {} — удар по {} ({}) не принят: жертва {}, дистанция {:.1f}",
            self->GetName(), target->GetName(), target->GetEntry(),
            self->GetVictim() ? self->GetVictim()->GetName() : "нет", self->GetExactDist2d(target));
        return false;
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
        if (!Cfg().Quests || c.State != Stage::InWorld || !c.Session)
            return;
        Player* self = c.Session->GetPlayer();
        if (!self || !self->IsInWorld() || !self->IsAlive())
            return;

        c.QuestMs += diff;
        if (c.QuestMs < Cfg().QuestIntervalMs)
            return;
        c.QuestMs = 0;

        // журнал полон — это ВСЕ слоты заняты, а не первые три
        uint32 used = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (self->GetQuestSlotQuestId(slot))
                ++used;
        if (used >= MAX_QUEST_LOG_SIZE || used >= Cfg().MaxQuests)
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
    // КОМУ СДАВАТЬ И ГДЕ ОН СТОИТ.
    //
    // Оператор, 2026-08-30: «у квестодателей и принимающих есть конкретные точки,
    // их не надо искать, а идти к ним». Так и есть, и так делает игрок: журнал
    // показывает ему, куда вернуться, а не заставляет обшаривать местность.
    //
    // Прежняя версия искала принимающего вокруг — сперва в 30 ярдах, потом в 120, —
    // и не находила: спутник уходит за добычей, а принимающий остаётся у себя.
    // Теперь берём его ТОЧКУ: квест -> кто принимает -> где он стоит. Всё это ядро
    // и так отдаёт клиенту, рисуя метку возврата.
    bool FindTurnIn(Companion& c, Player* self) const
    {
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 qid = self->GetQuestSlotQuestId(slot);
            if (!qid || self->GetQuestStatus(qid) != QUEST_STATUS_COMPLETE)
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(qid);
            if (!quest || !self->CanRewardQuest(quest, false))
                continue;

            // кто принимает этот квест
            for (auto const& [_, enderEntry] : sObjectMgr->GetCreatureQuestInvolvedRelationReverseBounds(qid))
            {
                // ближайшая его точка на нашей карте
                Position const* bestSpawn = nullptr;
                float bestDist = 100000.0f;
                for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
                {
                    if (data.id != enderEntry || data.mapId != self->GetMapId())
                        continue;
                    float d = self->GetExactDist2d(data.spawnPoint.GetPositionX(), data.spawnPoint.GetPositionY());
                    if (d < bestDist)
                    {
                        bestDist = d;
                        bestSpawn = &data.spawnPoint;
                    }
                }
                if (!bestSpawn)
                    continue;
                c.TurnInQuest = qid;
                c.TurnInEntry = enderEntry;
                c.TurnInPos = *bestSpawn;
                return true;
            }
        }
        return false;
    }

    // Сдача: поздороваться, сдать, выбрать награду — теми же опкодами, что клиент.
    // Правда берётся из СОСТОЯНИЯ: сокета нет, ответа не будет, поэтому после сдачи
    // спрашиваем, числится ли квест награждённым.
    void TurnInAt(Companion& c, Player* self, Creature* ender)
    {
        WorldPacket rawHello(CMSG_QUEST_GIVER_HELLO);
        WorldPackets::Quest::QuestGiverHello hello(std::move(rawHello));
        hello.QuestGiverGUID = ender->GetGUID();
        c.Session->HandleQuestgiverHelloOpcode(hello);

        Quest const* quest = sObjectMgr->GetQuestTemplate(c.TurnInQuest);
        if (!quest || self->GetQuestStatus(c.TurnInQuest) != QUEST_STATUS_COMPLETE)
            return;

        WorldPacket rawDone(CMSG_QUEST_GIVER_COMPLETE_QUEST);
        WorldPackets::Quest::QuestGiverCompleteQuest done(std::move(rawDone));
        done.QuestGiverGUID = ender->GetGUID();
        done.QuestID = c.TurnInQuest;
        c.Session->HandleQuestgiverCompleteQuest(done);

        WorldPacket rawPick(CMSG_QUEST_GIVER_CHOOSE_REWARD);
        WorldPackets::Quest::QuestGiverChooseReward pick(std::move(rawPick));
        pick.QuestGiverGUID = ender->GetGUID();
        pick.QuestID = c.TurnInQuest;
        pick.Choice.Item.ItemID = 0;
        c.Session->HandleQuestgiverChooseRewardOpcode(pick);

        if (self->IsQuestRewarded(c.TurnInQuest))
        {
            TC_LOG_INFO("server.worldserver", "Constellation: {} сдал квест {} '{}' (уровень {})",
                self->GetName(), c.TurnInQuest, quest->GetLogTitle(), uint32(self->GetLevel()));
            ++_questsTurnedIn;
        }
        else
            TC_LOG_INFO("server.worldserver", "Constellation: {} — сдача квеста {} не прошла",
                self->GetName(), c.TurnInQuest);
    }

    // Существо рядом, которое ЧИСЛИТСЯ ЦЕЛЬЮ незакрытого квеста в журнале.
    // Что именно убивать и сколько — знает ядро из quest_objectives; спрашиваем его.
    Creature* FindObjectiveTarget(Companion const& c, Player* self) const
    {
        // какие виды существ нам вообще нужны
        std::set<uint32> wanted;
        uint32 slotsUsed = 0, incomplete = 0, monsterObjs = 0, unmet = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 questId = self->GetQuestSlotQuestId(slot);
            if (!questId)
                continue;
            ++slotsUsed;
            if (self->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
                continue;
            ++incomplete;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;
            for (QuestObjective const& obj : quest->GetObjectives())
            {
                if (obj.Type != QUEST_OBJECTIVE_MONSTER || obj.ObjectID <= 0)
                    continue;
                ++monsterObjs;
                if (self->GetQuestObjectiveData(obj) >= obj.Amount)
                    continue;                       // эта цель уже набрана
                ++unmet;
                wanted.insert(uint32(obj.ObjectID));
            }
        }
        if (!_fightDiagDone && slotsUsed)
        {
            _fightDiagDone = true;
            TC_LOG_INFO("server.worldserver",
                "Constellation DIAG {}: слотов занято {}, незакрытых {}, целей-убить {}, ненабранных {}, видов {}",
                self->GetName(), slotsUsed, incomplete, monsterObjs, unmet, uint32(wanted.size()));
        }
        if (wanted.empty())
            return nullptr;

        std::list<Creature*> around;
        Trinity::AnyUnitInObjectRangeCheck check(self, Cfg().FightRange);
        Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, around, check);
        Cell::VisitGridObjects(self, searcher, Cfg().FightRange);

        uint32 seen = 0, matched = 0, rejected = 0, rejBusy = 0, rejInvalid = 0, rejLos = 0, rejPhase = 0;
        uint32 lastEntry = 0, lastFaction = 0;
        Creature* best = nullptr;
        float bestDist = Cfg().FightRange + 1.0f;
        for (Creature* creature : around)
        {
            ++seen;
            if (!creature->IsAlive() || !wanted.count(creature->GetEntry()))
                continue;
            ++matched;
            // ФАЗА. Поиск по сетке возвращает существ независимо от фазы, а игрок
            // видит только свою: в стартовых зонах их несколько, и без этой проверки
            // спутник целится в тех, кого на его месте не увидел бы вовсе.
            if (!self->GetPhaseShift().CanSee(creature->GetPhaseShift()))
                { ++rejPhase; ++rejected; continue; }
            // чужую добычу не отбираем: тот, кто уже с кем-то дерётся, не наш
            if (creature->IsInCombat() && creature->GetVictim() != self)
                { ++rejBusy; ++rejected; continue; }
            if (!self->IsValidAttackTarget(creature))
            {
                ++rejInvalid; ++rejected;
                lastEntry = creature->GetEntry(); lastFaction = creature->GetFaction();
                // Разбираем ОТКАЗ ЯДРА по составляющим, вместо догадок: повторяем те же
                // условия из WorldObject::IsValidAttackTarget (Object.cpp:2375+) и пишем,
                // какое именно не выполнено. Один раз за прогон.
                if (!_whyDone)
                {
                    _whyDone = true;
                    // САМЫЙ ИНФОРМАТИВНЫЙ ВОПРОС: видит ли спутник вообще КОГО-НИБУДЬ?
                    // Если не видит никого — беда в самом спутнике, и перебирать
                    // выходы функции видимости по одному бессмысленно. Если видит
                    // прочих, но не цель, — беда в цели.
                    uint32 vis = 0, invis = 0;
                    Creature* seenExample = nullptr;
                    for (Creature* any : around)
                        if (self->CanSeeOrDetect(any)) { ++vis; if (!seenExample) seenExample = any; }
                        else ++invis;
                    TC_LOG_INFO("server.worldserver",
                        "Constellation SEE {}: вокруг {} существ, вижу {}, не вижу {}; пример видимого: {} ({})",
                        self->GetName(), uint32(around.size()), vis, invis,
                        seenExample ? seenExample->GetName() : "НИКОГО",
                        seenExample ? seenExample->GetEntry() : 0);
                    // Раз невидимы ВСЕ поголовно — виноват выход, не разбирающий цели.
                    // Единственный такой: сопоставление «живой/призрак». Печатаем его
                    // числа и состояние жизни спутника.
                    // ВЕТКА ГМ — версия Кодекса. Прямой возврат срабатывает, только
                    // если у цели видимость для ГМ НЕНУЛЕВАЯ. Меряем оба числа, а не
                    // рассуждаем: SetGameMaster(false) ставит SEC_PLAYER, а это 0,
                    // то есть само по себе ничего бы не изменило.
                    {
                        uint32 gmMine = self->m_serverSideVisibilityDetect.GetValue(SERVERSIDE_VISIBILITY_GM);
                        uint32 gmTarget = creature->m_serverSideVisibility.GetValue(SERVERSIDE_VISIBILITY_GM);
                        uint32 withGm = 0;
                        for (Creature* any : around)
                            if (any->m_serverSideVisibility.GetValue(SERVERSIDE_VISIBILITY_GM))
                                ++withGm;
                        TC_LOG_INFO("server.worldserver",
                            "Constellation GM {}: моё обнаружение ГМ={} у цели видимость ГМ={} | существ с ненулевой видимостью ГМ: {} из {}",
                            self->GetName(), gmMine, gmTarget, withGm, uint32(around.size()));
                    }

                    // ПРОВЕРКА САМОГО ЗАМЕРА: себя спутник обязан видеть всегда
                    // (первая же строка функции: this == obj -> true). Если и это
                    // ложь — врёт мой вызов, а не ядро. И отдельно: видит ли он
                    // ДРУГОГО СПУТНИКА, то есть игрока, а не существо.
                    Player* otherBot = nullptr;
                    for (Companion const& o : _companions)
                        if (o.Session && o.Session->GetPlayer() && o.Session->GetPlayer() != self)
                            { otherBot = o.Session->GetPlayer(); break; }
                    TC_LOG_INFO("server.worldserver",
                        "Constellation SELF {}: вижу себя={} вижу другого спутника={} ({}) карта своя={} карта его={}",
                        self->GetName(), self->CanSeeOrDetect(self),
                        otherBot ? self->CanSeeOrDetect(otherBot) : false,
                        otherBot ? otherBot->GetName() : "нет",
                        self->GetMap() ? self->GetMap()->GetId() : 0,
                        (otherBot && otherBot->GetMap()) ? otherBot->GetMap()->GetId() : 0);
                    TC_LOG_INFO("server.worldserver",
                        "Constellation GHOST {}: моё обнаружение={} видимость цели={} | isDead={} IsAlive={} health={} deathState={}",
                        self->GetName(),
                        self->m_serverSideVisibilityDetect.GetValue(SERVERSIDE_VISIBILITY_GHOST),
                        creature->m_serverSideVisibility.GetValue(SERVERSIDE_VISIBILITY_GHOST),
                        self->isDead(), self->IsAlive(), self->GetHealth(),
                        uint32(self->getDeathState()));
                    TC_LOG_INFO("server.worldserver",
                        "Constellation WHY {} -> {} ({}) на {:.0f} ярдах: вижу={} | без фазы={} | со спавн-слежением={} | и то и то={} | одна карта={}",
                        self->GetName(), creature->GetName(), creature->GetEntry(),
                        self->GetExactDist2d(creature),
                        self->CanSeeOrDetect(creature),
                        self->CanSeeOrDetect(creature, { .IgnorePhaseShift = true }),
                        self->CanSeeOrDetect(creature, { .IncludeHiddenBySpawnTracking = true }),
                        self->CanSeeOrDetect(creature, { .IgnorePhaseShift = true, .IncludeHiddenBySpawnTracking = true }),
                        self->GetMap() == creature->GetMap());
                }
                continue;
            }
            // Прямую видимость на ВЫБОРЕ не требуем: за 120 ярдов почти всё за
            // чем-нибудь, а игрок обходит препятствие. Её проверяет шаг и само ядро
            // при ударе. Здесь отсеиваем лишь то, что уже признано недостижимым.
            if (c.Refused.count(creature->GetGUID()))
                { ++rejLos; ++rejected; continue; }
            float d = self->GetExactDist2d(creature);
            if (d < bestDist)
            {
                bestDist = d;
                best = creature;
            }
        }
        if (!best && matched && !_rejDiagDone)
        {
            _rejDiagDone = true;
            TC_LOG_INFO("server.worldserver",
                "Constellation REJ {}: видит {}, подходящих {}, чужая фаза {}, занято {}, недопустимо {}, без видимости {}; пример вид {} фракция {}",
                self->GetName(), seen, matched, rejPhase, rejBusy, rejInvalid, rejLos, lastEntry, lastFaction);
        }
        return best;
    }

    // Ближайший квестодатель, У КОТОРОГО ЕСТЬ ЧТО ПРЕДЛОЖИТЬ ИМЕННО ЭТОМУ спутнику.
    // Восклицательный знак над головой — это QuestGiverStatus, который ядро считает
    // для клиента; спрашиваем ровно его, а не таблицу связей.
    Creature* NearestQuestGiver(Player* self) const
    {
        std::list<Creature*> around;
        Trinity::AnyUnitInObjectRangeCheck check(self, Cfg().QuestGiverRange);
        Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, around, check);
        Cell::VisitGridObjects(self, searcher, Cfg().QuestGiverRange);

        Creature* best = nullptr;
        float bestDist = Cfg().QuestGiverRange + 1.0f;
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
    // ЗА КЕМ ИДТИ — ЯВНЫЙ ХОЗЯИН, А НЕ «КТО ПРИДЁТСЯ».
    //
    // Оператор, 2026-08-30: «а кто сейчас лидер??? чую подвох и мину в будущем». Мина
    // была настоящая, и не одна. Прежняя версия шла за ЛИДЕРОМ ГРУППЫ, а лидерство
    // нам не принадлежит и меняется само:
    //
    //   * оператор выходит из группы -> ядро назначает лидером СПУТНИКА, и остальные
    //     идут теперь за ним, а он в этот момент бежит к мобу за сто ярдов;
    //   * спутник стал лидером, а в группе живой игрок -> проверка «ходить за
    //     игроком» бесполезна, она смотрит на лидера, а лидер уже свой;
    //   * состав группы меняется ПОСЛЕ приглашения: позвали спутника, потом позвали
    //     друзей — и спутник в группе с посторонними, никого не приглашав.
    //
    // Поэтому лидерство убрано из уравнения. У спутника есть хозяин — тот, кто его
    // позвал, — и он не меняется от чужих действий. По умолчанию хозяина НЕТ, и тогда
    // спутник не идёт ни за кем: безопасное поведение — стоять, а не брести за
    // случайным лидером.
    Player* FollowTarget(Player* self) const
    {
        Companion const* me = nullptr;
        for (Companion const& c : _companions)
            if (c.Session && c.Session->GetPlayer() == self)
                me = &c;
        if (!me || me->Owner.IsEmpty())
            return nullptr;

        Player* owner = ObjectAccessor::FindConnectedPlayer(me->Owner);
        if (!owner || !owner->IsInWorld() || owner->GetMap() != self->GetMap())
            return nullptr;
        if (!self->GetPhaseShift().CanSee(owner->GetPhaseShift()))   // одна карта — ещё не одно место
            return nullptr;
        // В жизни хозяин — всегда человек (боты не приглашают). На стенде человека
        // нет, поэтому там хозяином может быть спутник — только в RigMode.
        if (IsCompanionAccount(owner->GetSession()->GetAccountId()) && !Cfg().RigMode)
            return nullptr;
        return owner;
    }

    bool IsCompanionAccount(uint32 accountId) const
    {
        for (Companion const& c : _companions)
            if (c.AccountId == accountId)
                return true;
        return false;
    }



    // .constellation follow <имя|off> — назначить хозяина или снять
    bool SetOwner(ChatHandler* handler, std::string const& who)
    {
        Player* master = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        uint32 n = 0;
        bool off = (who == "off" || who == "-");
        for (Companion& c : _companions)
        {
            if (who != "all" && !off && who != c.Entry->Name)
                continue;
            c.Owner = off ? ObjectGuid::Empty : (master ? master->GetGUID() : ObjectGuid::Empty);
            ++n;
        }
        if (off)
            handler->PSendSysMessage("Constellation: {} companions released.", n);
        else
            handler->PSendSysMessage("Constellation: {} companions now follow {}.", n,
                master ? master->GetName() : "nobody");
        return true;
    }

    void Status(ChatHandler* handler)
    {
        uint32 inWorld = 0, failed = 0;
        for (Companion const& c : _companions)
        {
            if (c.State == Stage::InWorld) ++inWorld;
            if (c.State == Stage::Failed)  ++failed;
        }
        handler->PSendSysMessage("Constellation {}: взято {}, боёв {}, СДАНО {}, переходов {}",
            CONSTELLATION_VERSION, _questsTaken, _fightsStarted, _questsTurnedIn, _transitions);
        uint32 idle = 0, following = 0, approaching = 0, attacking = 0;
        for (Companion const& c : _companions)
            switch (c.Mode)
            {
                case Behavior::Idle:              ++idle; break;
                case Behavior::FollowingOwner:    ++following; break;
                case Behavior::ApproachingTarget: ++approaching; break;
                case Behavior::Attacking:         ++attacking; break;
                case Behavior::TurningIn:         break;
            }
        handler->PSendSysMessage("  idle {}, following {}, approaching {}, attacking {}",
            idle, following, approaching, attacking);
        handler->PSendSysMessage("Constellation {}: {} — roster {}, in world {}, failed {}",
            CONSTELLATION_VERSION, Cfg().Enable ? "enabled" : "disabled",
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
        c.BnetEmail = Trinity::StringFormat("{}-R{}@algalon.local", Cfg().Account, uint32(c.Entry->Race));
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
                if (!Cfg().AutoSummon && c.Retries == 0 && c.TicksInState < 2)
                    return false;   // waits for .constellation summon
                if (uint32 cap = Cfg().MaxActive)
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
                    // ЗРЕНИЕ. Player::CanNeverSee отвечает «никогда» про ЛЮБОЙ объект, пока
                    // не выставлен PLAYER_LOCAL_FLAG_OVERRIDE_TRANSPORT_SERVER_TIME —
                    // намеренная задержка ядра: не показывать мир, пока клиент к нему не
                    // готов. Флаг ставится при разборе CMSG_MOVE_INIT_ACTIVE_MOVER_COMPLETE,
                    // который живой клиент шлёт, закончив загрузку. Спутник его не слал
                    // никогда — и видел ровно себя: ни существ, ни других игроков. Отсюда и
                    // ноль ударов: ядро отказывало в атаке по цели, которой для него не было.
                    //
                    // Шлём ПОСЛЕ входа в мир, а не в середине: в середине это ломало вход —
                    // спутники создавались и застревали, ни один не входил.
                    {
                        WorldPacket rawReady(CMSG_MOVE_INIT_ACTIVE_MOVER_COMPLETE);
                        WorldPackets::Movement::MoveInitActiveMoverComplete ready(std::move(rawReady));
                        ready.Ticks = GameTime::GetGameTimeMS();
                        c.Session->HandleMoveInitActiveMoverComplete(ready);
                    }
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
                // ЧЕЛОВЕК УШЁЛ — СПУТНИКИ ВЫХОДЯТ (оператор, 2026-08-30).
                //
                // Причина ухода не важна: вышел сам, вылетел по обрыву, был исключён.
                // Как только в группе не осталось ни одного человека, каждый спутник
                // выходит сам, и группа рассыпается.
                //
                // Путь выбран оператором и он же его проверил: сначала он предложил
                // расформировывать группу одним действием нового лидера, я нашёл в
                // ядре Group::Disband и пометил прямой вызов как исключение — а он
                // ОТКРЫЛ КЛИЕНТ И ТАКОЙ КНОПКИ НЕ НАШЁЛ. Значит и опкода нет, и
                // исключение было бы выдумкой: игрок так не может. Выходим по одному,
                // тем же CMSG_LEAVE_GROUP, что жмёт человек. Их не больше четырёх,
                // и это занимает считаные такты.
                //
                // Нулевой инвариант остаётся без единой прорехи.
                if (Group* grp = player->GetGroup())
                {
                    bool humanInside = false;
                    for (Group::MemberSlot const& slot : grp->GetMemberSlots())
                    {
                        Player* member = ObjectAccessor::FindConnectedPlayer(slot.guid);
                        if (member && member->GetSession()
                            && !IsCompanionAccount(member->GetSession()->GetAccountId()))
                            { humanInside = true; break; }
                    }
                    if (!humanInside)
                    {
                        WorldPacket rawLeave(CMSG_LEAVE_GROUP);
                        WorldPackets::Party::LeaveGroup leave(std::move(rawLeave));
                        c.Session->HandleLeaveGroupOpcode(leave);
                        TC_LOG_INFO("server.worldserver", "Constellation: {} вышел из группы — человека в ней нет",
                            player->GetName());
                        c.Owner.Clear();
                        return false;
                    }
                }

                // ПРАВИЛО ОПЕРАТОРА (2026-08-30): «боты в группы сами не собираются,
                // только если игрок-человек приглашает. Боты только принимают
                // приглашение, не выдают их. И принимают только от игрока-человека.»
                //
                // Отсюда всё остальное: пригласивший человек по умолчанию лидер, он же
                // становится хозяином — и это не меняется ни от смены лидера, ни от
                // того, кого позовут в группу потом. Приглашение от другого спутника
                // отвергается: сцепка ботов между собой не предусмотрена.
                if (Group* inv = player->GetGroupInvite())
                {
                    Player* inviter = ObjectAccessor::FindConnectedPlayer(inv->GetLeaderGUID());
                    if (!inviter || !inviter->GetSession()
                        || IsCompanionAccount(inviter->GetSession()->GetAccountId()))
                        return false;               // не человек — не принимаем
                    c.Owner = inviter->GetGUID();
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
    uint32 _fightsStarted = 0;
    uint32 _transitions = 0;
    uint32 _questsTurnedIn = 0;
    mutable bool _fightDiagDone = false;
    mutable bool _rejDiagDone = false;
    mutable bool _whyDone = false;
    bool _stepDiagDone = false;
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
        TC_LOG_INFO("server.loading", "Constellation {} [{}] - {}", CONSTELLATION_VERSION,
            CONSTELLATION_BUILD_STAMP,
            Constellation::Cfg().Enable ? "enabled" : "present but disabled (Constellation.Enable = 0)");
    }

    // сюда ядро зовёт и при старте, и при `.reload config` — значит ключи
    // по-прежнему меняются без пересборки
    void OnConfigLoad(bool /*reload*/) override
    {
        Constellation::Cfg().Load();
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
