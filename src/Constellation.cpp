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
#include "Loot.h"
#include "LootPackets.h"
#include "ItemTemplate.h"
#include "Item.h"
#include "Bag.h"
#include "ItemPackets.h"
#include "NPCPackets.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "ObjectAccessor.h"
#include "ConditionMgr.h"
#include "MapManager.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PartyPackets.h"
#include "Item.h"
#include "SharedDefines.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellPackets.h"
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
#include <mutex>
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
    bool  TakeQuests      = true;       // БРАТЬ квесты; сдавать разрешает Quests
    bool  Fight           = true;
    bool  Abilities       = false;      // произносить умения, а не только выбирать
    bool  Loot            = false;      // подбирать добычу с собственных убийств
    bool  Vending         = false;      // ходить к торговцу: продать хлам и починиться
    uint32 MaxActive      = 0;
    uint32 PerTick        = 6;
    uint32 MaxQuests      = 10;
    uint32 QuestIntervalMs = 5000;
    float FollowDistance  = 4.0f;
    float FollowMaxRange  = 60.0f;
    float MaxStepUp       = 2.0f;
    float QuestGiverRange = 30.0f;
    float FightRange      = 120.0f;
    float RestBelowPct    = 55.0f;      // ниже этого — отдыхаем
    float ResumeAbovePct  = 85.0f;      // и не встаём, пока не поднимемся сюда
    uint32 RestMaxMs      = 120000;     // но не дольше двух минут
    std::string Account   = "CONSTELLATION";
    std::string DebugGroupPair;

    void Load()
    {
        Enable          = sConfigMgr->GetBoolDefault("Constellation.Enable", false);
        AutoSummon      = sConfigMgr->GetBoolDefault("Constellation.AutoSummon", true);
        RigMode         = sConfigMgr->GetBoolDefault("Constellation.RigMode", false);
        Follow          = sConfigMgr->GetBoolDefault("Constellation.Follow", true);
        Quests          = sConfigMgr->GetBoolDefault("Constellation.Quests", true);
        // Оператор, 2026-08-30: «отключите автоматическое принятие квестов». Отдельный
        // ключ, а не Quests=0: сдавать уже взятое надо продолжать, иначе журналы у 122
        // спутников так и останутся забитыми невыполнимым.
        TakeQuests      = sConfigMgr->GetBoolDefault("Constellation.TakeQuests", true);
        Fight           = sConfigMgr->GetBoolDefault("Constellation.Fight", true);
        // УМЕНИЯ ПО УМОЛЧАНИЮ ТОЛЬКО ВЫБИРАЮТСЯ, НО НЕ ПРОИЗНОСЯТСЯ.
        // Второй читатель трижды показал, что безопасность выбора нельзя доказать
        // свойствами заклинания. Поэтому сперва собираем список того, что модуль ВЫБРАЛ
        // БЫ, — он пишется в журнал, — а произнесение включается этим ключом, когда
        // список прочитан. Включение не требует ни пересборки, ни остановки мира.
        Abilities       = sConfigMgr->GetBoolDefault("Constellation.Abilities", false);
        Loot            = sConfigMgr->GetBoolDefault("Constellation.Loot", false);
        Vending         = sConfigMgr->GetBoolDefault("Constellation.Vending", false);
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
        // ГИСТЕРЕЗИС, А НЕ ОДИН ПОРОГ (Кодекс, разбор плана): входить и выходить по
        // одному числу — значит дёргаться на границе и засорять журнал переходами.
        RestBelowPct    = sConfigMgr->GetFloatDefault("Constellation.RestBelowPct", 55.0f);
        ResumeAbovePct  = sConfigMgr->GetFloatDefault("Constellation.ResumeAbovePct", 85.0f);
        RestMaxMs       = sConfigMgr->GetIntDefault("Constellation.RestMaxMs", 120000);
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

enum class Behavior : uint8 { Idle, Recovering, FollowingOwner, Travelling, ApproachingTarget, Attacking, TurningIn, Vending };

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
    ObjectGuid GiverGuid;               // квестодатель, к которому идём (пусто = никуда)
    uint32 GiverMs = 0;                 // сколько уже идём к нему
    float GiverDist = 0.0f;             // и с какой дистанции начали — меряем прогресс
    std::set<ObjectGuid> GiverUnreachable;  // до кого не дойти: лестницы, помосты, геометрия
    uint32 GiverForgetMs = 0;           // и когда забыть этот список — «навсегда» было ошибкой
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
    ObjectGuid TurnInGuid;              // и кто именно, когда нашёлся
    Position TravelPos;                 // куда идти за целью задания
    uint32 TravelQuest = 0;             // ради какого квеста идём
    std::map<uint32, uint32> TravelBackoff; // квест -> сколько не ходить к нему снова, мс
    uint32 TravelScanMs = 0;            // не перебирать точки чаще раза в две секунды
    uint32 TravelCooldownMs = 0;        // не дойти — не долбиться
    uint8 FightsLogged = 0;             // по три записи с КАЖДОГО, а не 200 с первых
    uint32 RestSkipMs = 0;              // отдых не помог — столько не отдыхаем вовсе
    uint32 ReviveMs = 0;                // как скоро снова пробовать воскреснуть
    uint32 ReviveTries = 0;             // и сколько раз уже пробовали
    bool ReviveGaveUp = false;          // сдались: пишем один раз, потом молчим
    // СЧЁТ УДАРОВ, А НЕ ИСХОДОВ (задача 0022 просила именно это).
    // Мерить исходы бесполезно: ветка победы почти не срабатывала, а «цель на 100 %
    // жизни» одинаково выглядит и когда мы не били, и когда моб ушёл домой и восстановился.
    ObjectGuid FightVictim;             // по кому идёт бой (переживает смерть цели)
    std::string FightVictimName;        // имя — указателя после смерти уже нет
    uint32 FightVictimEntry = 0;
    uint32 GateTicks = 0;               // тактов автомата в бою (НЕ замахов)
    uint32 GateEvading = 0;             //   цель уклоняется — ядро отдаёт EVADE до всего
    uint32 GateBusy = 0;                //   мы читаем/несёмся — ядро выходит раньше вентиля
    uint32 GateNoState = 0;             //   ядро не считает нас атакующими
    uint32 GateOutOfRange = 0;          //   вне досягаемости
    uint32 GateBadFacing = 0;           //   вне сектора 120°
    uint32 CastMs = 0;                  // когда в последний раз решали про заклинание
    uint32 CastsTried = 0;              // за этот бой: попыток произнести
    uint32 CastsWent = 0;               //               и сколько ушло (по следу в ядре)
    uint32 LastSpell = 0;               // что именно произносили — иначе выбор не проверить
    float EngageRange = 0.0f;           // с какой дистанции драться: 0 = ещё не считали
    ObjectGuid ApproachFor;             // для кого посчитана точка подхода
    float ApproachX = 0.0f, ApproachY = 0.0f, ApproachZ = 0.0f;
    uint32 ApproachMs = 0;              // и когда пересчитать: цель могла отойти
    ObjectGuid VendorGuid;              // торговец, к которому идём
    uint32 VendCooldownMs = 0;          // не искать торговца каждый такт, если не нашли
    uint32 VendScanMs = 0;              // и не обходить сетку каждый такт ВООБЩЕ
    uint32 VendSold = 0;                // за всё время: продано предметов
    uint64 VendEarned = 0;              //               выручено медяков (64 бита: 122 бота)
    uint32 VendRepaired = 0;            //               починок
    uint32 VendNoVendor = 0;            //               некому продать поблизости
    uint32 VendPoor = 0;                //               не хватило денег на ремонт
    ObjectGuid LootTarget;              // труп нашего убийства, который ещё не обобран
    uint32 LootOpened = 0;              // за всё время: открыли трупов
    uint32 LootItems = 0;               //               взяли предметов
    uint32 LootMoney = 0;               //               взяли денег (в медяках)
    uint32 LootTooFar = 0;              //               не дотянулись — мера нужды в ходьбе
    uint32 LootDenied = 0;              //               ядро не дало (чужой лут, розыгрыш)
    uint32 CastsBusy = 0;               // не просили: уже читаем или не истёк общий откат
    uint32 CastsDiedUnder = 0;          // цель умерла, ПОКА мы читали — догадка оператора
    bool WasCasting = false;            // читали ли на прошлом такте (для счётчика выше)
    std::set<uint32> SpellsLogged;      // о каком выборе уже написали — по разу за всё время
    bool PaletteDumped = false;         // палитра класса выписана — один раз на спутника
    uint32 WantedCheckMs = 0;           // когда в последний раз спрашивали счётчик цели
    uint32 GateNotReady = 0;            //   вентиль открыт, но таймер удара не готов
    uint32 VictimSwaps = 0;             //   сколько раз цель подменилась
    uint32 SwingsAtStart = 0;           // отсечки на входе в бой
    uint64 DealtAtStart = 0;
    uint32 LandedAtStart = 0;
    uint32 ZeroedAtStart = 0;
    uint32 HitsAtStart = 0;
    uint64 TakenAtStart = 0;
    uint32 KillsAtStart = 0;
    ObjectGuid DamageVictim;            // по кому ведём счёт урона
    uint64 VictimHp = 0;                // НАШ накопленный урон на прошлой проверке
    uint32 NoDamageMs = 0;              // сколько бьём без всякого следа
    uint32 EnderScanMs = 0;             // когда искать заново
    uint32 IdleScanMs = 0;              // «стою» не перебирает мир на каждом такте
    bool FightDiagDone = false;         // диагностика боевого поиска — по разу на КАЖДОГО
    Position TurnInPos;                 // и где он стоит
    // ОТСРОЧКА У КАЖДОГО КВЕСТА СВОЯ. Был один таймер на спутника и общий набор:
    // любая новая неудача переписывала таймер, а по его истечении набор очищался
    // ЦЕЛИКОМ. То есть минутная неудача укорачивала чужую пятиминутную, и все
    // отложенные квесты оживали разом (Кодекс, проход 4).
    std::map<uint32, uint32> TurnInBackoff;   // квест -> сколько ещё ждать, мс
    std::set<uint32> Impossible;        // квесты, которые закрыть НЕЧЕМ (навсегда)
    ObjectGuid Owner;                   // кто позвал; пусто = не идти ни за кем
    std::set<ObjectGuid> Refused;       // цели, до которых не дойти или не ударить
    bool OwnerFromGroup = false;        // хозяин держится на ГРУППЕ, а не на памяти
    float LastX = 0.0f, LastY = 0.0f;   // где мы были — чтобы заметить, что не идём
    uint32 StuckMs = 0;                 // сколько стоим, хотя собирались идти
    uint32 UnstickTries = 0;            // сколько раз отступали вбок подряд
    uint32 UnstickTotal = 0;            // и сколько всего за это намерение (не сбросить движением)
    bool BrokenNoted = false;           // о сломанном снаряжении сказано один раз, не в каждый такт
    bool JumpProbed = false;            // самопроверка прыжка на стенде уже сделана
    uint8 JumpsLeft = 3;                // прыжков в запасе
    uint32 JumpCooldownMs = 0;          // истратил три — минуту без прыжков
    bool UnstickLeft = true;            // в какую сторону отступать следующей
    uint32 NoPathMs = 0;                // сколько ещё не трогать построитель маршрута
    uint8 NoPathFails = 0;              // подряд идущих отказов — отступ растёт с ними
    bool RawTarget = false;             // боковая точка не далась — идём на самого NPC
    bool Stalled = false;               // отступать больше некуда — решает автомат
    uint32 FollowCooldownMs = 0;        // не дёргаться к хозяину, до которого не дойти
};

class Manager
{
public:
    static Manager* Instance()
    {
        static Manager instance;
        return &instance;
    }

    // УДАРЫ, СЧИТАННЫЕ САМИМ ЯДРОМ.
    //
    // ЗАМОК ЗДЕСЬ ОБЯЗАТЕЛЕН, И ЭТО НЕ ПЕРЕСТРАХОВКА. На боевом MapUpdate.Threads = 6:
    // Unit::DealDamage выполняется на потоке КАРТЫ, а автомат поведения читает те же
    // счётчики с потока МИРА (WorldScript::OnUpdate). Незащищённая вставка в
    // unordered_map с шести потоков — гонка и падение процесса (Кодекс, проход 10).
    // Наружу отдаётся КОПИЯ, а не ссылка внутрь таблицы.
    //
    // Запись заводится ОДИН раз, при первом бое спутника (Register), поэтому обработчик
    // урона делает только find(): чужие игроки и персонажи оператора в таблицу не
    // попадают и не считаются.
    struct Blows
    {
        uint32 Swings = 0;              // ЗАМАХИ: ModifyMeleeDamage зовётся ДО броска
                                        // исхода (Unit.cpp:1377 против 1388), поэтому
                                        // сюда попадают и промах, и уклонение цели
        // ВНИМАНИЕ ПРИ ЧТЕНИИ: это НЕ «из замахов дошло». OnDamage зовётся на любой
        // наш урон — заклинание, периодический эффект, что угодно. У этих спутников
        // другого источника пока нет (в том и суть задачи 0022), поэтому на практике
        // числа сравнимы, но называть разницу «промахи» нельзя (Кодекс, проход 11).
        uint32 Landed = 0;              // событий НАШЕГО урона, любого происхождения
        uint32 Zeroed = 0;              // НЕ подмножество Landed, а вторая, взаимоисключающая
                                        // корзина: событие нашего урона на НОЛЬ
        uint64 Dealt = 0;               // и на сколько всего
        uint32 Hits = 0;                // сколько раз попали по нам
        uint64 Taken = 0;
        uint32 Kills = 0;               // АТРИБУЦИЯ ЯДРА: OnCreatureKill(мы, кто-то)
        ObjectGuid LastKilled;          // и КОГО именно — без этого победа не адресная
    };

    Blows BlowsOf(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(_blowsLock);
        auto it = _blows.find(guid);
        return it != _blows.end() ? it->second : Blows();
    }

    // ЗАВЕСТИ И СНЯТЬ ОТСЧЁТ — ОДНОЙ ОПЕРАЦИЕЙ.
    // Двумя блокировками между ними успевал проскочить замах с потока карты: он попадал
    // и в отсчёт, и мимо разницы (Кодекс, проход 11).
    Blows RegisterAndSnapshot(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(_blowsLock);
        return _blows.emplace(guid, Blows()).first->second;
    }

    void NoteSwing(Unit* attacker)
    {
        if (!attacker || !attacker->IsPlayer())
            return;                     // замок берём только ради игрока
        std::lock_guard<std::mutex> lock(_blowsLock);
        auto it = _blows.find(attacker->GetGUID());
        if (it != _blows.end())
            ++it->second.Swings;
    }

    void NoteDamage(Unit* attacker, Unit* victim, uint32 damage)
    {
        // СНАЧАЛА ОТСЕИВАЕМ, ПОТОМ БЕРЁМ ЗАМОК. Событий урона существо-против-существа
        // на боевом на порядки больше, чем наших, и сериализовать на них шесть потоков
        // карт незачем (Кодекс, проход 11).
        bool const mine = (attacker && attacker->IsPlayer()) || (victim && victim->IsPlayer());
        if (!mine)
            return;
        std::lock_guard<std::mutex> lock(_blowsLock);
        if (attacker && attacker->IsPlayer())
        {
            auto it = _blows.find(attacker->GetGUID());
            if (it != _blows.end())
            {
                // ЧТО ЭТО ЧИСЛО ДОКАЗЫВАЕТ, И ЧТО НЕТ.
                //
                // Доказывает ровно одно: было исходящее событие урона, и урон в нём НОЛЬ.
                // Не доказывает ни что это был автоудар (OnDamage зовётся на любой наш
                // урон), ни что виновата броня. Заведено потому, что прежде такие события
                // молча отбрасывались, и разница «замахи минус дошло» читалась как промахи:
                // на стенде с целым снаряжением она давала 24 %, на боевом со сломанным —
                // 52 %, и вторую цифру нечем было объяснить. Теперь обе корзины видны, и
                // объяснение будет взято из них, а не из рассуждения.
                if (!damage)
                    ++it->second.Zeroed;
                else
                {
                    ++it->second.Landed;
                    it->second.Dealt += damage;
                }
            }
        }
        if (victim && victim->IsPlayer() && damage)
        {
            auto it = _blows.find(victim->GetGUID());
            if (it != _blows.end())
            {
                ++it->second.Hits;
                it->second.Taken += damage;
            }
        }
    }

    // КОГО убили — обязательная часть. Считать «убил хоть кого-нибудь за время боя»
    // мало: спутник мог добить чужой цели или своего добить раньше, а нынешнюю цель
    // положил сосед — и бой засчитался бы победой (Кодекс, проход 11).
    void NoteKill(Player* killer, Creature* killed)
    {
        if (!killer || !killed)
            return;
        std::lock_guard<std::mutex> lock(_blowsLock);
        auto it = _blows.find(killer->GetGUID());
        if (it != _blows.end())
        {
            ++it->second.Kills;
            it->second.LastKilled = killed->GetGUID();
        }
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

    // РАЗОВАЯ УБОРКА ПОСЛЕДСТВИЙ ДЕФЕКТА, А НЕ ОБХОД ПРАВИЛ.
    //
    // Снаряжение сломал не игровой процесс, а ошибка модуля: спутники сотнями гибли в боях,
    // которые не могли выиграть. Мастер игры, чинящий вещи игрокам, — обычное действие; бот,
    // чинящий себя сам из воздуха, — нет. Поэтому это КОМАНДА, которую отдают руками, а
    // штатный путь остаётся клиентским: дойти до NPC с UNIT_NPC_FLAG_REPAIR и послать
    // CMSG_REPAIR_ITEM с пустым ItemGUID, за деньги (задача 0010).
    // ВНИМАНИЕ: PSendSysMessage форматирует в стиле printf (%u, %s), а TC_LOG_INFO — в
    // стиле fmt ({}). Они стоят рядом в одном файле, и весь набор команд девять вызовов
    // подряд печатал «{}» буквально, включая `.constellation status`. Нашлось только когда
    // новая строка про ремонт вывела «repaired {} companions».
    // ВАЙП: СТЕРЕТЬ СОСТАВ ДО НОВОРОЖДЁННОГО И ДАТЬ КОНВЕЙЕРУ СОЗДАТЬ ЗАНОВО.
    //
    // Ни одной своей строки SQL: удаляет ядро, создаёт наш же конвейер. Так и стартовые
    // вещи, и сумки, и панель команд, и здоровье оказываются правильными по построению —
    // а не потому что я угадал шестьдесят таблиц.
    //
    // ТРИ ЗАМКА, потому что это боевой сервер и отменить можно только из резервной копии:
    //   1. без слова подтверждения команда только ПОКАЗЫВАЕТ, что сделает;
    //   2. отказ, если в мире есть живой игрок — не наш спутник;
    //   3. чужие персонажи не трогаются вовсе: список берётся из состава модуля.
    bool WipeAll(ChatHandler* handler, std::string const& confirm)
    {
        // ДВА СЛОВА ПОДТВЕРЖДЕНИЯ, И ЭТО НЕ ЛЕНЬ ВЫБРАТЬ ОДНО: консоль на этой машине
        // ru-RU, и кириллица через ssh -> cmd -> оболочку уже ломалась не раз. ASCII-слово
        // это гарантированный путь, русское — удобный.
        bool const confirmed = (confirm == "СТЕРЕТЬ" || confirm == "WIPE");

        // ЖИВОЙ ИГРОК РЯДОМ — ОТКАЗ. Тем же вопросом, каким модуль отличает своих в группе.
        uint32 strangers = 0;
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        {
            if (!player || !player->GetSession())
                continue;
            if (!IsCompanionAccount(player->GetSession()->GetAccountId()))
                ++strangers;
        }

        uint32 total = 0, withChar = 0;
        for (Companion const& c : _companions)
        {
            ++total;
            if (!c.Guid.IsEmpty())
                ++withChar;
        }

        if (!confirmed)
        {
            handler->PSendSysMessage(
                "Constellation WIPE: would delete %u companion characters of %u and let the "
                "pipeline recreate them from scratch (same name/race/class/gender/account, NEW guid).",
                withChar, total);
            handler->PSendSysMessage(
                "Constellation WIPE: nothing done. Repeat with the confirmation word to proceed.");
            if (strangers)
                handler->PSendSysMessage(
                    "Constellation WIPE: %u non-companion player(s) online — the command would refuse.",
                    strangers);
            return true;
        }

        if (strangers)
        {
            handler->PSendSysMessage(
                "Constellation WIPE: REFUSED — %u non-companion player(s) online. "
                "Wipe only on an empty realm.", strangers);
            return false;
        }

        // СНАЧАЛА ВЫВЕСТИ ИЗ МИРА, ПОТОМ УДАЛЯТЬ. Живой Player держит состояние в памяти, и
        // выход записал бы его обратно поверх удаления (Кодекс: WorldSession.cpp:635).
        uint32 deleted = 0, failed = 0;
        for (Companion& c : _companions)
        {
            if (!c.Guid)
                continue;

            ObjectGuid const guid = c.Guid;
            uint32 const account = c.AccountId;

            // СЕССИЮ НАДО СНЕСТИ ЦЕЛИКОМ, А НЕ ПРОСТО ВЫЙТИ ИЗ МИРА.
            //
            // Первая версия звала LogoutPlayer и оставляла c.Session — и конвейер вставал
            // намертво: предохранитель MaxActive считает живыми тех, у кого сессия есть,
            // так что восемь осиротевших сессий держали счётчик на потолке и создание не
            // начиналось никогда. То же самое делает роспуск, и по той же причине.
            DropSession(c);

            // УДАЛЯЕТ ЯДРО, А НЕ Я. deleteFinally = true: стереть насовсем, а не пометить.
            Player::DeleteFromDB(guid, account, true, true);
            ++deleted;

            // Конвейер создаёт заново ровно при пустом Guid (BehaveTick, Stage::Offline).
            c.Guid.Clear();
            c.State = Stage::Offline;
            c.TicksInState = 0;
            c.Retries = 0;
            c.PaletteDumped = false;
            c.QuestRefused.clear();
            TC_LOG_INFO("server.worldserver",
                "Constellation WIPE: удалён персонаж {} (учётка {}), будет создан заново",
                guid.ToString(), account);
        }

        handler->PSendSysMessage(
            "Constellation WIPE: deleted %u characters (%u failed); the pipeline recreates them "
            "on the following ticks. Watch .constellation status.", deleted, failed);
        TC_LOG_INFO("server.worldserver", "Constellation WIPE: стёрто {} персонажей состава", deleted);
        return true;
    }

    // ОТПРАВИТЬ ВЕСЬ СОСТАВ К ТОРГОВЦУ ПРЯМО СЕЙЧАС.
    //
    // Обычное условие похода — поломка или полные сумки — в коротком прогоне не наступает,
    // поэтому проверить торговлю можно только так. Ищем ближайшего каждому спутнику: они
    // стоят в разных местах, и один общий торговец был бы неправдой.
    bool VendAll(ChatHandler* handler)
    {
        uint32 sent = 0, nobody = 0;
        for (Companion& c : _companions)
        {
            if (c.State != Stage::InWorld || !c.Session)
                continue;
            Player* self = c.Session->GetPlayer();
            if (!self || !self->IsInWorld())
                continue;
            // Команда оператора — идём за обеими услугами сразу, но не требуем ни одной:
            // это ручная проверка, а не автоматика, и отказ «никого нет» тут информативнее.
            Creature* vendor = FindVendorNear(self, false, false);
            if (!vendor)
                { ++nobody; continue; }
            c.VendorGuid = vendor->GetGUID();
            c.VendCooldownMs = 0;
            Switch(c, self, Behavior::Vending, "команда оператора");
            ++sent;
        }
        handler->PSendSysMessage(
            "Constellation: sent %u companions to a vendor; %u found nobody within 100 yards",
            sent, nobody);
        return true;
    }

    bool RepairAll(ChatHandler* handler)
    {
        // ОДНА ДОРОГА К ОДНОМУ ДЕЙСТВИЮ: та же RepairIfBroken, что чинит при входе в мир и
        // после смерти. Команда осталась ради разовой уборки и ради проверки руками.
        // Оговорка, которую надо назвать: DurabilityRepairAll обходит не только надетое, но
        // и рюкзак, сами сумки и их содержимое (Player.cpp:4611-4625) — отбираем спутников
        // по сломанному НАДЕТОМУ, а чиним у них всё.
        uint32 touched = 0, items = 0;
        for (Companion& c : _companions)
        {
            if (c.State != Stage::InWorld || !c.Session)
                continue;
            if (uint32 broken = RepairIfBroken(c.Session->GetPlayer(), "команда", /*operatorAsked=*/true))
            {
                c.BrokenNoted = false;
                ++touched;
                items += broken;
            }
        }
        handler->PSendSysMessage(
            "Constellation: repaired %u companions (%u broken equipped items counted; bags repaired too)",
            touched, items);
        return true;
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
        handler->PSendSysMessage("Constellation: %u companions queued (auto pipeline).", woken);
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
        handler->PSendSysMessage("Constellation: %u dismissed, %u deferred (mid-login).", dropped, deferred);
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
        if (pair.empty() || pair == "none" || _debugPairDone)
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
    // ПРЫЖОК — И ЧЕСТНО О ТОМ, ЧТО ЭТО ТАКОЕ.
    //
    // Оператор, 2026-08-30: «помимо шага в сторону добавь им 2-3 прыжка». Сделано, и
    // теми самыми опкодами, которыми прыгает клиент: CMSG_MOVE_JUMP и закрывающий его
    // CMSG_MOVE_FALL_LAND, оба через тот же HandleMovementOpcode, которым модуль уже
    // шлёт шаги. Нулевой инвариант цел.
    //
    // НО НЕ НАДО ДУМАТЬ, ЧТО СПУТНИК ПЕРЕЛЕТАЕТ ПРЕПЯТСТВИЕ. Сервер дугу не считает —
    // её считает клиент, а сервер принимает то, что ему сообщили. По существу это
    // ПЕРЕНОС на два ярда вперёд, ограниченный лучом ядра, плюс пересадка на законную
    // высоту. Кодекс сказал это прямо — «косметический прыжок вокруг маленького
    // переноса», — и он прав. Польза от него всё же настоящая, и ровно двойная:
    //   * низкий порог, который луч проходит, а маршрутизатор обойти не берётся;
    //   * спутник, чей Z уполз в геометрию: посадка сажает его на законную высоту, и
    //     полигон под ним снова находится.
    // Через стену он не переносит. Для стены есть шаг вбок, он идёт следующим.
    //
    // ПРЫЖОК И ПОСАДКА УХОДЯТ В ОДНОМ ТАКТЕ, и это не небрежность. Растянуть их на
    // 400 мс значило бы завести состояние «в воздухе», из которого есть выходы БЕЗ
    // посадки: смена управляющего, смерть, выключение модуля, телепорт между картами.
    // Каждый оставил бы спутника вечно падающим — а падающему наш же заслон в
    // StepToward запрещает идти. То есть ровно тот вечный тупик, который мы и чиним.
    // Кодекс нашёл четыре таких выхода; вместо того чтобы затыкать каждый, состояния
    // просто нет.
    bool Jump(Companion& c, Player* self, float tx, float ty)
    {
        float ang = self->GetAbsoluteAngle(tx, ty);
        Position land = self->GetFirstCollisionPosition(2.0f, ang - self->GetOrientation());

        // ВЫСОТУ ПОСАДКИ НЕ ПЕРЕСЧИТЫВАТЬ. Ядро уже посадило её само: последним делом
        // MovePositionToFirstCollision вызывает UpdateAllowedPositionZ, который знает
        // про полы и здания. Здесь стоял мой GetHeight поверх — то есть ровно тот брак,
        // с которого начался этот день: внутри аббатства он вернул бы землю ПОД домом и
        // посадил спутника под полом. Нашли оба, независимо: и я, и Кодекс.
        //
        // Обрыв, мост, этаж ниже: два ярда по горизонтали не гарантируют малого перепада
        // по высоте (Кодекс). Вниз больше чем на три ярда не прыгаем вовсе — и урона от
        // падения тогда быть не может, ядро начисляет его от 14.57.
        if (self->GetPositionZ() - land.GetPositionZ() > 3.0f)
            return false;

        MovementInfo up;
        up.guid = self->GetGUID();
        up.pos.Relocate(self->GetPosition());
        up.pos.SetOrientation(ang);
        up.flags = MOVEMENTFLAG_FALLING;
        up.time = GameTime::GetGameTimeMS();
        up.jump.zspeed = 7.955800f;         // скорость прыжка клиента
        up.jump.cosAngle = std::cos(ang);
        up.jump.sinAngle = std::sin(ang);
        up.jump.xyspeed = self->GetSpeed(MOVE_RUN);
        up.jump.fallTime = 0;
        c.Session->HandleMovementOpcode(CMSG_MOVE_JUMP, up);

        MovementInfo down;
        down.guid = self->GetGUID();
        down.pos.Relocate(land);
        down.pos.SetOrientation(ang);
        down.flags = 0;
        down.time = GameTime::GetGameTimeMS();
        down.jump.fallTime = 400;
        c.Session->HandleMovementOpcode(CMSG_MOVE_FALL_LAND, down);

        c.Moving = false;
        c.Waypoints.clear();                // маршрут пересчитаем с нового места
        return true;
    }

    // УПЁРЛИСЬ — ОТСТУПАЕМ ВДОЛЬ ПРЕПЯТСТВИЯ, КАК ЖИВОЙ.
    //
    // Оператор, 2026-08-30: «люди вперлись в препятствие, вы не учили ботов обходить
    // препятствия?? только честно а не сквозь текстуры». Честно: обходить учили —
    // маршрут строит PathGenerator по навигационной сетке ядра, тот же, которым ядро
    // водит существ. Чего НЕ было — выхода из положения, когда сетка маршрута не даёт:
    // код возвращал false, и спутник стоял у стены вечно.
    //
    // Живой игрок в этом месте не проходит сквозь стену — он смещается ВДОЛЬ неё и
    // пробует снова. Так и делаем, и шаг вбок считает само ядро своим лучом
    // GetFirstCollisionPosition: луч УПИРАЕТСЯ в стену, а не проходит её. Точка вбок
    // становится обычной путевой точкой, к ней идём бегом теми же пакетами. Ни одной
    // прорехи в нулевом инварианте и ни одного прохода сквозь текстуры.
    bool Unstick(Companion& c, Player* self, float tx, float ty)
    {
        // ЛЕСТНИЦА, А НЕ ОДНО СРЕДСТВО: сперва прыгнуть, и только если три прыжка не
        // сняли с места — уходить вбок. Порядок именно такой, потому что прыжок дешевле
        // (400 мс на месте против обхода в шесть ярдов) и чаще срабатывает.
        // ПОТОЛОК ПРЫЖКОВ — НАСТОЯЩИЙ, А НЕ «ТРИ НА КАЖДУЮ СМЕНУ НАМЕРЕНИЯ». Запас
        // пополнялся в Switch(), а спутник может колебаться «иду за хозяином -> стою ->
        // иду за хозяином» бесконечно и прыгать вечно (Кодекс). Поэтому истраченный
        // запас закрывает прыжки на минуту, и смена намерения этого не обходит.
        if (c.JumpsLeft && Jump(c, self, tx, ty))
        {
            // ОКНО ОТКРЫВАЕТ ПЕРВЫЙ ПРЫЖОК, а не третий. Прежде запас пополнялся в
            // Switch() при каждой смене намерения, и спутник, тративший по одному-два
            // прыжка и меняющий намерение, получал их снова без конца — потолок,
            // обещанный в комментарии, не был написан в коде (Кодекс, проход 4).
            // Теперь это честное «три прыжка в минуту», и смена намерения тут ни при чём.
            if (!c.JumpCooldownMs)
                c.JumpCooldownMs = 60000;
            --c.JumpsLeft;
            return true;                    // прыжок НЕ тратит запас отступов
        }

        // ДВА ПРЕДЕЛА, А НЕ ОДИН (Кодекс, 2026-08-30). Первый — попытки подряд; но его
        // обнуляет сам удавшийся отступ, ведь спутник при этом ДВИГАЛСЯ. Поэтому второй,
        // общий за намерение, движением не сбрасывается — только сменой намерения.
        if (++c.UnstickTries > 4 || ++c.UnstickTotal > 8)
            return false;                   // хватит топтаться — пусть решает автомат

        float toGoal = self->GetAbsoluteAngle(tx, ty);
        float side = toGoal + (c.UnstickLeft ? float(M_PI) / 2.0f : -float(M_PI) / 2.0f);
        c.UnstickLeft = !c.UnstickLeft;     // попеременно, чтобы не тереться об угол

        Position hop = self->GetFirstCollisionPosition(6.0f, side - self->GetOrientation());
        if (self->GetExactDist2d(hop.GetPositionX(), hop.GetPositionY()) < 1.5f)
            return true;                    // и вбок стена — на следующем такте другая сторона

        c.Waypoints.clear();
        c.Waypoints.push_back(hop);
        c.WaypointIndex = 0;
        c.PathTargetX = tx;                 // цель прежняя: пересчёт пойдёт с нового места
        c.PathTargetY = ty;
        return true;
    }

    // ОСТАНОВКУ НАДО ПОСЫЛАТЬ, А НЕ ПРОСТО ПЕРЕСТАТЬ ИДТИ.
    //
    // Оператор, 2026-08-30: «бриенная бежит на месте» — и на снимке она стоит у
    // Маршала Макбрайда, отыграв анимацию бега. Причина ровно эта: шаг уходит пакетом
    // с флагом «иду вперёд», а пакет с нулевыми флагами посылался ИЗ ОДНОГО-
    // ЕДИНСТВЕННОГО выхода — «дошёл». Все прочие (маршрут пройден, шаг нулевой, запрет
    // по флагам движения, застряли, маршрута нет) возвращали «не иду» МОЛЧА, и для
    // всех остальных клиентов последним словом сервера оставалось «бежит».
    //
    // У спутника своего клиента нет, поэтому увидеть это мог только живой игрок рядом —
    // отчего оно и дожило до боевого.
    void StopMoving(Companion& c, Player* self)
    {
        if (!c.Moving)
            return;
        SendMove(c, self, self->GetPosition(), 0);
        c.Moving = false;
    }

    bool StepToward(Companion& c, Player* self, float tx, float ty, float tz, float stopAt, float dt)
    {
        // РАССТОЯНИЕ ЗДЕСЬ — В ПРОСТРАНСТВЕ, А НЕ ПО ПЛОСКОСТИ.
        //
        // Было по плоскости, и это тихо ломало ВСЁ, что опирается на «дошёл»: цель на
        // помосте девятью ярдами выше числилась достигнутой, движение прекращалось, а
        // ядро — которое меряет в пространстве — отказывало. Спутник не застревал, он
        // считал себя пришедшим. Кодекс назвал это мёртвой зоной, и она общая для
        // следования, подхода, боя и сдачи, потому что мерка тут одна на всех.
        float dist = self->GetExactDist(tx, ty, tz);
        if (dist < stopAt)
        {
            if (c.Moving)
            {
                SendMove(c, self, self->GetPosition(), 0);
                c.Moving = false;
            }
            c.StuckMs = 0;
            c.UnstickTries = 0;
            c.NoPathFails = 0;
            c.NoPathMs = 0;
            c.RawTarget = false;        // дошли — в следующий раз снова вежливо, сбоку
            c.Stalled = false;          // дошли — тупика больше нет (Кодекс)
            return false;
        }

        // ЗАСТРЯЛИ — СУДИМ ПО ФАКТУ, А НЕ ПО НАМЕРЕНИЮ. Маршрут может существовать, а
        // спутник всё равно тереться об угол: значит смотрим, СДВИНУЛСЯ ли он на самом деле.
        if (self->GetExactDist2d(c.LastX, c.LastY) > 1.0f)
        {
            c.LastX = self->GetPositionX();
            c.LastY = self->GetPositionY();
            c.StuckMs = 0;
            c.UnstickTries = 0;
        }
        else
            c.StuckMs += uint32(dt * 1000.0f);

        static uint32 const FORBIDDEN = MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FALLING
            | MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_ROOT;
        if ((self->GetUnitMovementFlags() & FORBIDDEN) || self->GetTransport()
            || self->IsInWater() || self->IsFalling() || self->IsFlying())
        {
            StopMoving(c, self);
            return false;               // не наш случай; выручит срок состояния
        }

        if (c.StuckMs > 2500)
        {
            c.StuckMs = 0;
            if (!Unstick(c, self, tx, ty))
                c.Stalled = true;
            StopMoving(c, self);
            return false;
        }

        float step = std::min(self->GetSpeed(MOVE_RUN) * dt, dist - stopAt * 0.5f);
        if (step <= 0.0f)
        {
            StopMoving(c, self);
            return false;
        }

        // маршрут пересчитывается не каждый шаг: он нужен, только пока мы далеко
        // от следующей его точки
        if (c.Waypoints.empty() || c.WaypointIndex >= c.Waypoints.size()
            || self->GetExactDist2d(c.PathTargetX, c.PathTargetY) > 5.0f
                && (std::fabs(c.PathTargetX - tx) > 3.0f || std::fabs(c.PathTargetY - ty) > 3.0f))
        {
            // ВЫСОТА ЦЕЛИ — НАСТОЯЩАЯ, А НЕ ВЫЧИСЛЕННАЯ ЗАНОВО.
            //
            // Здесь стояло GetHeight(tx, ty, z + 5): переданный мне Z выбрасывался и
            // угадывался обратно по карте. Под зданием и на ступенях угадывалось МИМО —
            // возвращалась высота ЗЕМЛИ ПОД домом, точка уезжала внутрь геометрии, где
            // полигона нет, и построитель отвечал NOPATH (в живом журнале — «тип A»).
            // Спутник упирался в угол аббатства и стоял. Настоящий Z известен КАЖДОМУ
            // месту вызова — берём его.
            // ПРОВАЛ ПОСТРОИТЕЛЯ ОБЯЗАН ОТСТУПАТЬ, А НЕ ДОЛБИТЬСЯ.
            //
            // Вот из-за чего встал ВЕСЬ состав и мир упёрся в ядро. Список точек ниже
            // заполняется только при УСПЕХЕ; при отказе он остаётся ПУСТЫМ — а условие
            // пересчёта прямо над этим срабатывает как раз на пустом списке. Значит
            // неудачный поиск повторялся КАЖДЫЙ такт, четыре раза в секунду, бесконечно.
            //
            // Неудачный поиск по навигационной сетке — самый дорогой запрос из всех:
            // Detour обходит ВСЮ достижимую сетку, прежде чем ответить «нет». Успешный
            // останавливается, найдя цель. Полсотни спутников в отказе растянули такт
            // мира до секунд, после чего перестали двигаться и те, у кого маршрут был:
            // замер показал 0 сдвинувшихся из 122 и одну строку журнала за десять секунд.
            //
            // Отступ нарастающий — 3, 6, 12, 24 секунды, дальше 24. Цель за это время
            // никуда не убежит, а такт освобождается. Тот, кто так и не дойдёт, будет
            // отмечен «не дойти» по общему правилу и займётся другим делом.
            if (c.NoPathMs > 0)
            {
                uint32 const backoff = uint32(dt * 1000.0f);
                c.NoPathMs = (c.NoPathMs <= backoff) ? 0 : c.NoPathMs - backoff;
                StopMoving(c, self);
                return false;
            }
            // ХОДИТЬ ДАЛЕКО НАДО ПРЫЖКАМИ, А НЕ ОДНИМ ВОПРОСОМ.
            //
            // Замер на боевом 2026-09-01, после того как пробный шаг на пять ярдов удался
            // у 15 спутников из 18: сетка ПОД НОГАМИ в порядке, беда в конце пути. А концы
            // эти — настоящие места принимающих, сверенные с базой: Горнек -598 -4248 39,
            // Лантан Перилон 10302 -6229 26.7. Оба конца законные, и всё равно тип A.
            //
            // Расстояния при отказах: 241, 244, 271, 300 ярдов. Записи по Легиону называют
            // потолок сглаженного пути в 296 ярдов и решают это отдельной процедурой
            // «идти далеко»: если весь путь не строится, просить путь до ПРОМЕЖУТОЧНОЙ
            // точки, половиня досягаемость, пока сетка не ответит. Здесь этой процедуры не
            // было — я просил весь путь одним вызовом и сдавался.
            //
            // Вторая группа отказов той же природы: Фелендрен Изгнанный стоит на ВЕРШИНЕ
            // Академии Фалтриена (z=110), спутники — у подножия (z=25). Целиком такой путь
            // не строится, а до подножия башни — строится.
            //
            // Высоту промежуточной точки берём ОТ СВОЕГО ЯРУСА вниз, а не от неба: та же
            // ошибка Тельдрассила, что уже стоила Легиону 121 тупика.
            PathGenerator path(self);
            PathGenerator hop(self);
            bool built = path.CalculatePath(tx, ty, tz, false)
                && !(path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_SHORTCUT));
            Movement::PointsArray const* pts = built ? &path.GetPath() : nullptr;
            float aimX = tx, aimY = ty;
            if (!built)
            {
                float const whole = self->GetExactDist2d(tx, ty);
                float const ang = self->GetAbsoluteAngle(tx, ty);
                for (float reach = std::min(whole * 0.5f, 150.0f); reach >= 15.0f; reach *= 0.5f)
                {
                    float const hx = self->GetPositionX() + std::cos(ang) * reach;
                    float const hy = self->GetPositionY() + std::sin(ang) * reach;
                    if (!MapManager::IsValidMapCoord(self->GetMapId(), hx, hy))
                        continue;
                    float hz = self->GetMap()->GetHeight(self->GetPhaseShift(), hx, hy,
                                                         self->GetPositionZ() + 5.0f);
                    if (hz <= INVALID_HEIGHT)
                        continue;
                    if (hop.CalculatePath(hx, hy, hz, false)
                        && !(hop.GetPathType() & (PATHFIND_NOPATH | PATHFIND_SHORTCUT)))
                    {
                        built = true;
                        pts = &hop.GetPath();
                        aimX = hx;
                        aimY = hy;
                        ++_hops;
                        break;
                    }
                }
            }
            if (!built)
            {
                ++_noPath;
                if (_noPathLogged < 20)
                {
                    // прежняя диагностика стояла на ОДНОМ глобальном флаге и напечаталась
                    // за всю жизнь сервера ровно один раз — то есть скрыла масштаб беды
                    ++_noPathLogged;
                    // КООРДИНАТЫ ОБОИХ КОНЦОВ, ИНАЧЕ ОТКАЗ НЕРАЗЛИЧИМ.
                    //
                    // Тип A — это NOPATH|SHORTCUT: Detour не нашёл полигона под НАЧАЛОМ
                    // либо под КОНЦОМ пути и вернул прямую из двух точек. Это две разные
                    // болезни с разным лечением, а прежняя строка печатала только тип и
                    // число точек — по ней их не различить. Клетки сетки на местах стоянки
                    // проверены и существуют, значит подозрение на конец пути; но
                    // подозрение не доказательство, поэтому печатаем оба конца и разницу
                    // высот, которая и уводит точку с сетки.
                    TC_LOG_INFO("server.worldserver",
                        "Constellation STEP {}: маршрута нет, тип {:X}, точек {}, карта {}, "
                        "я {:.0f} {:.0f} {:.1f} -> цель {:.0f} {:.0f} {:.1f}, по плоскости {:.0f}, по высоте {:.1f}",
                        self->GetName(), uint32(path.GetPathType()), uint32(path.GetPath().size()),
                        self->GetMapId(),
                        self->GetPositionX(), self->GetPositionY(), self->GetPositionZ(),
                        tx, ty, tz, self->GetExactDist2d(tx, ty), tz - self->GetPositionZ());
                }
                // РАЗБРОС ОБЯЗАТЕЛЕН: без него 122 спутника, вставшие одновременно,
                // повторяют тяжёлый поиск ОДНИМ ЗАЛПОМ — реже, но всё так же кучно, и
                // такт мира снова проваливается раз в три секунды вместо постоянно.
                // Замечание Кодекса; лестница тоже его: 3, 6, 12, 24 секунды.
                // ЗАВИСШЕГО В ВОЗДУХЕ НАДО ВЕРНУТЬ НА ЗЕМЛЮ, ИНАЧЕ ОН ТАМ НАВСЕГДА.
                //
                // Ошибка с высотой (см. шаг движения ниже) уже подняла часть состава над
                // землёй, и сама по себе её починка их не опустит: из воздуха маршрут не
                // строится, а спускаться спутник умеет только маршрутом. Замкнутый круг,
                // и в нём на боевом сидело трое из измеренной выборки.
                //
                // Поэтому на ПЕРВОМ же отказе спрашиваем у карты настоящую высоту под
                // ногами и, если мы выше неё больше чем на два ярда, отправляемся вниз.
                // Запрос дорогой, но он случается только при отказе построителя, а не на
                // такте — то самое различие, которым мы уже дважды за сутки упирали мир
                // в ядро.
                // ПОД НОГАМИ ИЛИ ПОД ЦЕЛЬЮ — ОПЫТ, КОТОРЫЙ ЭТО РАЗЛИЧАЕТ.
                //
                // Тип A (NOPATH|SHORTCUT) означает, что Detour не нашёл полигона под ОДНИМ
                // из концов пути, но не говорит, под каким. Замер: все двадцать отказов — с
                // большой разницей высот, ни одного с разницей меньше пяти ярдов, при этом
                // расстояние по плоскости от 29 до 300 ярдов. То есть виновата высота, а не
                // дальность, — но чья, наша или цели, из этого не следует.
                //
                // Спрашиваем маршрут на пять ярдов вперёд по своему же направлению. Такая
                // цель заведомо на той же поверхности, что и мы. Провалился и он — значит
                // полигона нет ПОД НАМИ, и лечить надо своё положение. Удался — значит наше
                // место в порядке, и дело в конце пути.
                //
                // Один лишний вызов на ПЕРВЫЙ отказ, дальше отступание. Не на такте.
                if (!c.NoPathFails)
                {
                    PathGenerator probe(self);
                    float const px = self->GetPositionX() + std::cos(self->GetOrientation()) * 5.0f;
                    float const py = self->GetPositionY() + std::sin(self->GetOrientation()) * 5.0f;
                    bool const okNear = probe.CalculatePath(px, py, self->GetPositionZ(), false)
                        && !(probe.GetPathType() & (PATHFIND_NOPATH | PATHFIND_SHORTCUT));
                    TC_LOG_INFO("server.worldserver",
                        "Constellation STEP {}: пробный шаг на 5 ярдов {}, тип {:X} — {}",
                        self->GetName(), okNear ? "УДАЛСЯ" : "ПРОВАЛИЛСЯ",
                        uint32(probe.GetPathType()),
                        okNear ? "сетка под ногами есть, беда в конце пути"
                               : "сетки под ногами НЕТ, беда в нашем положении");
                }
                if (!c.NoPathFails)
                {
                    float const gz = self->GetMap()->GetHeight(self->GetPhaseShift(),
                        self->GetPositionX(), self->GetPositionY(), self->GetPositionZ(), true, 50.0f);
                    if (gz > INVALID_HEIGHT && self->GetPositionZ() - gz > 2.0f)
                    {
                        TC_LOG_INFO("server.worldserver",
                            "Constellation STEP {}: висел на {:.1f} над землёй ({:.1f} -> {:.1f}), спускаю",
                            self->GetName(), self->GetPositionZ() - gz, self->GetPositionZ(), gz);
                        Position down(self->GetPositionX(), self->GetPositionY(), gz, self->GetOrientation());
                        SendMove(c, self, down, 0);
                        c.Moving = false;
                    }
                }
                c.RawTarget = true;     // боковая точка не далась — дальше идём в центр
                if (c.NoPathFails < 4)
                    ++c.NoPathFails;
                static uint32 const backoffLadder[5] = { 0, 3000, 6000, 12000, 24000 };
                c.NoPathMs = backoffLadder[c.NoPathFails]
                           + uint32(self->GetGUID().GetCounter() % 1500u);
                if (!Unstick(c, self, tx, ty))
                    c.Stalled = true;
                StopMoving(c, self);
                return false;
            }
            c.NoPathFails = 0;              // маршрут нашёлся — отступ снимаем,
            c.NoPathMs = 0;                 // но НЕ признак «иди в центр»: снять его на
                                            // удачном маршруте значило бы снова подсунуть
                                            // ту же непроходимую боковую точку — качели,
                                            // которые Кодекс и разглядел прямо в коде
            c.Waypoints.clear();
            for (G3D::Vector3 const& v : *pts)
                c.Waypoints.emplace_back(v.x, v.y, v.z);
            c.WaypointIndex = 0;
            // ЦЕЛЬ ПЕРЕСЧЁТА — ТА ТОЧКА, КУДА МЫ РЕАЛЬНО ИДЁМ. При прыжке это его конец,
            // а не далёкая цель: иначе условие пересчёта считало бы, что мы уже у цели.
            c.PathTargetX = aimX;
            c.PathTargetY = aimY;
        }

        // идём к текущей точке маршрута; дошли — берём следующую
        while (c.WaypointIndex < c.Waypoints.size()
            && self->GetExactDist(c.Waypoints[c.WaypointIndex]) < 1.5f)
            ++c.WaypointIndex;
        if (c.WaypointIndex >= c.Waypoints.size())
        {
            c.Waypoints.clear();
            StopMoving(c, self);
            return false;                   // маршрут пройден
        }

        Position const& wp = c.Waypoints[c.WaypointIndex];
        float angle = self->GetAbsoluteAngle(wp.GetPositionX(), wp.GetPositionY());
        float legLen = self->GetExactDist2d(wp.GetPositionX(), wp.GetPositionY());
        float go = std::min(step, legLen);

        // ВЫСОТА БЕРЁТСЯ ПО ДОЛЕ ПРОЙДЕННОГО ОТРЕЗКА, А НЕ У ЕГО ДАЛЬНЕГО КОНЦА.
        //
        // Здесь стояло просто wp.GetPositionZ(), и это была основополагающая ошибка,
        // ломавшая ХОЖДЕНИЕ ЦЕЛИКОМ. Шаг за такт — около полутора ярдов, а отрезок
        // маршрута бывает в десятки. На ровном месте подмена высоты безвредна, но на
        // склоне, на лестнице или на помосте первый же шаг мгновенно поднимал спутника
        // на высоту КОНЦА отрезка, хотя по плоскости он сдвинулся на метр. Он оказывался
        // в воздухе — и это необратимо: полигона под точкой в воздухе нет, Detour отвечает
        // NOPATH|SHORTCUT (в журнале «тип A, точек 2»), и спутник не может построить
        // маршрут больше НИКОГДА.
        //
        // Замер на боевом, 2026-09-01: трое стоят над своими же соседями на том же
        // пятачке — Kaelor на 45,6 ярда выше, Elenwe на 42,8, Thragan на 35 над четырьмя
        // соседями. За пять минут 24 попытки пойти и НОЛЬ приходов. Отсюда же и «20 с без
        // продвижения», и «до принимающего не дойти»: идти было некуда, потому что идти
        // было неоткуда.
        //
        // Точки маршрута лежат на навигационной сетке, то есть на земле. Значит линейная
        // доля между своей высотой и высотой следующей точки идёт по земле и никуда не
        // взлетает. Опрашивать карту на каждом шаге не нужно и нельзя: это тот самый
        // дорогой запрос, который уже упирал мир в ядро.
        float const part = (legLen > 0.01f) ? (go / legLen) : 1.0f;
        float const nz = self->GetPositionZ()
                       + (wp.GetPositionZ() - self->GetPositionZ()) * part;
        Position next(self->GetPositionX() + std::cos(angle) * go,
                      self->GetPositionY() + std::sin(angle) * go,
                      nz, angle);
        SendMove(c, self, next, MOVEMENTFLAG_FORWARD);
        c.Moving = true;
        return true;
    }
    // СМЕРТЬ — НЕ КОНЕЦ, А ДОРОГА ОБРАТНО.
    //
    // Оператор увидел это в игре и спросил, чего они ждут: трое дворфов стоят на
    // кладбище Холодной долины, health = 1, mana = 0. Это состояние ДУХА, а не
    // раненого. Они погибли, дух отпустился, ядро перенесло их к целительнице — и там
    // они остались НАВСЕГДА, потому что весь код на случай смерти был «умер -> стою ->
    // выйти». То есть любой погибший спутник молча выбывал из игры насовсем, и в
    // журнале об этом не было ни строки: переход-то состоялся один раз.
    //
    // Делаем то, что делает живой, и теми же опкодами:
    //   тело ещё не отпущено  -> CMSG_REPOP_REQUEST
    //   дух рядом с целительницей -> CMSG_SPIRIT_HEALER_ACTIVATE
    // К целительнице надо ДОЙТИ: ядро переносит на кладбище, но не вплотную, а порог
    // близости у него свой — INTERACTION_DISTANCE, и меряется он в пространстве.
    // Спрашиваем ядро, а не придумываем: сегодня эта же подмена стоила дня.
    //
    // Почему целительница, а не бег к телу: тело лежит там, где нас убили, и дорога
    // туда ведёт мимо того же, что убило. Целительница берёт плату износом снаряжения
    // и слабостью после воскрешения — на низких уровнях слабости нет вовсе, а износ
    // спутнику дешевле смерти по кругу. Бег к телу — отдельная задача, не эта.
    void Revive(Companion& c, Player* self, float dt)
    {
        uint32 ms = uint32(dt * 1000.0f);
        if (c.ReviveMs > ms)
        {
            c.ReviveMs -= ms;
            return;                     // не чаще раза в две секунды
        }
        c.ReviveMs = 2000;

        // ПРЕДЕЛ, А НЕ ВЕЧНАЯ ПОПЫТКА (Кодекс, проход 5). Он перечислил четыре способа
        // застрять здесь навсегда: отказ отпустить дух, отсутствие целительницы в
        // шестидесяти ярдах, недостижимая по сетке целительница и молча отклонённое
        // воскрешение. Каждый из них крутился бы вечно и молча — ровно та беда, которую
        // мы весь день чиним. Двадцать попыток (сорок секунд), потом отдых пять минут и
        // ОДНА строка в журнал, чтобы это было видно.
        if (++c.ReviveTries > 20)
        {
            c.ReviveTries = 0;
            c.ReviveMs = 300000;
            if (!c.ReviveGaveUp)
            {
                c.ReviveGaveUp = true;
                TC_LOG_INFO("server.worldserver", "Constellation: {} не может воскреснуть — целительницы нет или до неё не дойти",
                    self->GetName());
            }
            return;
        }

        if (!self->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        {
            WorldPacket raw(CMSG_REPOP_REQUEST);
            WorldPackets::Misc::RepopRequest repop(std::move(raw));
            repop.CheckInstance = false;
            c.Session->HandleRepopRequest(repop);
            return;                     // отпустили дух; переносом займётся ядро
        }

        Creature* healer = nullptr;
        float best = 100000.0f;
        std::list<Creature*> near;
        Trinity::AnyUnitInObjectRangeCheck check(self, 60.0f);
        Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, near, check);
        Cell::VisitGridObjects(self, searcher, 60.0f);
        for (Creature* cr : near)
        {
            if (!cr->HasNpcFlag(UNIT_NPC_FLAG_SPIRIT_HEALER) || !cr->IsAlive())
                continue;
            float d = self->GetExactDist(cr);
            if (d < best)
                { best = d; healer = cr; }
        }
        if (!healer)
            return;                     // не видно — ждём, ядро само перенесёт на кладбище

        if (!self->IsWithinDistInMap(healer, INTERACTION_DISTANCE))
        {
            StepToward(c, self, healer->GetPositionX(), healer->GetPositionY(),
                healer->GetPositionZ(), 2.0f, dt);
            if (c.Stalled)              // до целительницы не дойти — считаем это попыткой
                c.ReviveTries += 5;
            return;
        }

        WorldPacket raw(CMSG_SPIRIT_HEALER_ACTIVATE);
        WorldPackets::NPC::SpiritHealerActivate act(std::move(raw));
        act.Healer = healer->GetGUID();
        c.Session->HandleSpiritHealerActivate(act);
        if (self->IsAlive())
        {
            ++_revived;
            c.ReviveTries = 0;
            c.ReviveGaveUp = false;
            // СМЕРТЬ СТОИТ ПРОЧНОСТИ — чиним здесь, иначе износ односторонний и любой
            // спутник рано или поздно молча перестаёт быть бойцом (легионовский урок).
            RepairIfBroken(self, "после смерти");
            c.BrokenNoted = false;
            c.TravelCooldownMs = 300000;    // после смерти не бежать туда же сразу (Кодекс)
            // и не возвращаться к убийце на половине здоровья — сперва отдышаться
            Switch(c, self, Behavior::Recovering, "воскрес, перевожу дух");
            TC_LOG_INFO("server.worldserver", "Constellation: {} воскрес у целительницы душ",
                self->GetName());
        }
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

        // ТЕЛЕПОРТ НАДО ПОДТВЕРДИТЬ — КЛИЕНТА, КОТОРЫЙ ЭТО ДЕЛАЕТ, У НАС НЕТ.
        //
        // Найдено кругом 9 стенда: четверо погибших отпустили дух и «не смогли дойти»
        // до целительницы. На самом деле они НЕ БЫЛИ на кладбище: отпуск духа — это
        // ближний телепорт, ядро взводит семафор и ждёт CMSG_MOVE_TELEPORT_ACK от
        // клиента (HandleMoveTeleportAck: проверяются только guid и семафор, счётчик
        // не сверяется — прочитано в обработчике, не угадано). Без подтверждения
        // перенос не завершается никогда — призрак стоит на месте смерти. Та же
        // судьба ждала бы ЛЮБОЙ будущий телепорт. Подтверждаем, как подтвердил бы
        // клиент; дальний перенос ядро само предусмотрело для вызовов с серверной
        // стороны — HandleMoveWorldportAck().
        if (self->IsBeingTeleportedNear())
        {
            WorldPacket raw(CMSG_MOVE_TELEPORT_ACK);
            WorldPackets::Movement::MoveTeleportAck ack(std::move(raw));
            ack.MoverGUID = self->GetGUID();
            ack.AckIndex = 0;
            ack.MoveTime = GameTime::GetGameTimeMS();
            c.Session->HandleMoveTeleportAck(ack);
            return;                     // этот такт ушёл на перенос
        }
        if (self->IsBeingTeleportedFar())
        {
            c.Session->HandleMoveWorldportAck();
            return;
        }

        c.ModeMs += diff;
        c.MoveMs += diff;
        for (auto it = c.TurnInBackoff.begin(); it != c.TurnInBackoff.end(); )
        {
            if (it->second <= diff)
                it = c.TurnInBackoff.erase(it);     // отлежался — можно пробовать снова
            else
                { it->second -= diff; ++it; }
        }
        if (c.FollowCooldownMs)
            c.FollowCooldownMs = (c.FollowCooldownMs <= diff) ? 0 : c.FollowCooldownMs - diff;
        if (c.EnderScanMs)
            c.EnderScanMs = (c.EnderScanMs <= diff) ? 0 : c.EnderScanMs - diff;
        if (c.IdleScanMs)
            c.IdleScanMs = (c.IdleScanMs <= diff) ? 0 : c.IdleScanMs - diff;
        if (c.TravelCooldownMs)
            c.TravelCooldownMs = (c.TravelCooldownMs <= diff) ? 0 : c.TravelCooldownMs - diff;
            c.VendCooldownMs   = (c.VendCooldownMs   <= diff) ? 0 : c.VendCooldownMs   - diff;

            // ЧЁРНЫЙ СПИСОК КВЕСТОДАТЕЛЕЙ ЗАБЫВАЕТСЯ ЧЕРЕЗ ПЯТЬ МИНУТ.
            //
            // «Навсегда» было ошибкой, и она остановила весь состав за ночь: пары неудач у
            // лестницы хватало, чтобы все квестодатели зоны попали в список, и спутник
            // переставал брать квесты вообще. Замер утром: двенадцать групп сдали свой
            // первый квест и встали, незакрытых квестов НИ У КОГО.
            //
            // Недостижимость не вечна: спутник смещается, NPC ходит, фаза меняется.
            // Пять минут — достаточно, чтобы не долбиться, и мало, чтобы не выпасть из игры.
            if (c.GiverForgetMs <= diff)
            {
                if (!c.GiverUnreachable.empty())
                    c.GiverUnreachable.clear();
                c.GiverForgetMs = 300000;
            }
            else
                c.GiverForgetMs -= diff;
        if (c.TravelScanMs)
            c.TravelScanMs = (c.TravelScanMs <= diff) ? 0 : c.TravelScanMs - diff;
        if (c.RestSkipMs)
            c.RestSkipMs = (c.RestSkipMs <= diff) ? 0 : c.RestSkipMs - diff;
        for (auto it = c.TravelBackoff.begin(); it != c.TravelBackoff.end(); )
        {
            if (it->second <= diff)
                it = c.TravelBackoff.erase(it);
            else
                { it->second -= diff; ++it; }
        }
        if (c.JumpCooldownMs)
        {
            c.JumpCooldownMs = (c.JumpCooldownMs <= diff) ? 0 : c.JumpCooldownMs - diff;
            // минута прошла — запас возвращается ЗДЕСЬ, а не при следующей смене
            // намерения: спутник, застрявший в одном намерении, иначе терял прыжки
            // навсегда, хотя отсрочка обещала обратное (Кодекс, проход 3)
            if (!c.JumpCooldownMs)
                c.JumpsLeft = 3;
        }
        if (c.MoveMs < 250)                 // 4 Гц, как поток живого клиента
            return;
        float dt = c.MoveMs / 1000.0f;
        // ОТРЕЗОК ВРЕМЕНИ САМОГО АВТОМАТА, в миллисекундах.
        //
        // Всё, что НАКАПЛИВАЕТСЯ ниже ворот «раз в 250 мс», обязано считать этот отрезок,
        // а не мировой diff: мировой такт сюда прилетает один раз на четверть секунды, и
        // счётчик на нём идёт в двадцать с лишним раз медленнее реального времени. Это и
        // была причина, по которой пятнадцатисекундный сторож «бью, а следа нет» молчал
        // все 1033 боя: за пять минут он набирал около двенадцати тысяч при пороге
        // пятнадцать, а под прежним двухминутным пределом — меньше пяти.
        //
        // Таких счётчиков ровно ДВА: NoDamageMs и WantedCheckMs. Все прочие отсчёты
        // (JumpCooldownMs, TravelCooldownMs, RestSkipMs, EnderScanMs и остальные) убывают
        // ВЫШЕ ворот, каждый мировой такт, и потому всегда шли верно — их трогать нельзя.
        uint32 const slice = c.MoveMs;
        c.MoveMs = 0;

        if (!self->IsAlive())
        {
            if (c.Mode != Behavior::Idle)
            {
                if (c.Mode == Behavior::Attacking || c.Mode == Behavior::ApproachingTarget)
                    LogFightOutcome(self, ObjectAccessor::GetCreature(*self, c.TargetGuid), "ПОГИБ", c);
                Switch(c, self, Behavior::Idle, "погиб");
            }
            // умерли по дороге — не бежать той же дорогой снова (Кодекс, проход 5):
            // без этого смерть возвращала в «стою», откуда та же цель выбиралась опять
            c.TravelCooldownMs = 300000;
            Revive(c, self, dt);
            return;
        }
        // не управляем собой (транспорт, контроль) — пакет с нашим guid относился бы
        // к другому существу
        if (self->GetUnitBeingMoved() != self)
            return;

        // САМОПРОВЕРКА ПРЫЖКА — ТОЛЬКО НА СТЕНДЕ.
        //
        // Без неё прыжок уезжает в выкладку недоказанным: за пятиминутный прогон
        // построитель маршрутов не отказал НИ РАЗУ (ноль строк STEP в журнале), значит
        // Unstick не вызывался вовсе, и сказать о прыжке было бы нечего, кроме
        // «собралось». А правило здесь одно: изменение ПРОВЕРЕНО, а не заявлено.
        //
        // Проверяем ровно то, что может сломаться: приняло ли ядро пару пакетов,
        // сдвинулся ли спутник и насколько, снят ли признак падения (иначе он больше
        // никогда не пойдёт) и не отняло ли здоровья.
        if (Cfg().RigMode && !c.JumpProbed)
        {
            c.JumpProbed = true;
            Position was = self->GetPosition();
            uint64 hpWas = self->GetHealth();
            bool ok = Jump(c, self,
                self->GetPositionX() + std::cos(self->GetOrientation()) * 5.0f,
                self->GetPositionY() + std::sin(self->GetOrientation()) * 5.0f);
            TC_LOG_INFO("server.worldserver",
                "Constellation JUMPTEST {}: прыгнул={} сдвиг={:.2f} по высоте={:.2f} падение={} здоровье {}->{}",
                self->GetName(), ok ? 1 : 0,
                self->GetExactDist2d(was.GetPositionX(), was.GetPositionY()),
                self->GetPositionZ() - was.GetPositionZ(),
                self->IsFalling() ? 1 : 0, hpWas, self->GetHealth());
            return;
        }


        switch (c.Mode)
        {
            case Behavior::Idle:
            {
                // «СТОЮ» НЕ ИМЕЕТ ПРАВА ПЕРЕБИРАТЬ МИР ЧЕТЫРЕ РАЗА В СЕКУНДУ.
                //
                // Два поиска ниже — самые дорогие в этом состоянии: FindTurnIn проходит
                // весь журнал заданий, обратные связи квестов и указатель мест появления,
                // а FindObjectiveTarget вдобавок обходит сетку и по нескольку раз
                // просматривает найденное целиком. Оба звались БЕЗ ограничения, и пока
                // спутнику нечем заняться — а таких сейчас большинство — это 122 × 4
                // полных перебора в секунду на единственном потоке мира. Кодекс перечислил
                // их по строкам как главных претендентов после построителя маршрута; замер
                // согласен: ядро поднялось с 45 % до 84 % ровно по мере того, как состав
                // скапливался в «стою».
                //
                // Раз в секунду достаточно: цель за это время не появится и не исчезнет
                // незаметно. Разброс — чтобы 122 спутника не перебирали мир одним залпом.
                bool const idleScan = (c.IdleScanMs == 0);
                if (idleScan)
                    c.IdleScanMs = 1000 + (c.Guid.GetCounter() % 250u);

                // ВЫПОЛНЕННОЕ СДАЁМ ПЕРВЫМ ДЕЛОМ: висящий в журнале готовый квест
                // занимает место и не даёт взять следующий, а награда — это опыт,
                // без которого спутник останется первого уровня навсегда.
                if (Cfg().Quests && idleScan && FindTurnIn(c, self))
                {
                    Switch(c, self, Behavior::TurningIn, "есть что сдать");
                    return;
                }
                // ОТДЫХ ВПЕРЕДИ ДРАКИ, но ПОЗАДИ сдачи: сдать готовое можно и раненым,
                // а вот идти за новой целью — нет.
                if (!c.RestSkipMs && NeedsRest(self))
                {
                    Switch(c, self, Behavior::Recovering, "надо перевести дух");
                    return;
                }
                // К ТОРГОВЦУ — ВПЕРЕДИ ДРАКИ, И ЭТО НЕ ЖАДНОСТЬ.
                //
                // Сломанное оружие не наносит урона вовсе — GetWeaponForAttack возвращает
                // на сломанном nullptr, — а полные сумки означают, что добыча с убийства
                // пропадёт. И то и другое делает следующий бой бессмысленным, поэтому
                // решается раньше него, но позже сдачи готовых квестов и отдыха.
                // ОБХОД СЕТКИ — НЕ ЧАЩЕ РАЗА В ПЯТЬ СЕКУНД.
                //
                // Замер на боевом: мир упёрся в 99 % ядра, автомат почти перестал тикать —
                // одна строка журнала за десять секунд. Причина моя: проверка «торговец
                // рядом?» звала обход сетки на СТО ярдов каждый такт у каждого изношенного
                // спутника. Изношены почти все, тактов четыре в секунду, спутников 122 —
                // полтысячи обходов в секунду.
                if (c.VendScanMs)
                {
                    c.VendScanMs = (c.VendScanMs <= diff) ? 0 : c.VendScanMs - diff;
                }
                else if (Cfg().Vending && !c.VendCooldownMs)
                {
                    c.VendScanMs = 5000;
                    // ТРИ РАЗНЫХ ПОВОДА, И ОНИ НЕ РАВНОЗНАЧНЫ.
                    //
                    // «Не могу бить» и «сумки полны» действительно останавливают спутника:
                    // без оружия он не наносит урона, с полными сумками теряет добычу. Ради
                    // них стоит идти. А просто изношенное снаряжение работает — ради него
                    // бросать квест и бой нельзя, чинимся мимоходом.
                    // «БИТЬ НЕЧЕМ» ЛЕЧИТСЯ РЕМОНТОМ ТОЛЬКО ЕСЛИ ЕСТЬ ЧТО ЧИНИТЬ.
                    //
                    // У рыцарей смерти слот оружия ПУСТ — оружие им выдаёт первый квест.
                    // Ремонт пустоту не заполняет, поэтому «бить нечем» у них истинно
                    // всегда, и они ходили к торговцу бесконечно. Пока нет надевания вещей
                    // из сумок (задача 0009), таких не отправляем вовсе.
                    bool const helpless = CannotFight(self) && BrokenCount(self) > 0;
                    bool const stuffed = FreeBagSpace(self) <= 2;
                    bool const worn = DamagedCount(self) > 0;

                    // МИМОХОДОМ: изношен, но дееспособен — только если торговец уже рядом.
                    // Двадцать ярдов это «прохожу мимо», а не «схожу-ка я за тридцать».
                    bool passingBy = false;
                    if (worn && !helpless && !stuffed)
                        if (Creature* near = FindVendorNear(self, false, true))
                            passingBy = self->IsWithinDistInMap(near, 20.0f);

                    if (helpless || stuffed || passingBy)
                    {
                        // ИЩЕМ ТОГО, КТО УМЕЕТ НУЖНОЕ. Полные сумки требуют продавца,
                        // поломка — ремонтника; идти к тому, кто не умеет, значит вернуться
                        // ни с чем и повторить через минуту (Кодекс).
                        if (Creature* vendor = FindVendorNear(self, stuffed, helpless || passingBy))
                        {
                            c.VendorGuid = vendor->GetGUID();
                            Switch(c, self, Behavior::Vending,
                                helpless ? "бить нечем, иду чиниться"
                                         : stuffed ? "сумки полны, иду продавать"
                                                   : "торговец рядом, чинюсь мимоходом");
                            return;
                        }
                        // НИКОГО ПОБЛИЗОСТИ. Это не ошибка, а измеряемый предел первой
                        // версии: искать по всей карте она не умеет. Считаем и молчим
                        // пять минут, чтобы не перебирать сетку каждый такт.
                        ++c.VendNoVendor;
                        c.VendCooldownMs = 300000 + (c.Guid.GetCounter() % 61) * 1000;
                    }
                }
                if (Creature* target = (Cfg().Fight && idleScan) ? FindObjectiveTarget(c, self) : nullptr)
                {
                    c.TargetGuid = target->GetGUID();
                    c.LastDist = self->GetExactDist2d(target);
                    Switch(c, self, Behavior::ApproachingTarget, "нашлась цель квеста");
                    return;
                }
                // за хозяином идём, только если он ДОСТИЖИМ. Иначе спутник метался
                // «пошёл — далеко — стою» по нескольку раз в секунду: 420 холостых
                // переходов за прогон, из которых ни один ничего не менял.
                // цели рядом нет — но она где-то есть, и мы знаем где
                // СЛОМАННЫЙ НЕ ИДЁТ И К МЕСТУ ЗАДАНИЯ.
                // Запрет только на выбор существа запретом не был: агро на боевой точке
                // берётся близостью, и круг смерти оставался, просто шёл медленнее
                // (Кодекс). Здесь мы уже после сдачи готовых квестов и после отдыха, так
                // что полезное спутник по-прежнему делает — и за хозяином ходит.
                if (Cfg().Fight && !c.TravelCooldownMs && !c.TravelScanMs && !BrokenForFight(c, self))
                {
                    if (FindObjectiveSpot(c, self))
                    {
                        Switch(c, self, Behavior::Travelling, "иду за целью задания");
                        return;
                    }
                    c.TravelScanMs = 2000;  // впустую — не перебирать точки каждый такт
                }
                Position owner;
                if (Cfg().Follow && !c.FollowCooldownMs && FollowTargetPos(c, self, &owner)
                    && self->GetExactDist2d(owner.GetPositionX(), owner.GetPositionY()) <= Cfg().FollowMaxRange)
                {
                    c.LastDist = self->GetExactDist2d(owner.GetPositionX(), owner.GetPositionY());
                    Switch(c, self, Behavior::FollowingOwner, "есть за кем идти");
                }
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
                // Единственное состояние, у которого НЕ БЫЛО срока: спутник, упёршийся
                // в угол по дороге к хозяину, стоял там вечно. Теперь и оно смертно —
                // причём срок обнуляет ПРИБЛИЖЕНИЕ, а не любое шевеление: иначе шаги
                // вбок у стены продлевали бы его до бесконечности (Кодекс, 2026-08-30).
                float od = self->GetExactDist2d(owner.GetPositionX(), owner.GetPositionY());
                StepToward(c, self, owner.GetPositionX(), owner.GetPositionY(),
                    owner.GetPositionZ(), Cfg().FollowDistance, dt);
                if (od < c.LastDist - 1.0f)
                {
                    c.LastDist = od;
                    c.ModeMs = 0;
                }
                else if (c.Stalled || c.ModeMs > 30000)
                {
                    char const* why = c.Stalled ? "до хозяина не дойти" : "полминуты без движения к хозяину";
                    c.FollowCooldownMs = 10000; // не возвращаться сюда каждый такт
                    Switch(c, self, Behavior::Idle, why);
                }
                return;
            }

            case Behavior::Recovering:
            {
                // напали — драться, а не сидеть: отдых прерывается боем
                if (self->IsInCombat())
                {
                    Switch(c, self, Behavior::Idle, "в бою не до отдыха");
                    return;
                }
                StopMoving(c, self);
                if (RestedEnough(self))
                {
                    c.RestSkipMs = 0;   // восстановление работает — запрет ни к чему
                    Switch(c, self, Behavior::Idle, "отдышался");
                    return;
                }
                // ПОТОЛОК ОЖИДАНИЯ — И ОН ДОЛЖЕН БЫТЬ НАСТОЯЩИМ.
                //
                // Первая редакция просто уходила в «стою» по истечении срока — а там
                // NeedsRest всё ещё истинно, и следующий же такт возвращал в отдых.
                // Получался вечный «отдыхал две минуты -> такт в стою -> снова две
                // минуты», то есть обещанный потолок не ограничивал ничего, и спутник
                // с застрявшим восстановлением стоял бы навсегда, лишь печатая по два
                // перехода в цикл (Кодекс, проход 7 — блокирующее).
                //
                // Настоящий выход: после неудачного отдыха ЗАПРЕЩАЕМ отдыхать вовсе на
                // время, вдвое большее самого отдыха. Спутник идёт драться раненым —
                // это плохо, но это ДЕЙСТВИЕ, из которого есть выход, в отличие от
                // стояния столбом.
                if (c.ModeMs > Cfg().RestMaxMs)
                {
                    c.RestSkipMs = Cfg().RestMaxMs * 2;
                    Switch(c, self, Behavior::Idle, "отдых не помогает, иду как есть");
                }
                return;
            }

            case Behavior::Vending:
            {
                Creature* vendor = ObjectAccessor::GetCreature(*self, c.VendorGuid);
                if (!vendor || !vendor->IsAlive())
                {
                    c.VendorGuid.Clear();
                    Switch(c, self, Behavior::Idle, "торговец пропал");
                    return;
                }

                // МОЖНО ЛИ УЖЕ ТОРГОВАТЬ — РЕШАЕТ ЯДРО, ТЕМ ЖЕ ВОПРОСОМ, ЧТО ЗАДАЁТ СЕБЕ
                // ОБРАБОТЧИК. Своей дистанции здесь нет намеренно: клиентский предел это
                // радиус досягаемости существа плюс четыре ярда, и повторять это число у
                // себя значит завести вторую истину, которая разойдётся с первой.
                if (!self->GetNPCIfCanInteractWith(c.VendorGuid, UNIT_NPC_FLAG_NONE, UNIT_NPC_FLAG_2_NONE))
                {
                    float vx, vy, vz;
                    ApproachPoint(c, vendor, self, vx, vy, vz, diff);
                    StepToward(c, self, vx, vy, vz, vendor->GetCombatReach() + 2.0f, dt);
                    // СРОК: не дошёл за две минуты — бросаем и живём дальше. Стоять
                    // столбом у недостижимого торговца хуже, чем ходить сломанным.
                    // ТУПИК РАСПОЗНАЁМ СРАЗУ, А НЕ ЧЕРЕЗ ДВЕ МИНУТЫ.
                    //
                    // Автомат уже умеет говорить «отступать больше некуда» (c.Stalled), и
                    // три других состояния его слушают. Торговля не слушала — значит
                    // недостижимый торговец означал две минуты бега в стену вместо трёх
                    // секунд. Разбор поймал это сравнением с Travelling.
                    if (c.Stalled || c.ModeMs > 120000)
                    {
                        c.VendCooldownMs = 300000;
                        c.VendorGuid.Clear();
                        Switch(c, self, Behavior::Idle, "не дошёл до торговца");
                    }
                    return;
                }

                // ДОШЛИ. Открываем прилавок тем же пакетом, что шлёт клиент; обработчик
                // сам перепроверит флаг продавца через GetNPCIfCanInteractWith.
                bool poorBefore = c.VendPoor;
                if (vendor->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
                {
                    WorldPacket raw(CMSG_LIST_INVENTORY);
                    WorldPackets::NPC::Hello list(std::move(raw));
                    list.Unit = c.VendorGuid;
                    c.Session->HandleListInventoryOpcode(list);
                    SellJunkTo(c, self, vendor);
                }
                RepairAt(c, self, vendor);

                // РАЗБРОС, ЧТОБЫ НЕ ХОДИТЬ СТРОЕМ. Кодекс: одинаковые пороги и одинаковые
                // таймеры у 122 спутников дают синхронную толпу у одного NPC — и человеку
                // это заметнее, чем сама частота пакетов. Разброс берём от идентификатора
                // спутника, а не от случайного числа: он постоянен и воспроизводим.
                // НЕ ХВАТИЛО ДЕНЕГ — УХОДИМ НАДОЛГО, А НЕ ПО КРУГУ.
                //
                // Живая петля, найденная оператором в клиенте: сломан -> к торговцу ->
                // денег нет -> обратно -> снова сломан -> снова к торговцу. Поход стоит
                // ВПЕРЕДИ драки и квестов, поэтому спутник переставал делать что-либо
                // ещё — а сломаны были почти все. Минутного срока не хватало: за минуту
                // он ничего не зарабатывает, потому что вместо заработка снова идёт.
                //
                // Десять минут — это время, за которое можно набить хлама и продать его.
                // ДЕНЕГ НЕТ — ЧИНИМ ДАРОМ. ЭТО СТРАХОВКА ОТ ТУПИКА, А НЕ ПОБЛАЖКА.
                //
                // Оператор поймал негодность прежнего замысла одним вопросом: «как они
                // будут воевать в сломанном? скилы не работают у тех, кто завязан на
                // оружии». Отправить сломанного зарабатывать нельзя — сломанным оружием
                // не заработать: GetWeaponForAttack на сломанном возвращает nullptr, и ни
                // автоудар, ни оружейные умения не проходят. Круг замкнут: чтобы починиться
                // нужны деньги, чтобы деньги — нужно бить, чтобы бить — нужна починка.
                //
                // Поэтому платный ремонт остаётся обычным путём, а даровой — полом, ниже
                // которого спутник не падает. Цена названа вслух: это отступление от
                // «только клиентскими опкодами», и оно сознательное. Альтернатива —
                // спутник, навсегда выбывший из игры.
                bool const stillPoor = c.VendPoor > poorBefore;
                if (stillPoor && BrokenCount(self))
                {
                    uint32 const fixed = RepairIfBroken(self, "страховка: денег нет", /*operatorAsked=*/true);
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ТОРГ {}: на ремонт не хватило — починил даром {} вещей, "
                        "иначе он выбывает навсегда", self->GetName(), fixed);
                }
                c.VendCooldownMs = 60000 + (c.Guid.GetCounter() % 47) * 1000;
                c.VendorGuid.Clear();
                Switch(c, self, Behavior::Idle, "торговля закончена");
                return;
            }

            case Behavior::Travelling:
            {
                // дошли настолько, что цель уже видно — дальше обычным порядком
                if (Creature* target = FindObjectiveTarget(c, self))
                {
                    c.TargetGuid = target->GetGUID();
                    c.LastDist = self->GetExactDist(target);
                    Switch(c, self, Behavior::ApproachingTarget, "цель показалась");
                    return;
                }
                // ПРИШЛИ, А ЦЕЛЕЙ НЕТ — это ВЫХОД, а не повод стоять до срока и идти
                // снова (Кодекс, проход 6: круг «дорога -> стою -> та же дорога» был
                // ограничен только внутри одного захода и повторялся вечно). Область
                // пуста — выбита или её наполняет скрипт волнами; откладываем ЭТОТ
                // квест на десять минут, остальным дорога открыта.
                if (self->GetExactDist2d(c.TravelPos.GetPositionX(), c.TravelPos.GetPositionY()) <= 12.0f)
                {
                    c.TravelBackoff[c.TravelQuest] = 600000;
                    Switch(c, self, Behavior::Idle, "пришёл — целей нет, отложил квест");
                    return;
                }
                StepToward(c, self, c.TravelPos.GetPositionX(), c.TravelPos.GetPositionY(),
                    c.TravelPos.GetPositionZ(), 10.0f, dt);
                // СРОК щедрый: до цели бывает и двести ярдов, а бежим мы шагами по 4 Гц
                if (c.Stalled || c.ModeMs > 180000)
                {
                    c.TravelCooldownMs = 120000;
                    Switch(c, self, Behavior::Idle, c.Stalled ? "до места задания не дойти" : "три минуты в пути без толку");
                }
                return;
            }

            case Behavior::ApproachingTarget:
            {
                // сломались по дороге — разворачиваемся, а не доходим умирать
                if (BrokenForFight(c, self))
                {
                    Switch(c, self, Behavior::Idle, "снаряжение сломано");
                    return;
                }
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
                // ДОСТАЛИ ЛИ МЫ ДО ЦЕЛИ — РЕШАЕТ ЯДРО. Здесь стояло «ближе пяти ярдов
                // по плоскости», и цель на помосте объявлялась достижимой: удар не
                // проходил, а мы заносили её в отказные НАВСЕГДА (Кодекс, проход 4).
                float dist = self->GetExactDist(target);

                // С КАКОЙ ДИСТАНЦИИ ЭТОТ СПУТНИК ВООБЩЕ МОЖЕТ ДРАТЬСЯ.
                //
                // Раньше здесь для всех стояли четыре ярда, и маг подбегал вплотную, чтобы
                // прочитать двухсекундное заклинание под ударами. Дистанцию знает само
                // заклинание — GetMaxRange, — и берём её у того, которое спутник и
                // применит. Два ярда внутрь предела: шаг движения не должен выбрасывать
                // за границу и срывать каст.
                //
                // Считаем один раз на бой: обход книги у 122 спутников на каждом такте
                // это полтысячи обходов в секунду впустую.
                if (c.EngageRange == 0.0f)
                {
                    c.EngageRange = -1.0f;      // -1 = посчитали, вышло «ближний бой»
                    if (Cfg().Abilities)
                        if (uint32 sp = PickAttackSpell(self, target))
                            if (SpellInfo const* si = sSpellMgr->GetSpellInfo(sp, self->GetMap()->GetDifficultyID()))
                            {
                                float const r = si->GetMaxRange(false, self);
                                if (r > 8.0f)   // всё, что меньше, — это и есть ближний бой
                                    c.EngageRange = r - 2.0f;
                            }
                }

                bool const closeEnough = c.EngageRange > 0.0f
                    ? self->IsWithinDistInMap(target, c.EngageRange) && self->IsWithinLOSInMap(target)
                    : self->IsWithinMeleeRange(target);

                if (closeEnough)
                {
                    if (TryAttack(c, self, target))
                        Switch(c, self, Behavior::Attacking,
                            c.EngageRange > 0.0f ? "на дистанции заклинания" : "дошёл и ударил");
                    else
                    {
                        c.Refused.insert(c.TargetGuid);
                        Switch(c, self, Behavior::Idle, "удар не принят ядром");
                    }
                    return;
                }
                StepToward(c, self, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(),
                    c.EngageRange > 0.0f ? c.EngageRange : 4.0f, dt);
                if (c.Stalled)
                {
                    c.Refused.insert(c.TargetGuid);
                    Switch(c, self, Behavior::Idle, "до цели не дойти");
                    return;
                }
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
                // самостоятельная сдача: никуда не идём, закрываем на месте
                if (!c.TurnInEntry)
                {
                    if (TurnInAt(c, self, nullptr))
                        Switch(c, self, Behavior::Idle, "сдал сам себе");
                    else
                    {
                        c.TurnInBackoff[c.TurnInQuest] = 300000;   // пять минут: такие квесты часто
                        Switch(c, self, Behavior::Idle, "сам себе не сдаётся");
                    }
                    return;
                }

                // ПОРОГ БЛИЗОСТИ ПРИДУМЫВАТЬ НЕЛЬЗЯ — ЕГО ЗНАЕТ ЯДРО.
                //
                // Здесь стояло «ближе шести ярдов ПО ПЛОСКОСТИ — значит дошёл, ищем
                // принимающего рядом». На боевом это дало второй мёртвый круг, и вот он
                // в числах: три гоблина стоят в (-4979, 783, 280), Карво Бластболт — в
                // (-4984, 780, 289). По плоскости 5.8 ярда, мой порог 6: «дошёл».
                // В ПРОСТРАНСТВЕ 10.7, а ядру нужно 5 — INTERACTION_DISTANCE, и меряет
                // оно в пространстве. Он стоит на помосте девятью ярдами выше. Итог:
                // 916 кругов на спутника за семь минут и НОЛЬ сдач.
                //
                // Это та же ошибка, что была утром с высотой цели, только в другой
                // мерке: своя мерка вместо мерки ядра. Поэтому теперь мы не решаем,
                // дошли ли мы, — мы СПРАШИВАЕМ ядро (CanInteractWithQuestGiver) и идём
                // к самому НПС, пока оно не ответит «да». Ровно так делает игрок,
                // бегущий по метке в журнале.
                // НАЙДЕННОГО ЗАПОМИНАЕМ. Обзор в сорок ярдов вчетверо дороже прежнего
                // двенадцатиярдового по площади, а шёл бы он каждый такт: 122 спутника
                // на 4 Гц — под пять сотен обходов сетки в секунду в потоке мира.
                // Ровно та же цена, которую Кодекс посчитал за указатель спавнов, и
                // ровно тот же ответ: искать редко, помнить найденное.
                Creature* ender = c.TurnInGuid.IsEmpty() ? nullptr : ObjectAccessor::GetCreature(*self, c.TurnInGuid);
                if (ender && !ender->IsAlive())
                    ender = nullptr;
                if (!ender)
                {
                    c.TurnInGuid.Clear();
                    if (!c.EnderScanMs)
                    {
                        c.EnderScanMs = 2000;       // не чаще раза в две секунды
                        std::list<Creature*> near;
                        Trinity::AnyUnitInObjectRangeCheck check(self, 40.0f);
                        Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, near, check);
                        Cell::VisitGridObjects(self, searcher, 40.0f);
                        float best = 100000.0f;
                        for (Creature* creature : near)
                        {
                            if (creature->GetEntry() != c.TurnInEntry || !creature->IsAlive())
                                continue;
                            float d3 = self->GetExactDist(creature);    // БЛИЖАЙШИЙ, а не первый попавшийся
                            if (d3 < best)
                                { best = d3; ender = creature; }
                        }
                        if (ender)
                            c.TurnInGuid = ender->GetGUID();
                    }
                }

                if (ender)
                {
                    if (self->CanInteractWithQuestGiver(ender))
                    {
                        if (TurnInAt(c, self, ender))
                            Switch(c, self, Behavior::Idle, "сдал");
                        else
                        {
                            // не вышло — откладываем ЭТОТ квест, а не пробуем снова
                            // четыре раза в секунду (Кодекс, 2026-08-30)
                            c.TurnInBackoff[c.TurnInQuest] = 60000;
                            Switch(c, self, Behavior::Idle, "сдача не прошла, отложил");
                        }
                        return;
                    }
                    // видим, но ядро говорит «далеко» — подходим к НЕМУ САМОМУ, со
                    // всеми тремя координатами: он может стоять выше или ниже нас, и
                    // именно это здесь и происходит
                    // К ТОЧКЕ НА ПОДХОДЕ, А НЕ В САМОГО ПРИНИМАЮЩЕГО (задача 0013).
                    float ax, ay, az;
                    ApproachPoint(c, ender, self, ax, ay, az, diff);
                    StepToward(c, self, ax, ay, az, ender->GetCombatReach() + 2.0f, dt);
                    if (c.Stalled || c.ModeMs > 60000)
                    {
                        c.TurnInBackoff[c.TurnInQuest] = 60000;
                        Switch(c, self, Behavior::Idle, "к принимающему не подойти вплотную");
                    }
                    return;
                }

                // принимающего не видно вовсе — идём к его точке из указателя спавнов
                float d = self->GetExactDist2d(c.TurnInPos.GetPositionX(), c.TurnInPos.GetPositionY());
                if (d > 6.0f)
                {
                    StepToward(c, self, c.TurnInPos.GetPositionX(), c.TurnInPos.GetPositionY(),
                        c.TurnInPos.GetPositionZ(), 5.0f, dt);
                    if (c.Stalled || c.ModeMs > 60000)
                    {
                        c.TurnInBackoff[c.TurnInQuest] = 60000;
                        Switch(c, self, Behavior::Idle, "до принимающего не дойти");
                    }
                    return;
                }

                // пришли на точку, а его там нет.
                //
                // ОТСРОЧКА ЗДЕСЬ И ПОТЕРЯЛАСЬ, и это моя процессная ошибка, а не
                // логическая: я её сюда добавлял, но тем скриптом, который делал замену
                // БЕЗ проверки совпадения, а искомая строка отличалась отступом. Замена
                // молча не произошла, и я этого не увидел. Все позднейшие правки идут
                // через помощник, который падает, если образец не найден.
                c.TurnInBackoff[c.TurnInQuest] = 60000;
                Switch(c, self, Behavior::Idle, "у точки принимающего нет");
                return;
            }

            case Behavior::Attacking:
            {
                Unit* victim = self->GetVictim();
                if (!victim || !victim->IsAlive())
                {
                    // ПОБЕДУ СЧИТАЕТ ЯДРО, А НЕ Я.
                    //
                    // Прежняя ветка читала GetVictim(), который ядро обнуляет в момент
                    // смерти цели, — победа не засчитывалась почти никогда, и ночные
                    // выводы строились на числе, которое мерило редкую гонку.
                    //
                    // Первая попытка починки объявляла победой «запомненный моб мёртв или
                    // исчез», и Кодекс её справедливо забраковал: это доказывает смерть
                    // моба, а не НАШЕ авторство. Сто двадцать два спутника ходят по одним
                    // стартовым зонам, поэтому чужое убийство записывалось бы нам. Теперь
                    // берём атрибуцию у ядра: OnCreatureKill(убийца, убитый) зовётся из
                    // Unit::Kill (Unit.cpp:11558) ровно для того, кто добил.
                    // ДВА УСЛОВИЯ, И ОБА НУЖНЫ: счёт убийств вырос ЗА ЭТОТ БОЙ, и
                    // последним убитым был ИМЕННО наш противник. Одного счётчика мало —
                    // спутник мог добить кого-то ещё; одного совпадения тоже мало —
                    // существо возрождается с тем же GUID, и старая запись о победе над
                    // ним засчиталась бы снова (Кодекс, проход 11).
                    // ЧИТАЛИ ЛИ МЫ ЗАКЛИНАНИЕ, КОГДА БОЙ КОНЧИЛСЯ.
                    // Догадка оператора: цель добивает другой спутник, пока мы читаем, и
                    // каст срывается. Проверять это можно только ЗДЕСЬ — ниже по ветке
                    // CastAtTarget уже не зовётся. Само по себе число не доказывает срыв
                    // (мы могли и дочитать в тот же миг), но если оно ноль, версии не на
                    // чем стоять; а если велико — смотреть, чем кончился бой: «ПОБЕДА»
                    // значит дочитали и добили сами, «добили не мы» — это ровно случай
                    // оператора.
                    if (c.WasCasting)
                        ++c.CastsDiedUnder;

                    Manager::Blows const k = Manager::Instance()->BlowsOf(self->GetGUID());
                    bool const won = k.Kills > c.KillsAtStart && k.LastKilled == c.FightVictim;
                    char const* how;
                    if (won)
                        how = "ПОБЕДА";
                    else if (victim)
                        how = "цель мертва, но добили не мы";
                    else
                        how = "бой прекратился";
                    // ДОБЫЧА ТОЛЬКО СО СВОЕГО УБИЙСТВА, и это не вежливость: сто двадцать
                    // два спутника ходят по одним стартовым зонам, и бег к любому видимому
                    // трупу — это кража у живого игрока и лагерь у чужого тела (Кодекс,
                    // разбор рисков). Право проверит и ядро, но пакета, заведомо обречённого
                    // на отказ, лучше не слать вовсе.
                    if (Cfg().Loot && won && c.Session)
                    {
                        c.LootTarget = c.FightVictim;
                        LootFromCorpse(c, self);
                        c.LootTarget.Clear();
                    }

                    LogFightOutcome(self, victim, how, c);
                    Switch(c, self, Behavior::Idle, how);
                    return;
                }
                // МАНЕКЕН НЕ УМРЁТ, И ЖДАТЬ ЕГО СМЕРТИ БЕССМЫСЛЕННО.
                //
                // Четверо троллей висели в бою по четырнадцать раз, при ПОЛНОМ здоровье:
                // их квест «The Basics: Hitting Things» велит бить Tiki Target, а у того
                // unit_flags = PACIFIED|STUNNED — это тренировочный манекен. Он не
                // отвечает и не умирает, и засчитано за наши удары не было ничего:
                // строк прогресса в базе НОЛЬ. Ждать тут нечего — уходим через
                // пятнадцать секунд, а не через две минуты.
                // Первая версия помнила МИНИМУМ здоровья, и Кодекс показал, чем это
                // плохо: после любого лечения последующий настоящий урон выше старого
                // минимума становился невидим, и живой лечащийся моб объявлялся
                // манекеном. Плюс отсчёт не был привязан к КОНКРЕТНОЙ жертве: смена
                // цели под автоматом наследовала чужой отсчёт.
                // ПЕРЕПИСЬ СОСТОЯНИЙ — ЭТО ДОЛЯ ТАКТОВ, А НЕ ЧИСЛО ОТКАЗОВ.
                //
                // Ядро проверяет вентиль в Player::Update, когда готов таймер замаха, и
                // отдельно для правой и левой руки. Автомат идёт 4 Гц и берёт ОДНО
                // состояние за такт, поэтому короткие выпадения он пропустит, а долгие
                // переоценит (Кодекс, проход 10). Читать эти числа как «сколько времени
                // спутник провёл в состоянии, при котором удар невозможен» — и только.
                //
                // ПОРЯДОК — КАК В ЯДРЕ, и уклонение считается ОТДЕЛЬНО.
                // DoMeleeAttackIfReady проверяет: боевое состояние, рывок, чтение,
                // жертву, дальность, сектор. Уклонение цели вычисляется ПОЗЖЕ, уже
                // внутри броска исхода, поэтому это независимое наблюдение, а не звено
                // той же цепочки: моб может одновременно уклоняться и быть вне
                // досягаемости, и ядро откажет по дальности (Кодекс, проход 11).
                ++c.GateTicks;
                if (Creature* vc = victim->ToCreature())
                    if (vc->IsEvadingAttacks())
                        ++c.GateEvading;
                if (!self->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                    ++c.GateNoState;
                else if (self->HasUnitState(UNIT_STATE_CHARGING) || self->HasUnitState(UNIT_STATE_CASTING))
                    ++c.GateBusy;
                else if (!self->IsWithinMeleeRange(victim))
                    ++c.GateOutOfRange;
                else if (!self->IsWithinBoundaryRadius(victim)
                    && !self->HasInArc(2.0f * float(M_PI) / 3.0f, victim))
                    ++c.GateBadFacing;
                else if (!self->isAttackReady(BASE_ATTACK))
                    ++c.GateNotReady;   // всё открыто, а таймер удара не подошёл

                // СЧИТАЕМ СВОЙ УРОН, А НЕ ЗДОРОВЬЕ ЦЕЛИ.
                //
                // Прежний вариант обнулял отсчёт при любом падении здоровья жертвы. В
                // общих стартовых зонах по одному мобу бьют несколько спутников, поэтому
                // ЧУЖОЙ удар постоянно продлевал наш бессмысленный бой: за 1033 боя этот
                // выход не сработал НИ РАЗУ, хотя 429 боёв кончились с целью ровно на
                // 100 %. Свой урон даёт перехватчик, и он не зависит ни от чужих ударов,
                // ни от восстановления моба при уходе домой.
                // ЦЕЛЬ БОЯ НЕ ДОЛЖНА ПОДМЕНЯТЬСЯ ПОД НАМИ.
                //
                // Прежде подмена молча сбрасывала сторожа, и бой продолжался вечно. Ядро
                // само перечисляет, когда бой надо прекратить, и подмена жертвы в этом
                // списке: весь учёт боя — отсечки, замахи, урон — привязан к ОДНОЙ цели,
                // и с другой он бессмыслен. Уходим и выбираем заново.
                if (victim->GetGUID() != c.FightVictim)
                {
                    ++c.VictimSwaps;
                    LogFightOutcome(self, victim, "цель подменилась", c);
                    Switch(c, self, Behavior::Idle, "цель подменилась");
                    return;
                }

                // СТОРОЖ СЧИТАЕТ УРОН ЭТОГО БОЯ, А НЕ ВСЮ ЖИЗНЬ СПУТНИКА.
                // Отсечка снята на входе в бой (DealtAtStart), поэтому «урон вырос»
                // означает «вырос с начала ЭТОГО боя». Оговорка, которую надо назвать:
                // счётчик урона не разделён по жертвам, так что попадание по кому-то
                // другому в том же бою тоже продлит сторожа. У этих спутников иного
                // источника урона нет, но правдой это не станет само собой.
                uint64 mine = Manager::Instance()->BlowsOf(self->GetGUID()).Dealt;
                if (mine > c.VictimHp)
                {
                    c.VictimHp = mine;
                    c.NoDamageMs = 0;       // НАШ урон в ЭТОМ бою есть — считаем заново
                }
                else
                    c.NoDamageMs += slice;
                // ЦЕЛЬ НАБРАНА — БОЙ ОКОНЧЕН, ДАЖЕ ЕСЛИ ПРОТИВНИК ЖИВ.
                //
                // Tiki Target — тренировочный манекен: его скрипт (zone_durotar.cpp) при
                // ударе, который бы его убил, оставляет ему единицу здоровья, выдаёт ОДИН
                // зачёт через KilledMonsterCredit и убирает манекен. То есть цель квеста
                // «побей шестерых» закрывается ударами, а не смертями, и ждать смерти
                // бессмысленно. Проверять надо не флаги существа (Кодекс: PACIFIED и
                // STUNNED описывают состояние цели, а не «зачёт невозможен»), а СЧЁТЧИК
                // ЦЕЛИ — тот же, по которому цель и выбиралась.
                // УМЕНИЕ — ДОБАВКА К АВТОУДАРУ, А НЕ ЗАМЕНА ЕМУ.
                // Автоатака идёт своим чередом в ядре; сюда мы только добавляем то, что
                // спутник и так умеет. Раз в полторы секунды, потому что очередь каста в
                // этой сборке ЗАМЕЩАЕТ предыдущий запрос: чаще — значит не произнести
                // ничего вовсе.
                c.CastMs += slice;
                if (c.CastMs >= 1500)
                {
                    c.CastMs = 0;
                    CastAtTarget(c, self, victim);
                }

                // РАЗ В СЕКУНДУ, А НЕ КАЖДЫЙ ТАКТ: проверка обходит весь журнал заданий и
                // все их цели и строит набор заново, а зачёт не может появиться чаще, чем
                // прилетает удар (Кодекс: не блокер, но лишняя постоянная работа на 122).
                c.WantedCheckMs += slice;
                if (c.WantedCheckMs >= 1000)
                {
                    c.WantedCheckMs = 0;
                    if (!StillWanted(self, victim->GetEntry()))
                    {
                        LogFightOutcome(self, victim, "цель задания набрана", c);
                        Switch(c, self, Behavior::Idle, "цель задания набрана");
                        return;
                    }
                }
                // ТРИДЦАТЬ СЕКУНД, А НЕ ПЯТНАДЦАТЬ (Кодекс, разбор правки времени).
                // Пятнадцати хватает для защиты от зависания, но их же хватает и на
                // ложный отказ: медленное оружие даёт за это время четыре-пять ударов, а
                // ударов на НОЛЬ урона после брони на стенде 21 из 117. Четыре нуля
                // подряд — редкость, но на тысячах боёв редкость случается. Тридцать
                // секунд оставляют запас и всё равно в десять раз быстрее прежнего
                // поведения, при котором сторож не срабатывал НИКОГДА.
                if (c.NoDamageMs > 30000)
                {
                    LogFightOutcome(self, victim, "бью, а следа нет", c);
                    c.Refused.insert(victim->GetGUID());
                    Switch(c, self, Behavior::Idle, "бью, а следа нет — не наша цель");
                    return;
                }
                if (!self->IsWithinMeleeRange(victim))
                {
                    // отошли или цель убежала — догоняем, оставаясь на той же цели
                    StepToward(c, self, victim->GetPositionX(), victim->GetPositionY(),
                        victim->GetPositionZ(), 4.0f, dt);
                    // тупик в бою был единственным, который никто не разбирал: спутник
                    // тёрся о стену до двухминутного срока (Кодекс, 2026-08-30)
                    if (c.Stalled)
                    {
                        LogFightOutcome(self, victim, "до цели в бою не дойти", c);
                        c.Refused.insert(victim->GetGUID());
                        Switch(c, self, Behavior::Idle, "до цели в бою не дойти");
                        return;
                    }
                }
                else
                {
                    // ДОШЛИ: сперва останавливаемся, потом поворачиваемся к цели.
                    // Порядок важен (Кодекс, проход 9): пока догоняем, поворот задаёт
                    // само движение, и отдельный пакет только затирал бы состояние; а
                    // остановка, не сделанная при входе в досягаемость, оставляла бы
                    // «иду вперёд» висеть, пока шагов уже нет.
                    StopMoving(c, self);
                    FaceTarget(c, self, victim);
                }
                // СРОК — ТЕПЕРЬ ПРЕДОХРАНИТЕЛЬ, А НЕ СУДЬЯ.
                //
                // Двух минут не хватало ровно тем боям, которые ШЛИ КАК НАДО. Манекену с
                // двумя тысячами здоровья при полусотне урона за дошедший удар нужно около
                // сорока попаданий; сорок попаданий сами по себе в две минуты уложились бы,
                // но с промахами, паузами и закрытым вентилем — уже нет (Кодекс справедливо
                // поправил слишком уверенную формулировку). Держится это не на расчёте, а на
                // наблюдении: обрывало на 19 % и 41 %, то есть за шаг до зачёта.
                // Бессмысленные бои теперь кончает сторож на пятнадцатой секунде, поэтому
                // сюда доходит только то, что действительно продвигается.
                if (c.ModeMs > 300000)
                {
                    LogFightOutcome(self, victim, "пять минут без исхода", c);
                    Switch(c, self, Behavior::Idle, "пять минут боя без исхода");
                }
                return;
            }
        }
    }

    // ЧЕМ КОНЧИЛСЯ БОЙ — С ИМЕНАМИ И ЧИСЛАМИ.
    //
    // На стенде восемь спутников в Северной Долине выигрывают 94 боя из 102. На боевом
    // те же двоичные файлы дают 4 победы на 65 подходов, 33 смерти и 17 «двух минут без
    // исхода» за семь минут. Значит дело не в коде боя, а в ТОМ, КОГО он выбирает, — а
    // этого в журнале нет: переход пишет «погиб», не говоря, от кого.
    //
    // Проверять догадками дорого: сегодня я уже предположил, что спутники дерутся
    // голыми руками, и замер это отверг — оружие есть у 109 из 122. Поэтому пишем факт:
    // кто, какого уровня, против кого, какого уровня, и сколько у противника осталось
    // здоровья. Доля оставшегося здоровья и есть ответ: 95 % значит «мы не наносим
    // урона», 10 % — «почти выиграли и не дожали».
    // СЛОМАННОЕ СНАРЯЖЕНИЕ — ЭТО НЕ «ПОМЕНЬШЕ УРОНА», А НОЛЬ.
    //
    // Item::IsBroken() — это MaxDurability > 0 && Durability == 0 (Item.h:265), и применение
    // характеристик сломанные вещи ПРОПУСКАЕТ (Player.cpp:8936-8992). Оружие при этом
    // перестаёт существовать и для GetWeaponForAttack(..., true) — тот отдаёт nullptr именно
    // на сломанном (Player.cpp:9646).
    //
    // Считаем не «есть ли оружие», а сколько надетого сломано: спутник в сломанной броне
    // гибнет так же надёжно, как и без оружия.
    // БЬЁТ ИЛИ ЛЕЧИТ — СПРАШИВАТЬ НАДО НА ОДИН УРОВЕНЬ ГЛУБЖЕ.
    //
    // Заклинание-снаряд само урона не несёт: у него эффект «запусти вот это», и весь урон
    // лежит в запускаемом. Frostbolt(116) именно такой, и потому книга мага первого уровня
    // выглядела пустой — не потому что пуста, а потому что прямых бьющих эффектов в ней
    // нет ни одного. На Легионе это уже находили и чинили (задача 0005), и там же терялся
    // Rising Sun Kick у монаха — значит теряется не «маг», а целый ВИД заклинаний у кого
    // угодно.
    //
    // Четыре запускающих эффекта разворачиваются ровно на ОДИН уровень. Глубже намеренно
    // не идём: цепочки бывают, но каждый лишний уровень — ещё один способ принять за
    // оружие то, чем оно не является. Один определитель на все три места, а не три копии
    // массива, которые разъедутся при первой же правке.
    static bool SpellDoes(SpellInfo const* si, bool wantHeal, Difficulty diff, int depth = 1)
    {
        static SpellEffectName const hurting[] = {
            SPELL_EFFECT_SCHOOL_DAMAGE, SPELL_EFFECT_HEALTH_LEECH,
            SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL, SPELL_EFFECT_WEAPON_PERCENT_DAMAGE,
            SPELL_EFFECT_WEAPON_DAMAGE, SPELL_EFFECT_NORMALIZED_WEAPON_DMG };
        static SpellEffectName const healing[] = {
            SPELL_EFFECT_HEAL, SPELL_EFFECT_HEAL_MAX_HEALTH, SPELL_EFFECT_HEAL_PCT };
        static SpellEffectName const triggers[] = {
            SPELL_EFFECT_TRIGGER_SPELL, SPELL_EFFECT_TRIGGER_MISSILE,
            SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE,
            SPELL_EFFECT_TRIGGER_MISSILE_SPELL_WITH_VALUE };

        if (!si)
            return false;
        if (wantHeal)
        {
            for (SpellEffectName e : healing)
                if (si->HasEffect(e))
                    return true;
        }
        else
        {
            for (SpellEffectName e : hurting)
                if (si->HasEffect(e))
                    return true;
        }
        if (depth <= 0)
            return false;
        for (SpellEffectInfo const& eff : si->GetEffects())
        {
            bool trigger = false;
            for (SpellEffectName t : triggers)
                if (eff.IsEffect(t)) { trigger = true; break; }
            if (!trigger || !eff.TriggerSpell)
                continue;
            if (SpellDoes(sSpellMgr->GetSpellInfo(eff.TriggerSpell, diff), wantHeal, diff, depth - 1))
                return true;
        }
        return false;
    }

    // ПАЛИТРА: ЧТО У ЭТОГО КЛАССА ВООБЩЕ ЕСТЬ, ОДИН РАЗ НА СПУТНИКА.
    //
    // Без неё ротацию не спроектировать: нельзя решать, что чем сменять, не видя списка.
    // Пишем ресурс (его вид и запас) и КАЖДОЕ непассивное заклинание книги — с уровнем,
    // стоимостью, временем произнесения, пометкой «бьёт/лечит/никак» и причиной, по
    // которой отбор его не берёт. Именно каждое, включая отвергнутые: дамп, печатающий
    // только прошедшее через фильтр, уже один раз соврал здесь про пустую книгу мага.
    void DumpPalette(Companion& c, Player* self)
    {
        if (c.PaletteDumped)
            return;
        c.PaletteDumped = true;

        Difficulty const diff = self->GetMap()->GetDifficultyID();
        Powers const pw = self->GetPowerType();
        TC_LOG_INFO("server.worldserver",
            "Constellation ПАЛИТРА {} класс {} ур {}: ресурс {} = {}/{}",
            self->GetName(), uint32(self->GetClass()), uint32(self->GetLevel()),
            uint32(pw), self->GetPower(pw), self->GetMaxPower(pw));

        // СЫРОЙ, А НЕ ОТФИЛЬТРОВАННЫЙ. Печатаем КАЖДУЮ непассивную запись книги вместе с
        // причиной, по которой отбор её не берёт. Ровно это правило записано в 0005 после
        // истории с монахом: удобный вид скрыл настоящую запись, и проверявший честно
        // подтвердил пустоту. Пусть лучше в журнале будет лишняя строка, чем вывод «у
        // класса ничего нет», который нечем перепроверить.
        for (auto const& [id, ps] : self->GetSpellMap())
        {
            SpellInfo const* si = sSpellMgr->GetSpellInfo(id, diff);
            if (!si || si->IsPassive())
                continue;

            bool const hurts = SpellDoes(si, false, diff);
            bool const heals = !hurts && SpellDoes(si, true, diff);
            char const* kind = hurts ? "бьёт" : heals ? "лечит" : "никак";

            // ПОЧЕМУ ОТБОР ЭТО НЕ ВОЗЬМЁТ — первая же несданная проверка, в том же порядке.
            char const* why = "годится";
            if (!self->HasActiveSpell(id))                           why = "нет-в-книге";
            else if (!hurts && !heals)                               why = "не-бьёт-не-лечит";
            else if (hurts && si->IsPositive())                      why = "доброе";
            else if (hurts && !si->NeedsExplicitUnitTarget())        why = "без-цели";
            else if (si->IsAffectingArea() || si->IsTargetingArea()) why = "площадь";
            else if (!si->CanBeUsedInCombat(self))                   why = "нельзя-в-бою";
            else if (self->GetSpellHistory()->HasCooldown(si))       why = "откат";

            // стоимость: вид ресурса и сколько. Считает ядро, не я.
            int32 amount = 0; uint32 power = uint32(POWER_MANA);
            for (SpellPowerCost const& cost : si->CalcPowerCost(self, si->GetSchoolMask()))
                if (cost.Amount > 0) { amount = cost.Amount; power = uint32(cost.Power); break; }

            // ФОРМА ЗАКЛИНАНИЯ: номер эффекта, а через «>» — что он запускает, если
            // запускает. Ровно здесь видно разницу, невидимую на экране: у одного
            // прямой эффект урона, у другого эффект «запусти вот это», и урон уже там.
            std::string shape;
            for (SpellEffectInfo const& eff : si->GetEffects())
            {
                if (!shape.empty())
                    shape += ",";
                shape += std::to_string(uint32(eff.Effect));
                if (eff.TriggerSpell)
                    shape += ">" + std::to_string(eff.TriggerSpell);
                if (eff.ApplyAuraName)
                    shape += "/аура" + std::to_string(uint32(eff.ApplyAuraName));
            }

            TC_LOG_INFO("server.worldserver",
                "Constellation ПАЛИТРА {} класс {}: {} закл {} ур {} цена {}/{} каст {} мс [{}] эфф {}",
                self->GetName(), uint32(self->GetClass()), kind, id, si->SpellLevel,
                amount, power, si->CalcCastTime(), why, shape);
        }
    }

    // ТОЧКА, В КОТОРУЮ НАДО ИДТИ, ЧТОБЫ ЗАГОВОРИТЬ — НЕ САМА ЦЕЛЬ.
    //
    // Задача 0013, поставленная оператором 11.08 и до сегодня несделанная: «боты должны
    // использовать внутриигровое расстояние, а не стремиться встать точкой в точку».
    // Симптом, который он описал с экрана утром 01.09: спутник стоит лицом к принимающему,
    // видит его — и не делает последних шагов. Потому что шёл в его координаты, а там
    // стоит он сам.
    //
    // Берём готовое из ядра, как задача и требовала. GetContactPoint строит точку на луче
    // ОТ цели В НАШУ сторону, а GetNearPoint под ним правит высоту по рельефу
    // (UpdateAllowedPositionZ) и, если точка не в прямой видимости, обходит по кругу до
    // видимой. Своей тригонометрией ни того ни другого не получить.
    //
    // Полтора ярда — это уже ПОВЕРХ габаритов: GetNearPoint2D добавляет размеры обоих
    // объектов сам. Порог ядра при этом GetCombatReach() + 4, так что запас честный.
    // СЧИТАЕМ ОДИН РАЗ НА ЦЕЛЬ, А НЕ ЧЕТЫРЕ РАЗА В СЕКУНДУ.
    //
    // GetNearPoint под GetContactPoint при отсутствии прямой видимости ОБХОДИТ цель по
    // кругу мелкими шагами, проверяя видимость на каждом. Замер на живом сервере: вызов
    // на каждом такте у каждого идущего спутника снова упёр мир в 107 % ядра, автомат
    // выдавал одну строку за десять секунд, и все 122 стояли. Это второй раз за сутки,
    // когда я кладу дорогой поиск в горячий такт, — первый был с поиском торговца.
    //
    // Пересчитываем раз в три секунды или при смене цели: NPC за это время далеко не уйдёт.
    void ApproachPoint(Companion& c, WorldObject const* target, Player* self,
                       float& x, float& y, float& z, uint32 diff)
    {
        // ТОЧКА СБОКУ — ВЕЖЛИВОСТЬ, А НЕ ТРЕБОВАНИЕ.
        //
        // GetContactPoint проверяет ВИДИМОСТЬ, а маршрут строится по НАВИГАЦИОННОЙ
        // СЕТКЕ — это разные механизмы, и точка в полутора ярдах сбоку от NPC бывает
        // видна, но недостижима: другой полигон, стена между, неудобная высота. Ровно
        // это и сказал оператор своими словами — «они его видят, но не подходят».
        //
        // Поэтому после первого же отказа построителя идём НА САМОГО NPC. Его
        // собственные координаты по построению лежат на сетке — он там стоит, — и до
        // сегодняшнего утра именно так состав и ходил: 896 побед и 331 сданный квест
        // за ночь. Останавливаемся всё равно за stopAt, так что упереться в него нельзя.
        if (c.ApproachMs)
            c.ApproachMs = (c.ApproachMs <= diff) ? 0 : c.ApproachMs - diff;
        if (c.RawTarget)
        {
            x = target->GetPositionX();
            y = target->GetPositionY();
            z = target->GetPositionZ();
            return;
        }
        if (c.ApproachFor != target->GetGUID() || !c.ApproachMs)
        {
            target->GetContactPoint(self, c.ApproachX, c.ApproachY, c.ApproachZ, 1.5f);
            c.ApproachFor = target->GetGUID();
            c.ApproachMs = 3000;
        }
        x = c.ApproachX; y = c.ApproachY; z = c.ApproachZ;
    }

    // ВЫБОР БОЕВОГО УМЕНИЯ — ВОПРОСАМИ К ЯДРУ, БЕЗ ЕДИНОГО ЗАШИТОГО НОМЕРА.
    //
    // Книга берётся оттуда же, откуда её берёт клиент: GetSpellMap плюс HasActiveSpell —
    // «показывать в книге заклинаний» (Player.h:1896). Годность каждого спрашивается у
    // самого заклинания, а не выводится: не пассивное, не доброе, требует явной цели,
    // не на откате, и на него хватает сил.
    //
    // Порядок — легионовский, записанный в задаче 0005 и уже отработавший однажды:
    // СПЕРВА БЕСПЛАТНЫЕ, потом по уровню заклинания. Он не требует таблиц и сам растёт
    // вместе с персонажем.
    uint32 PickAttackSpell(Player* self, Unit* victim) const
    {
        Difficulty const diff = self->GetMap()->GetDifficultyID();

        // ДВА РАЗДЕЛЬНЫХ ЛУЧШИХ: платное и бесплатное. Внутри каждой группы порядок по
        // уровню заклинания, и это ПРИЗНАННЫЙ СУРРОГАТ — уровень не измеряет силу
        // (Кодекс). Настоящая мера это урон за произнесение, и её надо намерить, а не
        // придумать; до тех пор суррогат честнее случайного выбора.
        uint32 bestPaid = 0, bestPaidLevel = 0;
        uint32 bestFreeId = 0, bestFreeLevel = 0;
        for (auto const& [id, ps] : self->GetSpellMap())
        {
            if (!self->HasActiveSpell(id))
                continue;                       // в книге у клиента этого нет
            SpellInfo const* si = sSpellMgr->GetSpellInfo(id, self->GetMap()->GetDifficultyID());
            if (!si || si->IsPassive() || si->IsPositive())
                continue;                       // пассивное или доброе — не оружие
            if (!si->NeedsExplicitUnitTarget())
                continue;                       // не по цели — не наш случай

            // «НЕ ДОБРОЕ И ПО ЦЕЛИ» — ЭТО ЕЩЁ НЕ ОРУЖИЕ (Кодекс, разбор умений).
            // Под прежний фильтр проходили насмешки, снятия эффектов и что угодно ещё.
            // Требуем настоящего урона в эффектах, пригодности в бою и годности ИМЕННО
            // этой цели — все три ответа даёт ядро.
            // ЧТО ЭТО ТРЕБОВАНИЕ ОТСЕКАЕТ, И ЭТО ИЗВЕСТНАЯ ПОТЕРЯ, А НЕ НЕДОСМОТР.
            // Кодекс: способности-открывашки вроде рывка воина урона в эффектах не несут
            // и сюда не пройдут. Это недостающая возможность, а не опасность, и она
            // осознанно оставлена на потом: лучше спутник без рывка, чем спутник,
            // применивший что-то, чего никто не проверял, на живом сервере.
            if (!SpellDoes(si, false, diff))
                continue;
            if (!si->CanBeUsedInCombat(self))
                continue;

            // ПО ПЛОЩАДИ — НИ В КОЕМ СЛУЧАЕ (Кодекс, второй разбор умений).
            // Заклинание с уроном по площади подтягивает соседние группы, и на боевом это
            // не «чуть хуже», а цепная смерть: спутник собирает на себя пятерых, гибнет,
            // возрождается, идёт туда же. Спрашиваем у ядра оба признака.
            if (si->IsAffectingArea() || si->IsTargetingArea())
                continue;
            if (si->CheckTarget(self, victim, false) != SPELL_CAST_OK)
                continue;

            if (self->GetSpellHistory()->HasCooldown(si))
                continue;

            // ХВАТАЕТ ЛИ СИЛ — СЧИТАЕТ ЯДРО. Своя арифметика здесь стоила бы ровно того же,
            // что стоила арифметика по здоровью моба: правдоподобного и неверного числа.
            bool free = true, affordable = true;
            for (SpellPowerCost const& cost : si->CalcPowerCost(self, si->GetSchoolMask()))
            {
                if (cost.Amount <= 0)
                    continue;
                free = false;
                if (self->GetPower(cost.Power) < cost.Amount)
                    { affordable = false; break; }
            }
            if (!affordable)
                continue;

            if (free)
            {
                if (!bestFreeId || si->SpellLevel > bestFreeLevel)
                    { bestFreeId = id; bestFreeLevel = si->SpellLevel; }
            }
            else
            {
                if (!bestPaid || si->SpellLevel > bestPaidLevel)
                    { bestPaid = id; bestPaidLevel = si->SpellLevel; }
            }
        }
        // ПЛАТНОЕ ВПЕРЕДИ БЕСПЛАТНОГО, и это поправка оператора, а не украшение:
        // «самые дешёвые это автоатаки и всякие жезлы — неверно использовать их в первую
        // очередь; бот должен ТРАТИТЬ ресурс для эффективного уничтожения целей».
        // Прежнее правило говорило ровно обратное и потому не тратило ничего.
        return bestPaid ? bestPaid : bestFreeId;
    }

    // СКОЛЬКО МЕСТА РЕАЛЬНО ЕСТЬ ПОД ДОБЫЧУ.
    //
    // Не GetFreeInventorySlotCount: он начинает счёт с INVENTORY_SLOT_BAG_START, поэтому
    // пустые МЕСТА ПОД СУМКИ идут у него в зачёт как свободные ячейки. Положить туда
    // добычу нельзя — только сумку, — и бот, поверивший этому числу, будет уверен, что
    // место есть, пока ядро молча отказывает в каждом предмете. Ровно это и случилось на
    // Легионе (задача 0008): «4 свободных» и 325 неудачных попыток по одному кобольду.
    //
    // Считаем то, что действительно примет предмет: рюкзак с ITEM_START и содержимое
    // надетых сумок.
    uint32 FreeBagSpace(Player* self) const
    {
        uint32 free = 0;
        uint8 const end = INVENTORY_SLOT_ITEM_START + self->GetInventorySlotCount();
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < end; ++i)
            if (!self->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                ++free;
        for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
            if (Bag* bag = self->GetBagByPos(i))
                for (uint32 j = 0; j < GetBagSize(bag); ++j)
                    if (!GetItemInBag(bag, j))
                        ++free;
        return free;
    }

    // КОМУ МОЖНО ПРОДАТЬ И У КОГО ПОЧИНИТЬСЯ — СПРАШИВАЕМ У ЗАГРУЖЕННОЙ ОКРУГИ.
    //
    // Той же связкой, которой модуль уже дважды ищет существ: поиск по сетке плюс флаг.
    // Своего радиуса взаимодействия не выдумываем — его позже проверит само ядро; сто
    // ярдов здесь это радиус ПОИСКА, то есть «стоит ли вообще идти», а не «можно ли
    // торговать». Предпочитаем того, кто умеет и то и другое: один поход вместо двух.
    // needSell/needRepair — ЗАЧЕМ идём. Кодекс: без этого бот с полными сумками мог уйти
    // к ремонтнику, ничего не продать, поставить минутный таймер и вернуться — и так по
    // кругу вечно. Услуга, за которой идём, теперь обязательна, а вторая — приятный бонус.
    Creature* FindVendorNear(Player* self, bool needSell, bool needRepair) const
    {
        Creature* both = nullptr; float bothDist = 100000.0f;
        Creature* any = nullptr;  float anyDist = 100000.0f;
        std::list<Creature*> near;
        Trinity::AnyUnitInObjectRangeCheck check(self, 100.0f);
        Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, near, check);
        Cell::VisitGridObjects(self, searcher, 100.0f);
        for (Creature* cr : near)
        {
            if (!cr->IsAlive())
                continue;
            bool const sells = cr->HasNpcFlag(UNIT_NPC_FLAG_VENDOR);
            bool const fixes = cr->HasNpcFlag(UNIT_NPC_FLAG_REPAIR);
            // ГОДИТСЯ, ТОЛЬКО ЕСЛИ УМЕЕТ ТО, ЗАЧЕМ ИДЁМ.
            if (needSell && !sells)
                continue;
            if (needRepair && !fixes)
                continue;
            if (!sells && !fixes)
                continue;
            if (!self->IsValidAssistTarget(cr) && self->IsValidAttackTarget(cr))
                continue;               // враждебный — торговать не станет
            float const d = self->GetExactDist(cr);
            if (sells && fixes)
                { if (d < bothDist) { bothDist = d; both = cr; } }   // один поход вместо двух
            else if (d < anyDist)
                { anyDist = d; any = cr; }
        }
        return both ? both : any;
    }

    // ПРОДАТЬ ХЛАМ — ПО ОДНОМУ ПРЕДМЕТУ, ПОТОМУ ЧТО ПАЧКОЙ ЭТА СБОРКА НЕ УМЕЕТ.
    //
    // Кодекс советовал CMSG_SELL_ALL_JUNK_ITEMS: ядро само знает, что серое — это мусор,
    // и само применяет запреты. Проверил в собираемом дереве (rev e861aa8ed2): опкод там
    // привязан к Handle_NULL и не делает НИЧЕГО. Обработчик есть только в более новом
    // upstream, который он читал. Поэтому перебираем сами.
    //
    // ЧТО ПРОДАЁМ: только серое, только с ненулевой ценой и только не нужное заданию.
    // Ядро проверит своё (чужой предмет, непустая сумка, открытый лут, возвратный), но
    // «это ещё пригодится» оно за нас не решит — это наша обязанность.
    uint32 SellJunkTo(Companion& c, Player* self, Creature* vendor)
    {
        std::vector<Item*> junk;
        auto consider = [&](Item* it)
        {
            if (!it || it->IsBag())
                return;
            ItemTemplate const* tpl = it->GetTemplate();
            if (!tpl || tpl->GetQuality() != ITEM_QUALITY_POOR)
                return;
            if (!tpl->GetSellPrice())
                return;                 // ядро такое всё равно откажется купить
            if (self->HasQuestForItem(tpl->GetId()))
                return;                 // нужное заданию не продаём никогда
            junk.push_back(it);
        };
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            consider(self->GetItemByPos(INVENTORY_SLOT_BAG_0, i));
        for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
            if (Bag* bag = self->GetBagByPos(i))
                for (uint32 j = 0; j < GetBagSize(bag); ++j)
                    consider(GetItemInBag(bag, j));

        uint32 sold = 0, refused = 0;
        uint64 const before = self->GetMoney();
        for (Item* it : junk)
        {
            // УСПЕХ — ЭТО ИСЧЕЗНОВЕНИЕ ПРЕДМЕТА, А НЕ ОТПРАВКА ПАКЕТА.
            //
            // Кодекс: прежний счёт увеличивался после КАЖДОГО вызова обработчика, поэтому
            // отвергнутый предмет записывался проданным, и журнал мог сказать «продал 3,
            // выручил 0». Спрашиваем ядро, лежит ли предмет ещё у нас.
            ObjectGuid const itemGuid = it->GetGUID();
            WorldPacket raw(CMSG_SELL_ITEM);
            WorldPackets::Item::SellItem sell(std::move(raw));
            sell.VendorGUID = vendor->GetGUID();
            sell.ItemGUID = itemGuid;
            sell.Amount = it->GetCount();
            c.Session->HandleSellItemOpcode(sell);
            if (self->GetItemByGuid(itemGuid))
                ++refused;              // остался у нас — значит не продан
            else
                ++sold;
        }
        uint64 const earned = self->GetMoney() > before ? self->GetMoney() - before : 0;
        c.VendSold += sold;
        c.VendEarned += earned;
        if (sold || refused)
            TC_LOG_INFO("server.worldserver",
                "Constellation ТОРГ {}: продал {} предметов у {} ({}), выручил {} медяков; "
                "отвергнуто ядром {}",
                self->GetName(), sold, vendor->GetName(), vendor->GetEntry(), earned, refused);
        return sold;
    }

    // ПОЧИНИТЬСЯ — ПАКЕТОМ, А НЕ ПРЯМЫМ ВЫЗОВОМ.
    //
    // Пустой ItemGUID означает «починить всё», ровно как кнопка клиента. Обработчик сам
    // найдёт ремонтника, проверит флаг, вражду, бой и дистанцию и применит скидку за
    // репутацию — ничего из этого прямой вызов не делает.
    //
    // ДЕНЕГ НЕ ХВАТИЛО — ЯДРО МОЛЧА НЕ ЧИНИТ НИЧЕГО (Кодекс: считает полную стоимость и
    // выходит). Поэтому сверяем прочность до и после и пишем, если ничего не изменилось:
    // иначе «починился» было бы утверждением, а не фактом.
    bool RepairAt(Companion& c, Player* self, Creature* vendor)
    {
        if (!vendor->HasNpcFlag(UNIT_NPC_FLAG_REPAIR))
            return false;

        // НЕЧЕГО ЧИНИТЬ — НЕ ШЛЁМ ПАКЕТ ВОВСЕ.
        //
        // Найдено испытанием: ремонт при целом снаряжении всё равно списывает МОНЕТУ —
        // CalculateDurabilityRepairCost округляет нулевую стоимость до единицы. Один медяк
        // мелочь, но на 122 спутниках это постоянная утечка на пустом месте, а в журнале
        // при этом появлялось «ремонт не прошёл», хотя чинить было нечего. Два разных
        // случая нельзя писать одним словом.
        if (!BrokenCount(self) && !DamagedCount(self))
            return true;

        uint32 const brokenBefore = BrokenCount(self);
        uint32 const damagedBefore = DamagedCount(self);
        uint64 const moneyBefore = self->GetMoney();

        WorldPacket raw(CMSG_REPAIR_ITEM);
        WorldPackets::Item::RepairItem fix(std::move(raw));
        fix.NpcGUID = vendor->GetGUID();
        fix.ItemGUID = ObjectGuid::Empty;   // пусто = «починить всё»
        fix.UseGuildBank = false;
        c.Session->HandleRepairItemOpcode(fix);

        uint32 const brokenAfter = BrokenCount(self);
        uint32 const damagedAfter = DamagedCount(self);
        uint64 const spent = moneyBefore > self->GetMoney() ? moneyBefore - self->GetMoney() : 0;
        // УСПЕХ МЕРЯЕТСЯ ПРОЧНОСТЬЮ, А НЕ ДЕНЬГАМИ (Кодекс: brokenAfter уже посчитан и не
        // использован). Деньги отвечают на вопрос «сколько стоило», а не «получилось ли»:
        // нулевой расход бывает и при бедности, и при отказе, и при нулевой стоимости.
        bool const better = brokenAfter < brokenBefore || damagedAfter < damagedBefore;
        if (!better)
        {
            ++c.VendPoor;
            TC_LOG_INFO("server.worldserver",
                "Constellation ТОРГ {}: ремонт НЕ прошёл у {} ({}) — сломано {}, изношено {}, "
                "денег {}, списано {}; ядро чинит всё или ничего",
                self->GetName(), vendor->GetName(), vendor->GetEntry(),
                brokenBefore, damagedBefore, moneyBefore, spent);
            return false;
        }
        ++c.VendRepaired;
        TC_LOG_INFO("server.worldserver",
            "Constellation ТОРГ {}: починился у {} ({}) за {} медяков; сломано {} -> {}, "
            "изношено {} -> {}",
            self->GetName(), vendor->GetName(), vendor->GetEntry(), spent,
            brokenBefore, brokenAfter, damagedBefore, damagedAfter);
        return true;
    }

    // ОБОБРАТЬ ТРУП — ЧЕТЫРЬМЯ ПАКЕТАМИ, В ТОМ ЖЕ ПОРЯДКЕ, ЧТО ШЛЁТ КЛИЕНТ.
    //
    // Возвращает true, если с этим трупом закончили (успешно или нет) — вызывающий тогда
    // забывает его. Всё делается за ОДИН заход: спутник после ближнего боя уже стоит
    // вплотную, а ходьбу к трупу первая версия не умеет и честно считает, сколько раз она
    // была бы нужна.
    bool LootFromCorpse(Companion& c, Player* self)
    {
        Creature* corpse = ObjectAccessor::GetCreature(*self, c.LootTarget);
        if (!corpse || corpse->IsAlive())
            return true;                        // исчез или воскрес — забыть

        // ДИСТАНЦИЮ СПРАШИВАЕМ У ЯДРА ТЕМ ЖЕ ЧИСЛОМ, что применяет обработчик лута
        // (LootHandler.cpp: 30 ярдов от трупа). Своего числа не выдумываем.
        if (!self->IsWithinDistInMap(corpse, 30.0f))
        {
            ++c.LootTooFar;
            TC_LOG_INFO("server.worldserver",
                "Constellation ЛУТ {}: не дотянулся до {} ({}), {:.1f} ярдов",
                self->GetName(), corpse->GetName(), corpse->GetEntry(),
                self->GetDistance(corpse));
            return true;
        }

        // ПРАВО НА ЛУТ РЕШАЕТ ЯДРО. isAllowedToLoot — тот же предикат, которым обработчик
        // отсеивает чужую добычу, метку другой группы, розыгрыш и мастер-лут. Спрашиваем
        // его сами, чтобы не слать пакет, который заведомо отвергнут.
        if (!self->isAllowedToLoot(corpse))
        {
            ++c.LootDenied;
            TC_LOG_INFO("server.worldserver",
                "Constellation ЛУТ {}: ядро не дало обобрать {} ({})",
                self->GetName(), corpse->GetName(), corpse->GetEntry());
            return true;
        }

        // 1. ОТКРЫТЬ. Без этого обработчик предмета не найдёт лут в m_AELootView.
        {
            WorldPacket raw(CMSG_LOOT_UNIT);
            WorldPackets::Loot::LootUnit open(std::move(raw));
            open.Unit = c.LootTarget;
            c.Session->HandleLootOpcode(open);
        }
        if (self->GetAELootView().empty())
        {
            ++c.LootDenied;                     // ядро отказало молча — не настаиваем
            TC_LOG_INFO("server.worldserver",
                "Constellation ЛУТ {}: открыл {} ({}), но вид лута пуст",
                self->GetName(), corpse->GetName(), corpse->GetEntry());
            return true;
        }
        ++c.LootOpened;

        // 2. ДЕНЬГИ — если они там есть. Пакет без GUID: обработчик берёт из всего
        //    открытого вида сразу, поэтому шлём его один раз.
        bool anyGold = false;
        for (auto const& [lootGuid, loot] : self->GetAELootView())
            if (loot && loot->gold)
                { anyGold = true; c.LootMoney += loot->gold; }
        if (anyGold)
        {
            WorldPacket raw(CMSG_LOOT_MONEY);
            WorldPackets::Loot::LootMoney money(std::move(raw));
            c.Session->HandleLootMoneyOpcode(money);
        }

        // 3. ПРЕДМЕТЫ. Собираем запросы по всему открытому виду и шлём ОДНИМ пакетом:
        //    он и рассчитан на список (Array<LootRequest, 100>).
        //
        //    ЧТО БЕРЁМ, И ПОЧЕМУ ИМЕННО ЭТО: нужное текущим заданиям — иначе задание не
        //    закроется; и серый хлам — его ядро само считает мусором и умеет продавать.
        //    Всё остальное пока мимо: чтобы решать про зелёное и выше, нужна логика
        //    сравнения с надетым, а её нет, и подобранная привязка необратима.
        WorldPacket rawItems(CMSG_LOOT_ITEM);
        WorldPackets::Loot::LootItem take(std::move(rawItems));
        uint32 const freeSlots = FreeBagSpace(self);
        uint32 asked = 0;
        for (auto const& [lootGuid, loot] : self->GetAELootView())
        {
            if (!loot)
                continue;
            for (LootItem const& item : loot->items)
            {
                if (item.is_looted || item.is_blocked)
                    continue;
                if (!item.HasAllowedLooter(self->GetGUID()))
                    continue;
                ItemTemplate const* tpl = sObjectMgr->GetItemTemplate(item.itemid);
                if (!tpl)
                    continue;
                bool const forQuest = item.needs_quest || self->HasQuestForItem(item.itemid);
                bool const junk = tpl->GetQuality() == ITEM_QUALITY_POOR;

                // СЫРАЯ ЗАПИСЬ: что лежало и почему не взяли. Без неё «предметов 0»
                // неотличимо от «фильтр всё съел».
                char const* skip = nullptr;
                if (!forQuest && !junk)                  skip = "не-квест-и-не-хлам";
                // Кодекс: было asked + 1 >= freeSlots, и одна свободная ячейка
                // не использовалась никогда — при freeSlots == 1 отказывали даже первому предмету.
                else if (!forQuest && asked + 2 > freeSlots) skip = "мало-места-под-хлам";
                else if (asked >= freeSlots)            skip = "сумки-полны";
                TC_LOG_INFO("server.worldserver",
                    "Constellation ЛУТ-СОДЕРЖИМОЕ {}: предмет {} кач {} кол {} квест {} "
                    "свободно {} (ядро говорит {}) -> {}",
                    self->GetName(), item.itemid, uint32(tpl->GetQuality()), item.count,
                    forQuest ? "да" : "нет", freeSlots,
                    self->GetFreeInventorySlotCount(), skip ? skip : "БЕРЁМ");
                if (skip)
                {
                    if (asked >= freeSlots)
                        break;
                    continue;
                }
                WorldPackets::Loot::LootRequest& req = take.Loot.emplace_back();
                req.Object = lootGuid;          // КЛЮЧ вида — это и есть GUID объекта лута
                req.LootListID = uint8(item.LootListId);   // НЕ номер в списке
                ++asked;
            }
        }
        if (asked)
        {
            // СЧИТАЕМ ВЗЯТОЕ, А НЕ ЗАПРОШЕННОЕ. Тот же урок, что и в продаже: ядро может
            // отказать (переполнение стопки, уникальность, полные сумки), и запрос не
            // равен предмету в рюкзаке. Меряем по свободному месту до и после — это
            // ровно то, что изменилось бы, если предмет действительно лёг.
            uint32 const spaceBefore = FreeBagSpace(self);
            c.Session->HandleAutostoreLootItemOpcode(take);
            uint32 const spaceAfter = FreeBagSpace(self);
            uint32 const landed = spaceBefore > spaceAfter ? spaceBefore - spaceAfter : 0;
            c.LootItems += landed;
            if (landed != asked)
                TC_LOG_INFO("server.worldserver",
                    "Constellation ЛУТ {}: запрошено {}, легло {} — остальное ядро не приняло "
                    "(или ушло в существующие стопки, что места не занимает)",
                    self->GetName(), asked, landed);
        }

        TC_LOG_INFO("server.worldserver",
            "Constellation ЛУТ {}: обобрал {} ({}) — денег {}, предметов запрошено {}; "
            "за всё время трупов {}, предметов {}, денег {}",
            self->GetName(), corpse->GetName(), corpse->GetEntry(),
            anyGold ? "да" : "нет", asked,
            c.LootOpened, c.LootItems, c.LootMoney);

        // 4. ОТПУСТИТЬ. Иначе вид остаётся открытым и следующий труп не откроется.
        {
            WorldPacket raw(CMSG_LOOT_RELEASE);
            WorldPackets::Loot::LootRelease done(std::move(raw));
            done.Unit = c.LootTarget;
            c.Session->HandleLootReleaseOpcode(done);
        }
        return true;
    }

    // САМОЛЕЧЕНИЕ: ТОТ ЖЕ ОБХОД КНИГИ, НО ЛЕЧАЩЕЕ И НА СЕБЯ.
    //
    // Порог 35 % — первое защитимое правило (Кодекс), и он же назвал, чем его заменить
    // потом: лечиться, если «здоровье ÷ входящий урон» меньше времени произнесения плюс
    // запас. Это расчёт, а не константа, и для него нужны счётчики, которых пока нет.
    //
    // ЧЕГО ЭТО НЕ ВИДИТ, И ЭТО НАЗВАНО, А НЕ УМОЛЧАНО: лечение через ауру. Обновление у
    // жреца и Возрождение у друида лечат периодической аурой, а не эффектом лечения, и в
    // три искомых эффекта не попадают — как Frostbolt не попадал в шесть бьющих. Разница
    // в том, что здесь это осознанный предел первой версии (Кодекс: «прямое
    // самолечение»), а не сюрприз: постепенное лечение при 35 % здоровья и так почти
    // всегда опаздывает. Расширять — вместе со счётчиками вылеченного и перелеченного,
    // иначе нечем будет доказать, что стало лучше.
    uint32 PickSelfHeal(Player* self) const
    {
        Difficulty const diff = self->GetMap()->GetDifficultyID();
        uint32 best = 0, bestLevel = 0;
        for (auto const& [id, ps] : self->GetSpellMap())
        {
            if (!self->HasActiveSpell(id))
                continue;
            SpellInfo const* si = sSpellMgr->GetSpellInfo(id, diff);
            if (!si || si->IsPassive())
                continue;
            if (!SpellDoes(si, true, diff))
                continue;
            if (si->IsAffectingArea() || si->IsTargetingArea())
                continue;
            if (!si->CanBeUsedInCombat(self))
                continue;
            if (si->CheckTarget(self, self, false) != SPELL_CAST_OK)
                continue;               // на себя не ложится — не наш случай
            if (self->GetSpellHistory()->HasCooldown(si))
                continue;
            bool affordable = true;
            for (SpellPowerCost const& cost : si->CalcPowerCost(self, si->GetSchoolMask()))
                if (cost.Amount > 0 && self->GetPower(cost.Power) < cost.Amount)
                    { affordable = false; break; }
            if (!affordable)
                continue;
            if (!best || si->SpellLevel > bestLevel)
                { best = id; bestLevel = si->SpellLevel; }
        }
        return best;
    }

    // ПРОИЗНЕСТИ — ТЕМ ЖЕ ОПКОДОМ, ЧТО И КЛИЕНТ.
    //
    // Возвращает true, если ядро оставило след: заклинание идёт или встал откат. Ответа
    // ждать неоткуда — сокета нет, — поэтому проверяем ПОСЛЕДСТВИЕ, как и везде в модуле.
    bool CastAtTarget(Companion& c, Player* self, Unit* victim)
    {
        DumpPalette(c, self);

        bool const castingNow = self->HasUnitState(UNIT_STATE_CASTING);
        c.WasCasting = castingNow;

        // НЕ ПРОСИМ КАСТ ПОВЕРХ КАСТА. Ядро такую просьбу не отклоняет — оно её ОТКЛАДЫВАЕТ,
        // а следующая замещает отложенную (Player.cpp:30891). Просить каждый такт значит
        // восемь раз за двухсекундное заклинание выбросить собственный же запрос.
        if (castingNow)
            { ++c.CastsBusy; return false; }

        // РАНЕН — ЛЕЧИСЬ. Цель тогда мы сами, а не противник.
        Unit* castTarget = victim;
        uint32 spellId = 0;
        if (self->GetHealthPct() <= 35.0f)
            if (uint32 heal = PickSelfHeal(self))
                { spellId = heal; castTarget = self; }
        if (!spellId)
            spellId = PickAttackSpell(self, victim);
        if (!spellId)
            return false;
        SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId, self->GetMap()->GetDifficultyID());
        if (!si)
            return false;
        // очередь этой сборки: если она не примет, слать бессмысленно (Player.cpp:30922)
        if (!self->CanRequestSpellCast(si, self))
            return false;

        // ВЫБРАЛИ — ЗАПИСАЛИ, НО КАЖДУЮ ПАРУ «СПУТНИК + ЗАКЛИНАНИЕ» РОВНО ОДИН РАЗ.
        //
        // Кодекс: ограничения «сменился номер» мало — при чередовании умений это до
        // восьмидесяти строк в секунду на 122 спутниках. Сегодня уже видно, чего стоит
        // потоп в журнале на этом сервере: 14 МиБ в минуту от служебных строк ядра.
        // Набор конечен по своей природе — у спутника несколько боевых умений, — поэтому
        // журнал соберёт ровно нужный список и замолчит сам.
        c.LastSpell = spellId;
        if (c.SpellsLogged.insert(spellId).second)
        {
            TC_LOG_INFO("server.worldserver",
                "Constellation УМЕНИЕ {} (класс {}, ур {}) выбрал заклинание {} против {} ({}){}",
                self->GetName(), uint32(self->GetClass()), uint32(self->GetLevel()),
                spellId, castTarget->GetName(), castTarget->GetEntry(),
                Cfg().Abilities ? "" : " — ТОЛЬКО ВЫБОР, произнесение выключено");
        }
        if (!Cfg().Abilities)
            return false;

        // ОБЩИЙ ОТКАТ СПРАШИВАЕМ У ЯДРА ТЕМ ЖЕ ВОПРОСОМ, что задаёт себе оно само
        // (CanExecutePendingSpellCastRequest). Своя арифметика по времени здесь стоила бы
        // ровно того же, что стоила своя арифметика по здоровью и по прочности.
        if (self->GetSpellHistory()->GetRemainingGlobalCooldown(si) > 0ms)
            { ++c.CastsBusy; return false; }

        ++c.CastsTried;
        WorldPacket raw(CMSG_CAST_SPELL);
        WorldPackets::Spells::CastSpell cast(std::move(raw));
        // ИДЕНТИФИКАТОР КАСТА ЛЕПИМ ТАК ЖЕ, КАК ЕГО ЛЕПИТ САМО ЯДРО (Unit.cpp:12307).
        cast.Cast.CastID = ObjectGuid::Create<HighGuid::Cast>(SPELL_CAST_SOURCE_NORMAL,
            self->GetMapId(), spellId, self->GetMap()->GenerateLowGuid<HighGuid::Cast>());
        cast.Cast.SpellID = int32(spellId);
        cast.Cast.Target.Flags = TARGET_FLAG_UNIT;
        cast.Cast.Target.Unit = castTarget->GetGUID();
        // MoveUpdate НЕ ЗАПОЛНЯЕМ: обработчик при нём прогоняет CMSG_MOVE_STOP через
        // HandleMovementOpcode, а тот ЗАМЕЩАЕТ всё состояние движения — ровно та ловушка,
        // на которой пришлось разбираться с поворотом к цели.
        c.Session->HandleCastSpellOpcode(cast);

        // СЛЕД УСПЕХА — ОБЩИЙ ОТКАТ, а не «идёт ли заклинание».
        //
        // Прежний признак был верен только для читаемых. Мгновенное умение исполняется и
        // исчезает в тот же миг, своего отката у Sinister Strike, Slam и Crusader Strike
        // нет, — и все три засчитывались как непрошедшие. Общий откат стартует в обоих
        // случаях, и это тот же вопрос, которым ядро само решает, можно ли исполнять
        // следующий запрос.
        if (self->GetSpellHistory()->GetRemainingGlobalCooldown(si) > 0ms
            || self->GetCurrentSpell(CURRENT_GENERIC_SPELL)
            || self->GetSpellHistory()->HasCooldown(si))
        {
            ++c.CastsWent;
            return true;
        }
        return false;
    }

    // ЕДИНСТВЕННОЕ МЕСТО, ГДЕ РЕШАЕТСЯ «МОЖНО ЛИ НАМ ДРАТЬСЯ».
    //
    // Первая версия запрещала только ВЫБОР существа, и Кодекс показал, что это не запрет:
    // спутник продолжал идти к боевой точке задания и ловил там агро близостью, а уже
    // начатый подход к цели не прерывался вовсе. Круг смерти оставался, просто шёл медленнее.
    //
    // ЧТО ИМЕННО ЗАКРЫТО, без преувеличений: выбор существа, выход в дорогу к боевой точке
    // и уже начатый подход к цели. НЕ закрыты два случая, и оба намеренно: уже идущий бой
    // (цель бьёт в ответ, а выхода из боя как поведения пока нет) и уже начатая дорога
    // (обычный путь к поломке — смерть, а она и так возвращает в «стою»). Оба выхода ведут
    // в «стою», откуда новый бой уже не начать.
    bool BrokenForFight(Companion& c, Player* self) const
    {
        uint32 broken = BrokenCount(self);
        if (!broken)
        {
            c.BrokenNoted = false;
            return false;
        }
        if (!c.BrokenNoted)
        {
            c.BrokenNoted = true;
            TC_LOG_INFO("server.worldserver",
                "Constellation: {} не идёт в бой — сломано вещей: {}", self->GetName(), broken);
        }
        return true;
    }

    // ПОЧИНИТЬ, ЕСЛИ ЕСТЬ ЧТО. Возвращает, сколько надетого было сломано ДО починки, —
    // ноль означает, что вызывать ядро не пришлось вовсе.
    // ВТОРОЙ ДОВОД ЗА ФЛАГ, помимо инварианта: даровой ремонт скрывает саму нужду в
    // деньгах. Пока чинят бесплатно, невозможно узнать, хватает ли спутнику выручки на
    // собственное содержание, — а это и есть вопрос, ради которого заводилась торговля.
    uint32 RepairIfBroken(Player* self, char const* why, bool operatorAsked = false) const
    {
        if (!self || !self->IsInWorld())
            return 0;
        if (Cfg().Vending && !operatorAsked)
            return 0;                   // чинимся у ремонтника за деньги, а не даром
        uint32 broken = BrokenCount(self);
        if (!broken)
            return 0;
        self->DurabilityRepairAll(false, 0.0f, false);
        TC_LOG_INFO("server.worldserver", "Constellation: {} — починено {} вещей ({})",
            self->GetName(), broken, why);
        return broken;
    }

    uint32 BrokenCount(Player* self) const
    {
        uint32 broken = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* it = self->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (it->IsBroken())
                    ++broken;
        return broken;
    }

    // ИЗНОШЕННОЕ — ЭТО НЕ СЛОМАННОЕ, И ЧИНИТЬ НАДО РАНЬШЕ.
    //
    // Кодекс: поход запускался только при BrokenCount > 0, то есть при вещах с НУЛЕВОЙ
    // прочностью. Изношенное, но ещё работающее снаряжение не чинилось никогда — а это,
    // как он верно замечает, гораздо более частое состояние, чем полная поломка. Ждать
    // нуля значит ждать, пока оружие перестанет наносить урон вовсе.
    //
    // Половина — ПОРОГ, А НЕ ВЫВОД. Ядро своего «пора чиниться» не определяет, поэтому
    // число выбрано, а не найдено; названо здесь, чтобы его можно было оспорить.
    uint32 DamagedCount(Player* self) const
    {
        uint32 damaged = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* it = self->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                // Читаем те же два поля, что читает Item::IsBroken (Item.h:265) — своих
                // имён не выдумываем, старые ITEM_FIELD_* в этой версии не существуют.
                uint32 const maxDur = *it->m_itemData->MaxDurability;
                if (!maxDur)
                    continue;           // прочности нет вовсе — нечего изнашивать
                if (uint32(*it->m_itemData->Durability) * 2 < maxDur)
                    ++damaged;
            }
        return damaged;
    }

    // МОГУ ЛИ Я ВООБЩЕ НАНОСИТЬ УРОН — один вопрос вместо «сломан ли» и «изношен ли».
    //
    // GetWeaponForAttack(..., useable=true) возвращает nullptr И на сломанном оружии, И на
    // пустой руке — то есть ровно в обоих случаях, когда бить нечем. Прежние счётчики
    // пустую руку молча пропускали, и спутник без оружия считался исправным.
    //
    // ПРЕДЕЛ НАЗВАН: заклинателю оружие для заклинания не нужно, и он тут будет признан
    // «не могущим бить» слишком рано. Это осознанно: без оружия он всё равно теряет и
    // автоудар, и жезл, а различать классы по книге заклинаний здесь — это тот же обход
    // книги на каждом такте, от которого мы уже отказались в другом месте.
    bool CannotFight(Player* self) const
    {
        return self->GetWeaponForAttack(BASE_ATTACK, true) == nullptr;
    }

    void LogFightOutcome(Player* self, Unit* victim, char const* how, Companion& c)
    {
        // Пишем и БЕЗ указателя на цель: ядро обнуляет жертву при её смерти, а это как
        // раз тот случай, который надо записать. Имя и запись берём из памяти о бое.
        Manager::Blows const b = Manager::Instance()->BlowsOf(self->GetGUID());
        TC_LOG_INFO("server.worldserver",
            "Constellation БОЙ {} (эфф ур {}) против {} ({}): {} — у него {}, у нас {:.0f}%; "
            "за {} с: замахов {}, урон прошёл {} раз на {}, вничью {} раз; умений {} из {} (закл {}, занят {}, цель умерла под каст {}); по нам {} на {}; "
            "тактов {} (уклоняется {}, занят {}, не в бою {}, вне досягаемости {}, вне сектора {}, "
            "таймер не готов {}); подмен цели {}",
            self->GetName(), uint32(self->GetEffectiveLevel()),
            c.FightVictimName.empty() ? (victim ? victim->GetName() : "?") : c.FightVictimName,
            c.FightVictimEntry, how,
            victim ? Trinity::StringFormat("{:.0f}%", victim->GetHealthPct()) : std::string("мёртв/исчез"),
            self->GetHealthPct(),
            c.ModeMs / 1000,
            b.Swings - c.SwingsAtStart, b.Landed - c.LandedAtStart, b.Dealt - c.DealtAtStart,
            b.Zeroed - c.ZeroedAtStart,
            c.CastsWent, c.CastsTried, c.LastSpell, c.CastsBusy, c.CastsDiedUnder,
            b.Hits - c.HitsAtStart, b.Taken - c.TakenAtStart,
            c.GateTicks, c.GateEvading, c.GateBusy, c.GateNoState, c.GateOutOfRange, c.GateBadFacing,
            c.GateNotReady, c.VictimSwaps);
    }

    // Переход — ЕДИНСТВЕННОЕ место, где пишется строка. Потактовая запись однажды
    // дала 31 миллион строк; переходов у спутника единицы в минуту.
    void Switch(Companion& c, Player* self, Behavior to, char const* why)
    {
        if (c.Mode == to)
            return;
        TC_LOG_INFO("server.worldserver", "Constellation FSM {}: {} -> {} ({})",
            self->GetName(), ModeName(c.Mode), ModeName(to), why);
        if (Player* me = c.Session ? c.Session->GetPlayer() : nullptr)
        {
            StopMoving(c, me);          // сменили намерение — ноги остановились
            // И РУКИ ТОЖЕ. Switch менял только намерение, поэтому «ушёл из боя» было
            // правдой лишь внутри модуля: ядро продолжало автоатаку по прежней цели, а
            // «стою» тут же выбирало её снова (Кодекс). Уходим тем же опкодом, каким
            // это делает клиент: CMSG_ATTACK_STOP -> Player::AttackStop().
            c.EngageRange = 0.0f;           // новая цель — новая дистанция боя
        if (c.GiverUnreachable.size() > 40)
            c.GiverUnreachable.clear();  // список не должен расти без предела
        if (c.Mode == Behavior::Attacking && me->GetVictim())
            {
                WorldPacket raw(CMSG_ATTACK_STOP);
                WorldPackets::Combat::AttackStop stop(std::move(raw));
                c.Session->HandleAttackStopOpcode(stop);
            }
        }
        c.Mode = to;
        c.ModeMs = 0;
        if (to != Behavior::TurningIn)
            c.TurnInGuid.Clear();       // другое намерение — найденный принимающий не наш
        if (to != Behavior::Attacking)
            { c.VictimHp = 0; c.NoDamageMs = 0; c.DamageVictim.Clear(); }
        c.Stalled = false;              // новое намерение — новая попытка дойти
        c.StuckMs = 0;
        c.UnstickTries = 0;
        c.NoPathFails = 0;              // и новая цель — отступ по маршруту снимается
        c.NoPathMs = 0;
        c.RawTarget = false;
        c.UnstickTotal = 0;
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
            case Behavior::Recovering:        return "перевожу дух";
            case Behavior::Vending:           return "иду к торговцу";
            case Behavior::Travelling:        return "иду к месту задания";
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

        FaceTarget(c, self, target);        // иначе ядро ответит BadFacing и не ударит
        WorldPacket rawSwing(CMSG_ATTACK_SWING);
        WorldPackets::Combat::AttackSwing swing(std::move(rawSwing));
        swing.Victim = target->GetGUID();
        c.Session->HandleAttackSwingOpcode(swing);

        // проверяем ПОСЛЕДСТВИЕ, а не факт вызова: сокета нет, ответа не будет
        if (self->GetVictim() == target)
        {
            ++_fightsStarted;
            // ОТСЕЧКА: всё, что насчитается дальше, относится ИМЕННО к этому бою.
            // Register заводит запись, если её ещё нет; обработчики урона делают только
            // find(), поэтому персонажи оператора в счёт не попадают.
            Manager::Blows const b = Manager::Instance()->RegisterAndSnapshot(self->GetGUID());
            c.SwingsAtStart = b.Swings;
            c.LandedAtStart = b.Landed;
            c.ZeroedAtStart = b.Zeroed;
            c.DealtAtStart  = b.Dealt;
            c.HitsAtStart   = b.Hits;
            c.TakenAtStart  = b.Taken;
            c.KillsAtStart  = b.Kills;
            c.GateTicks = c.GateEvading = c.GateBusy = 0;
            c.GateNoState = c.GateOutOfRange = c.GateBadFacing = 0;
            c.GateNotReady = c.VictimSwaps = 0;
            c.CastMs = 1500;            // первое решение — сразу, а не через полторы секунды
            c.CastsTried = c.CastsWent = c.LastSpell = 0;
            c.CastsBusy = c.CastsDiedUnder = 0;
            c.WasCasting = false;
            c.DamageVictim = target->GetGUID();
            c.VictimHp = b.Dealt;       // отсечка сторожа: урон НА НАЧАЛО этого боя
            c.NoDamageMs = 0;
            c.FightVictim = target->GetGUID();
            c.FightVictimName = target->GetName();
            c.FightVictimEntry = target->GetEntry();
            // ХАРАКТЕРИСТИКА ЦЕЛИ В МОМЕНТ НАЧАЛА БОЯ. Исход пишется не всегда (ядро
            // очищает жертву при её смерти, и ветка победы почти не срабатывает), а
            // начало — всегда. Без этого нельзя сравнить того, кого спутники бьют, с
            // тем, от кого гибнут: по базе у них одна настройка содержимого и один
            // множитель здоровья, а в мире — разное.
            if (c.FightsLogged < 3)
            {
                ++c.FightsLogged;
                TC_LOG_INFO("server.worldserver",
                    "Constellation БОЙ-НАЧАЛО {} (ур {}/эфф {}, жизни {}) -> {} ({}): сырой ур {}, ДЛЯ НАС ур {}, "
                    "жизней сырых {} / для нас {:.0f}, его урон x{:.2f}, его броня x{:.2f}",
                    self->GetName(), uint32(self->GetLevel()), uint32(self->GetEffectiveLevel()), self->GetMaxHealth(),
                    target->GetName(), target->GetEntry(),
                    uint32(target->GetLevel()), uint32(target->GetLevelForTarget(self)),
                    target->GetMaxHealth(),
                    float(target->GetMaxHealth()) * target->GetHealthMultiplierForTarget(self),
                    target->GetDamageMultiplierForTarget(self),
                    target->GetArmorMultiplierForTarget(self));
            }
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
        if (!Cfg().Quests || !Cfg().TakeQuests || c.State != Stage::InWorld || !c.Session)
            return;
        Player* self = c.Session->GetPlayer();
        if (!self || !self->IsInWorld() || !self->IsAlive())
            return;

        // ИДЁМ К НАЙДЕННОМУ — КАЖДЫЙ ТАКТ, А НЕ РАЗ В ПЯТЬ СЕКУНД.
        //
        // Поиск дорогой и остаётся под сроком, а вот дорога должна идти шагами по 4 Гц,
        // иначе спутник будет ползти к NPC по одному шагу в пять секунд.
        if (!c.GiverGuid.IsEmpty())
        {
            Creature* going = ObjectAccessor::GetCreature(*self, c.GiverGuid);
            bool const busy = c.Mode != Behavior::Idle;     // дерётся или идёт по своим делам
            if (!going || !going->IsAlive() || busy
                || self->GetExactDist(going) > Cfg().QuestGiverRange + 15.0f)
            {
                c.GiverGuid.Clear();                        // цель протухла — забыть
            }
            else if (!self->CanInteractWithQuestGiver(going))
            {
                float gx, gy, gz;
                ApproachPoint(c, going, self, gx, gy, gz, diff);
                StepToward(c, self, gx, gy, gz, going->GetCombatReach() + 2.0f, diff / 1000.0f);

                // ГРАНИЦЫ ДОРОГИ. Без них спутник топчется у лестницы вечно: шаг формально
                // удаётся, Stalled не взводится, автомат стоит, боёв нет. Это была живая
                // регрессия, замеченная оператором в клиенте.
                c.GiverMs += diff;
                float const now = self->GetExactDist(going);
                bool const noProgress = c.GiverMs >= 20000 && now > c.GiverDist - 1.0f;
                if (c.Stalled || noProgress || c.GiverMs >= 30000)
                {
                    TC_LOG_INFO("server.worldserver",
                        "Constellation: {} — до квестодателя {} ({}) не дойти за {} с, "
                        "было {:.1f} ярдов, стало {:.1f}; больше не пробую",
                        self->GetName(), going->GetName(), going->GetEntry(),
                        c.GiverMs / 1000, c.GiverDist, now);
                    c.GiverUnreachable.insert(c.GiverGuid);   // иначе выберем его снова
                    c.GiverGuid.Clear();
                    c.GiverMs = 0;
                    StopMoving(c, self);
                }
                return;                                     // идём; разговор — как дойдём
            }
            // дошли — падаем ниже, к разговору, минуя срок
            c.QuestMs = Cfg().QuestIntervalMs;
        }

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

        Creature* giver = NearestQuestGiver(c, self);
        if (!giver)
            return;

        // каноническая проверка ядра: расстояние, флаги, враждебность, смерть.
        // Прямой вызов обработчика мог бы обойти то, что клиенту не позволено.
        //
        // ДАЛЕКО — ЭТО ПОВОД ПОДОЙТИ, А НЕ ПОВОД СДАТЬСЯ. Здесь стоял голый return, и
        // спутник вечно стоял в пятнадцати ярдах от NPC с восклицательным знаком.
        // ПРИДЯ — ПОВЕРНУТЬСЯ. На проверку дистанции это не влияет, но это то, что делает
        // игрок, и это видно в клиенте: спутник, говорящий с NPC спиной, выглядит поломкой.
        if (self->CanInteractWithQuestGiver(giver))
            self->SetFacingToObject(giver);

        if (!self->CanInteractWithQuestGiver(giver))
        {
            if (c.Mode == Behavior::Idle)
            {
                c.GiverGuid = giver->GetGUID();     // пойдём к нему со следующего такта
                c.GiverMs = 0;
                c.GiverDist = self->GetExactDist(giver);
            }
            return;
        }
        c.GiverGuid.Clear();                        // дошли и говорим — цель больше не нужна

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
    // Оператор: «у квестодателей и принимающих есть конкретные точки, их не надо
    // искать, а идти к ним». Так и делает игрок: журнал показывает метку возврата.
    //
    // ЦЕНА. Первая версия перебирала ВСЕ спавны мира — и делала это каждый такт у
    // каждого стоящего спутника. Кодекс посчитал: 122 спутника на 4 Гц дают 488
    // полных обходов мира в СЕКУНДУ, в потоке обновления мира. На стенде с восемью
    // ботами это не проявилось никак. Теперь указатель строится ОДИН РАЗ при
    // загрузке, а поиск идёт только по нужному виду на нашей карте.
    bool FindTurnIn(Companion& c, Player* self) const
    {
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 qid = self->GetQuestSlotQuestId(slot);
            if (!qid || self->GetQuestStatus(qid) != QUEST_STATUS_COMPLETE)
                continue;
            if (c.TurnInBackoff.count(qid))      // недавно не вышло — не долбимся
                continue;
            if (c.Impossible.count(qid))         // закрыть нечем — и не станет
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(qid);
            if (!quest || !self->CanRewardQuest(quest, false))
                continue;

            // САМОСТОЯТЕЛЬНАЯ СДАЧА — ПО ФЛАГУ ЯДРА, А НЕ ПО ОТСУТСТВИЮ ПРИНИМАЮЩЕГО.
            //
            // Повод: оператор увидел двух эльфов второго уровня, стоящих на месте, — в
            // живом журнале 4007 попыток сдать квест 55660 «Time Trials», у которого нет
            // строки в creature_questender.
            //
            // Первая версия объявила самосдаваемым ЛЮБОЙ квест без принимающего-НПС. Это
            // была догадка, и Кодекс её снёс, а замер по базе подтвердил его правоту:
            // без принимающего и без флага — 19 868 квестов, а настоящих самосдаваемых —
            // 4 733. Ошибка вчетверо, и не в безопасную сторону.
            //
            // Разрешение выдаёт САМО ЯДРО, и одной строкой: HandleQuestgiverCompleteQuest
            // пропускает режим самосдачи только при QUEST_FLAGS_AUTO_COMPLETE, иначе
            // требует, чтобы объект ЧИСЛИЛСЯ принимающим, — а объект в этом режиме сам
            // игрок. Спрашиваем ровно этот флаг. У 55660 его, к слову, нет.
            if (quest->HasFlag(QUEST_FLAGS_AUTO_COMPLETE))
            {
                c.TurnInQuest = qid;
                c.TurnInEntry = 0;              // 0 = сдать самому себе, никуда не идти
                c.TurnInPos = self->GetPosition();
                return true;
            }

            bool anyEnder = false;
            for (auto const& [_, enderEntry] : sObjectMgr->GetCreatureQuestInvolvedRelationReverseBounds(qid))
            {
                anyEnder = true;            // принимающий В МИРЕ есть — приговора не будет
                auto mapIt = _spawns.find(self->GetMapId());
                if (mapIt == _spawns.end())
                    continue;
                auto entryIt = mapIt->second.find(enderEntry);
                if (entryIt == mapIt->second.end())
                    continue;
                Position const* bestSpawn = nullptr;
                float bestDist = 100000.0f;
                for (Position const& pos : entryIt->second)
                {
                    float d = self->GetExactDist2d(pos.GetPositionX(), pos.GetPositionY());
                    if (d < bestDist)
                        { bestDist = d; bestSpawn = &pos; }
                }
                if (!bestSpawn)
                    continue;
                c.TurnInQuest = qid;
                c.TurnInEntry = enderEntry;
                c.TurnInPos = *bestSpawn;
                return true;
            }

            // ПРИГОВОР — ТОЛЬКО ЗА УСТРОЙСТВО КВЕСТА, А НЕ ЗА ТО, ГДЕ МЫ СТОИМ.
            //
            // Кодекс остановил этим выкладку, и правильно: поиск точки идёт по ТЕКУЩЕЙ
            // карте спутника, а первая редакция приговаривала квест, если точки на ней
            // не нашлось. То есть принимающий на другой карте означал «никогда» — и
            // спутник, доехавший туда позже, сдать бы уже не смог. Регрессию внёс я,
            // вместе с самой пометкой, часом раньше.
            //
            // Навсегда помечаем ровно тот случай, который и наблюдался: принимающего нет
            // НИ СРЕДИ СУЩЕСТВ, НИ СРЕДИ ОБЪЕКТОВ, и флага самосдачи тоже нет. По замеру
            // базы это 19 868 квестов — их закрывают сценарии и события, которые модулю
            // изображать незачем. Всё остальное просто ждёт: FindTurnIn вернёт false,
            // спутник займётся делом и попробует снова, когда окажется где надо.
            //
            // lazy: 970 квестов принимает ИГРОВОЙ ОБЪЕКТ — они сюда НЕ попадают (проверяем
            // и эту таблицу), но и сдать их модуль пока не умеет. Задача 0020.
            if (!anyEnder && sObjectMgr->GetGOQuestInvolvedRelationReverseBounds(qid).begin()
                             == sObjectMgr->GetGOQuestInvolvedRelationReverseBounds(qid).end())
            {
                if (c.Impossible.insert(qid).second)
                    TC_LOG_INFO("server.worldserver", "Constellation: {} — квест {} '{}' закрыть нечем: ни принимающего, ни флага самосдачи",
                        self->GetName(), qid, quest->GetLogTitle());
            }
        }
        return false;
    }

    // Указатель «карта -> вид -> где стоит», построенный один раз.
    void BuildSpawnIndex()
    {
        uint32 n = 0;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            _spawns[data.mapId][data.id].push_back(data.spawnPoint);
            ++n;
        }
        TC_LOG_INFO("server.loading", "Constellation: указатель спавнов — {} точек на {} картах",
            n, uint32(_spawns.size()));
    }

    // Сдача: поздороваться, сдать, выбрать награду — теми же опкодами, что клиент.
    // Правда берётся из СОСТОЯНИЯ: сокета нет, ответа не будет, поэтому после сдачи
    // спрашиваем, числится ли квест награждённым.
    // ender == nullptr означает самостоятельную сдачу: ядро в этом режиме берёт
    // самого игрока как «объект», и здороваться не с кем.
    bool TurnInAt(Companion& c, Player* self, Creature* ender)
    {
        if (ender)
        {
            WorldPacket rawHello(CMSG_QUEST_GIVER_HELLO);
            WorldPackets::Quest::QuestGiverHello hello(std::move(rawHello));
            hello.QuestGiverGUID = ender->GetGUID();
            c.Session->HandleQuestgiverHelloOpcode(hello);
        }

        Quest const* quest = sObjectMgr->GetQuestTemplate(c.TurnInQuest);
        if (!quest || self->GetQuestStatus(c.TurnInQuest) != QUEST_STATUS_COMPLETE)
            return false;

        WorldPacket rawDone(CMSG_QUEST_GIVER_COMPLETE_QUEST);
        WorldPackets::Quest::QuestGiverCompleteQuest done(std::move(rawDone));
        done.QuestGiverGUID = ender ? ender->GetGUID() : self->GetGUID();
        done.QuestID = c.TurnInQuest;
        done.FromScript = (ender == nullptr);
        c.Session->HandleQuestgiverCompleteQuest(done);

        WorldPacket rawPick(CMSG_QUEST_GIVER_CHOOSE_REWARD);
        WorldPackets::Quest::QuestGiverChooseReward pick(std::move(rawPick));
        pick.QuestGiverGUID = ender ? ender->GetGUID() : self->GetGUID();
        pick.QuestID = c.TurnInQuest;
        // если квест предлагает выбор, берём ПЕРВУЮ настоящую награду, а не ноль:
        // с нулём ядро откажет, и спутник вернётся сюда через секунду (Кодекс)
        pick.Choice.Item.ItemID = quest->GetRewChoiceItemsCount() > 0 ? quest->RewardChoiceItemId[0] : 0;
        pick.Choice.LootItemType = LootItemType::Item;
        c.Session->HandleQuestgiverChooseRewardOpcode(pick);

        if (self->IsQuestRewarded(c.TurnInQuest))
        {
            TC_LOG_INFO("server.worldserver", "Constellation: {} сдал квест {} '{}' (уровень {})",
                self->GetName(), c.TurnInQuest, quest->GetLogTitle(), uint32(self->GetLevel()));
            ++_questsTurnedIn;
            return true;
        }
        TC_LOG_INFO("server.worldserver", "Constellation: {} — сдача квеста {} не прошла ({})",
            self->GetName(), c.TurnInQuest, ender ? "у принимающего" : "самому себе");
        return false;
    }

    // Существо рядом, которое ЧИСЛИТСЯ ЦЕЛЬЮ незакрытого квеста в журнале.
    // Что именно убивать и сколько — знает ядро из quest_objectives; спрашиваем его.
    // ЧТО НАМ ВООБЩЕ НУЖНО УБИТЬ. Вынесено отдельно, потому что этим пользуются двое:
    // поиск цели ВОКРУГ и — с 2026-08-30 — поиск МЕСТА, куда за целью идти.
    // Нужна ли нам ещё ЭТА запись существа. Тот же вопрос и тому же счётчику ядра, что
    // и при выборе цели, — просто заданный ещё раз, посреди боя.
    bool StillWanted(Player* self, uint32 entry) const
    {
        std::set<uint32> wanted, wantedItems;
        WantedEntries(self, wanted, nullptr, nullptr, nullptr, nullptr, &wantedItems);
        if (wanted.count(entry))
            return true;
        // цель могла быть выбрана КАК ИСТОЧНИК ПРЕДМЕТА — тогда её номера в wanted нет,
        // и прежний ответ был бы «не нужна» посреди боя, и бой бросился бы.
        if (!wantedItems.empty())
            if (std::vector<uint32> const* qi = sObjectMgr->GetCreatureQuestItemList(
                    entry, self->GetMap()->GetDifficultyID()))
                for (uint32 item : *qi)
                    if (wantedItems.count(item))
                        return true;
        return false;
    }

    void WantedEntries(Player* self, std::set<uint32>& wanted, uint32* slotsUsed = nullptr,
        uint32* incomplete = nullptr, uint32* monsterObjs = nullptr, uint32* unmet = nullptr,
        std::set<uint32>* wantedItems = nullptr) const
    {
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 questId = self->GetQuestSlotQuestId(slot);
            if (!questId)
                continue;
            if (slotsUsed) ++*slotsUsed;
            if (self->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
                continue;
            if (incomplete) ++*incomplete;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;
            for (QuestObjective const& obj : quest->GetObjectives())
            {
                if (obj.ObjectID <= 0)
                    continue;

                // ДВА ВИДА ЦЕЛЕЙ, А НЕ ОДИН.
                //
                // «Убить существо» даёт номер существа прямо. «Собрать предмет» даёт номер
                // ПРЕДМЕТА — а кто его роняет, спросим у каждого встречного существа
                // отдельно: ядро само сообщает клиенту этот список (Creature.cpp:230), и
                // это ровно те сведения, которые видит игрок, а не внутренняя кухня лута.
                bool const isMonster = obj.Type == QUEST_OBJECTIVE_MONSTER;
                bool const isItem = obj.Type == QUEST_OBJECTIVE_ITEM;
                if (!isMonster && !isItem)
                    continue;                       // поговорить, посетить, применить — пока не умеем
                if (isMonster && monsterObjs) ++*monsterObjs;
                if (self->GetQuestObjectiveData(obj) >= obj.Amount)
                    continue;                       // эта цель уже набрана
                if (unmet) ++*unmet;
                if (isMonster)
                    wanted.insert(uint32(obj.ObjectID));
                else if (wantedItems)
                    wantedItems->insert(uint32(obj.ObjectID));
            }
        }
    }

    // ПЕРЕВЕСТИ ДУХ, ПРЕЖДЕ ЧЕМ ЛЕЗТЬ В СЛЕДУЮЩУЮ ДРАКУ.
    //
    // Замер на боевом через пять минут после заливки: 122 спутника, 132 найденных цели,
    // 127 ударов — и ТРИДЦАТЬ смертей, шесть в минуту. Причём сосредоточенных: Эмрик 6,
    // Аделина 6, Деверел 5 — тканевые классы первого-второго уровня. Модуль умеет только
    // рукопашную автоатаку, поэтому маг, бьющий волка посохом, обречён; настоящее
    // лечение этому — дальний бой, задача 0007. Но круг «умер -> воскрес -> вернулся ->
    // умер» ломается и малым: не начинать драку, пока не отдышался.
    //
    // Оператор ставил эту фазу в конец, считая, что «негде умирать да и не от кого». Это
    // было верно, пока спутники СТОЯЛИ. Замер после того, как они пошли, говорит иначе,
    // и я иду за замером — с отметкой для утренней проверки.
    //
    // Мана — только тем, у кого она ДЕЙСТВУЮЩИЙ вид силы (Кодекс: GetPower(MANA)==0 у
    // воина ничего не значит и запарковало бы его навсегда).
    bool NeedsRest(Player* self) const
    {
        if (self->IsInCombat())
            return false;               // в бою не отдыхают
        if (self->GetHealthPct() < Cfg().RestBelowPct)
            return true;
        return self->GetPowerType() == POWER_MANA
            && self->GetPowerPct(POWER_MANA) < Cfg().RestBelowPct;
    }

    bool RestedEnough(Player* self) const
    {
        if (self->GetHealthPct() < Cfg().ResumeAbovePct)
            return false;
        return self->GetPowerType() != POWER_MANA
            || self->GetPowerPct(POWER_MANA) >= Cfg().ResumeAbovePct;
    }

    // КУДА ИДТИ ЗА ЦЕЛЬЮ — ПО ТОЧКАМ ИНТЕРЕСА КВЕСТА, КАК ИГРОК ПО МЕТКЕ.
    //
    // Оператор, 2026-08-30, отклоняя предел в четыреста ярдов: «у нас есть точные
    // места и ареалы куда и зачем ходить». Есть: quest_poi_points — те самые области,
    // которые игра рисует игроку на карте; ядро грузит их целиком и отдаёт клиенту.
    // Это закрывает и дворфов, чьих Захватчиков призывает СКРИПТ и статичных спавнов
    // у них нет вовсе, — а область квеста по устройству соразмерна его уровню, что
    // снимает и опасение Кодекса про уход малышей в чужие зоны (задача 0017).
    //
    // Контракт добыт из исходников ядра тремя читателями и сверщиком (2026-08-30;
    // сверщик, к слову, поймал двоих на выдуманных числах — цена перепроверки видна):
    //   * блоб цели отличается от маркеров сдачи/приёма признаком QuestObjectiveID != 0;
    //     сопоставление — сперва по ID цели, запасное — по StorageIndex;
    //   * координаты точек — МИРОВЫЕ на карте MapID; транспортные карты — мимо;
    //   * ВЫСОТЕ POI НЕ ВЕРИТЬ НИКОГДА: единственный её потребитель в ядре (.go quest)
    //     сам её выбрасывает и берёт землю (cs_go.cpp:293); 709 из 4473 точек несут
    //     ноль, а ноль против настоящих 383 не попадает даже в 50-ярдовое окно поиска
    //     полигона — маршрут молча выродится в прямую. Землю ищем ОТ СВОЕГО ЯРУСА вниз,
    //     а MAX_HEIGHT только запасным: см. опыт Легиона в разборе ниже по коду;
    //   * NavigationPlayerConditionID и PlayerConditionID обязательны к проверке,
    //     Flags/SpawnTrackingID/UiMapID ядро само не читает — игнорируем;
    //   * указатель GetQuestPOIData НЕ ДЕРЖАТЬ между тактами: .reload quest_poi чистит
    //     хранилище без всякой синхронизации. Каждый раз спрашиваем заново — это один
    //     шаг по хеш-таблице, уже целиком сидящей в памяти.
    bool BlobDestination(Player* self, QuestPOIBlobData const& blob, Position* out) const
    {
        float x, y;
        if (blob.Points.size() == 1)
        {
            x = float(blob.Points[0].X);
            y = float(blob.Points[0].Y);
        }
        else
        {
            // 3..12 вершин — замкнутый контур области
            std::vector<Position> verts;
            verts.reserve(blob.Points.size());
            float sx = 0.0f, sy = 0.0f;
            for (QuestPOIBlobPoint const& pt : blob.Points)
            {
                verts.emplace_back(float(pt.X), float(pt.Y), 0.0f);
                sx += float(pt.X);
                sy += float(pt.Y);
            }
            x = sx / float(blob.Points.size());
            y = sy / float(blob.Points.size());
            if (!Position(x, y, 0.0f).IsInPolygon2D(Position(0.0f, 0.0f, 0.0f), verts))
            {
                // контур вогнутый и середина снаружи — идём к ближайшей вершине
                float best = 100000.0f;
                for (Position const& v : verts)
                {
                    float d = self->GetExactDist2d(v.GetPositionX(), v.GetPositionY());
                    if (d < best)
                        { best = d; x = v.GetPositionX(); y = v.GetPositionY(); }
                }
            }
        }
        if (!MapManager::IsValidMapCoord(self->GetMapId(), x, y))
            return false;
        // ВЫСОТУ СПРАШИВАЕМ ОТ СВОЕГО ЯРУСА, А НЕ ОТ НЕБА.
        //
        // Здесь стоял GetHeight(..., MAX_HEIGHT) — самая верхняя поверхность в столбце.
        // Ровно эта ошибка стоила Легиону 121 тупика за одно окно (заметки 2026-08-13,
        // раздел «Teldrassil: зона — ЯРУСЫ»): на мировом древе верхняя поверхность это
        // ветка далеко над целью, и каждая проба возвращала 0x8 — полигон не найден.
        //
        // То же самое здесь и сегодня: спутник стоит на земле у Шпиля на z≈25, а точка
        // задания приходит с z=110 — это КРЫША здания, а не пол, по которому он может
        // идти. Замер: все двадцать отказов построителя с большой разницей высот, ни
        // одного с разницей меньше пяти ярдов.
        //
        // Починка та же, что сработала на Легионе: искать ВНИЗ от собственного яруса
        // спутника. На ровной местности это тот же ответ, что и MAX_HEIGHT, поэтому
        // MAX_HEIGHT остаётся запасным — если своя высота не дала ничего, значит цель
        // выше нас, и тогда пусть будет хоть какая-то.
        float z = self->GetMap()->GetHeight(self->GetPhaseShift(), x, y,
                                            self->GetPositionZ() + 5.0f);
        if (z <= INVALID_HEIGHT)
            z = self->GetMap()->GetHeight(self->GetPhaseShift(), x, y, MAX_HEIGHT);
        if (z <= INVALID_HEIGHT)
            return false;
        out->Relocate(x, y, z);
        return true;
    }

    // Запасной путь для квестов без точек: ближайший спавн из указателя. Прежний
    // предел дальности остаётся ТОЛЬКО здесь — этот путь, в отличие от точек квеста,
    // ничем не привязан к его области.
    bool SpawnDestination(Player* self, uint32 entry, Position* out) const
    {
        auto mapIt = _spawns.find(self->GetMapId());
        if (mapIt == _spawns.end())
            return false;
        auto entryIt = mapIt->second.find(entry);
        if (entryIt == mapIt->second.end())
            return false;
        Position const* best = nullptr;
        float bestDist = 100000.0f;
        for (Position const& pos : entryIt->second)
        {
            float d = self->GetExactDist2d(pos.GetPositionX(), pos.GetPositionY());
            if (d < bestDist)
                { bestDist = d; best = &pos; }
        }
        if (!best || bestDist > 400.0f)
            return false;
        *out = *best;
        return true;
    }

    bool FindObjectiveSpot(Companion& c, Player* self) const
    {
        Position best;
        float bestDist = 100000.0f;
        bool found = false;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 questId = self->GetQuestSlotQuestId(slot);
            if (!questId || self->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
                continue;
            if (c.TravelBackoff.count(questId))
                continue;                   // недавно сходили впустую — не повторяем
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;
            QuestPOIData const* poi = sObjectMgr->GetQuestPOIData(int32(questId));
            for (QuestObjective const& obj : quest->GetObjectives())
            {
                if (obj.Type != QUEST_OBJECTIVE_MONSTER || obj.ObjectID <= 0)
                    continue;
                if (self->GetQuestObjectiveData(obj) >= obj.Amount)
                    continue;

                Position dest;
                bool got = false;
                if (poi)
                {
                    // Кодекс, проход 6: (1) негодный лучший блоб раньше уводил на
                    // запасной путь, не дав шанса ОСТАЛЬНЫМ кандидатам — теперь точка
                    // проверяется у каждого, и выбирается лучший из ГОДНЫХ; (2) ряд
                    // сравнения полный, по контракту: приоритет, ближе к нам, меньший
                    // BlobIndex.
                    int32 bestPrio = 0, bestBlob = 0;
                    float bestD = 0.0f;
                    for (int pass = 0; pass < 2 && !got; ++pass)
                        for (QuestPOIBlobData const& b : poi->Blobs)
                        {
                            if (b.Points.empty() || !b.QuestObjectiveID)
                                continue;               // пустые и маркеры сдачи — мимо
                            if (b.MapID != int32(self->GetMapId()) || sObjectMgr->IsTransportMap(uint32(b.MapID)))
                                continue;
                            if (b.NavigationPlayerConditionID
                                && !ConditionMgr::IsPlayerMeetingCondition(self, uint32(b.NavigationPlayerConditionID)))
                                continue;
                            if (b.PlayerConditionID
                                && !ConditionMgr::IsPlayerMeetingCondition(self, uint32(b.PlayerConditionID)))
                                continue;
                            bool match = (pass == 0)
                                ? (b.QuestObjectiveID == int32(obj.ID))
                                : (b.ObjectiveIndex == int32(obj.StorageIndex));
                            if (!match)
                                continue;
                            Position cand;
                            if (!BlobDestination(self, b, &cand))
                                continue;               // негодная точка — следующий кандидат
                            float dd = self->GetExactDist2d(cand.GetPositionX(), cand.GetPositionY());
                            bool better = !got
                                || b.Priority > bestPrio
                                || (b.Priority == bestPrio && dd < bestD - 0.5f)
                                || (b.Priority == bestPrio && std::fabs(dd - bestD) <= 0.5f
                                    && b.BlobIndex < bestBlob);
                            if (better)
                            {
                                got = true;
                                dest = cand;
                                bestPrio = b.Priority;
                                bestD = dd;
                                bestBlob = b.BlobIndex;
                            }
                        }
                }
                if (!got)
                    got = SpawnDestination(self, uint32(obj.ObjectID), &dest);
                if (!got)
                    continue;
                float d = self->GetExactDist2d(dest.GetPositionX(), dest.GetPositionY());
                if (d < bestDist)
                    { bestDist = d; best = dest; found = true; c.TravelQuest = questId; }
            }
        }
        // ближе FightRange идти незачем: там цель и так увидит обычный поиск
        if (!found || bestDist < Cfg().FightRange)
            return false;
        c.TravelPos = best;
        return true;
    }

    Creature* FindObjectiveTarget(Companion& c, Player* self) const
    {
        // СО СЛОМАННЫМ СНАРЯЖЕНИЕМ ЦЕЛЬ НЕ ИЩЕМ ВОВСЕ.
        //
        // Это и есть тот круг, который держал боевой сервер: тканевый не может выиграть
        // автоударом, гибнет, каждая смерть снимает десятую часть прочности со ВСЕГО
        // надетого, через десять смертей сломано всё — и выиграть он не может уже никогда,
        // потому что бьёт голыми руками и без брони. На 2026-08-31 в этом круге сидели
        // сорок спутников трёх классов, по семь сломанных вещей из семи, при нуле сломанных
        // у всех прочих классов.
        //
        // Пока похода к починке нет (задача 0010), спутник просто не ищет боя: сдавать
        // готовые квесты, отдыхать и ходить за хозяином он по-прежнему может.
        if (BrokenForFight(c, self))
            return nullptr;

        // какие виды существ нам вообще нужны
        std::set<uint32> wanted, wantedItems;
        uint32 slotsUsed = 0, incomplete = 0, monsterObjs = 0, unmet = 0;
        WantedEntries(self, wanted, &slotsUsed, &incomplete, &monsterObjs, &unmet, &wantedItems);
        // ДИАГНОСТИКА ПО РАЗУ НА КАЖДОГО, А НЕ ПО РАЗУ НА ВЕСЬ МОДУЛЬ.
        //
        // Флаг был один на всех, и за целый прогон печаталась РОВНО ОДНА строка — про
        // Гаррика. По ней нельзя сказать ничего о составе: у него нашлось ноль целей-убить,
        // а у скольких ещё — неизвестно. Пять гипотез подряд разбились об это, поэтому
        // флаг переезжает в спутника: 122 строки один раз, и картина видна целиком.
        if (!c.FightDiagDone && slotsUsed)
        {
            c.FightDiagDone = true;
            TC_LOG_INFO("server.worldserver",
                "Constellation DIAG {}: слотов занято {}, незакрытых {}, целей-убить {}, ненабранных {}, видов {}",
                self->GetName(), slotsUsed, incomplete, monsterObjs, unmet, uint32(wanted.size()));
        }
        if (wanted.empty() && wantedItems.empty())
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
            if (!creature->IsAlive())
                continue;

            // ГОДИТСЯ ЛИБО КАК ЦЕЛЬ УБИЙСТВА, ЛИБО КАК ИСТОЧНИК НУЖНОГО ПРЕДМЕТА.
            //
            // Второе спрашивается у ядра тем же списком, что оно шлёт клиенту, когда тот
            // запрашивает сведения о существе. Обратный индекс не нужен: перебор существ
            // вокруг уже идёт, и вопрос задаётся ровно тем, кто попался на глаза.
            bool suitable = wanted.count(creature->GetEntry()) != 0;
            if (!suitable && !wantedItems.empty())
                if (std::vector<uint32> const* qi = sObjectMgr->GetCreatureQuestItemList(
                        creature->GetEntry(), self->GetMap()->GetDifficultyID()))
                    for (uint32 item : *qi)
                        if (wantedItems.count(item))
                            { suitable = true; break; }
            if (!suitable)
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
    Creature* NearestQuestGiver(Companion const& c, Player* self) const
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
            if (c.GiverUnreachable.count(creature->GetGUID()))
                continue;               // уже пробовали дойти и не вышло
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
    // ПОВЕРНУТЬСЯ К ЦЕЛИ — БЕЗ ЭТОГО ЯДРО НЕ ДАЁТ УДАРИТЬ.
    //
    // Замер на боевом за десять минут: 896 начатых боёв, ДВЕНАДЦАТЬ побед, 455 смертей,
    // 421 «две минуты без исхода». Противники при этом — первого-второго уровня с 27-51
    // единицей здоровья ДЛЯ НАС, а у спутника 148. Проигрывать такому в 98.6 % случаев
    // невозможно, если бьёшь. Значит не бьёт.
    //
    // Ответ нашёлся там, где и положено, — в ядре. Unit::UpdateMeleeAttackingState
    // требует ДВУХ условий, а не одного:
    //     if (!IsWithinMeleeRange(victim))                  -> NotInRange
    //     if (!IsWithinBoundaryRadius(victim) && !HasInArc(2*PI/3, victim)) -> BadFacing
    // Дальность я держал, а ПОВОРОТ не задавал никогда. Пока спутник идёт, он смотрит по
    // ходу движения; дойдя, шлёт пакет остановки со СТАРОЙ ориентацией. Если она мимо
    // цели — каждый замах отвергается, таймер переставляется на 100 мс, и так до самой
    // смерти. Ноль урона.
    //
    // Отсюда же и разница со стендом, которая сбивала меня всю ночь: там цели далеко,
    // спутник ИДЁТ к ним и в конце пути смотрит на цель — 94 победы из 102. На боевом
    // мобы подходят сами, спутник бьёт не сойдя с места и глядя в сторону.
    //
    // Поворот шлём тем же опкодом, что и клиент: CMSG_MOVE_SET_FACING через тот же
    // HandleMovementOpcode. Нулевой инвариант цел.
    void FaceTarget(Companion& c, Player* self, Unit* target)
    {
        if (!target)
            return;

        // УГОЛ СЧИТАЕМ ЗНАКОВЫЙ, А НЕ ЧЕРЕЗ НОРМАЛИЗАЦИЮ В [0, 2PI).
        // Кодекс, проход 9: NormalizeOrientation превращает -0.01 в 6.27, и модуль
        // считал бы «повёрнут неверно» при отклонении в полградуса — с одной стороны
        // цели пакеты шли бы без конца, с другой не шли бы вовсе.
        float ang = self->GetAbsoluteAngle(target);
        float diff = ang - self->GetOrientation();
        while (diff > float(M_PI))  diff -= 2.0f * float(M_PI);
        while (diff < -float(M_PI)) diff += 2.0f * float(M_PI);
        if (std::fabs(diff) < 0.05f)
            return;                         // уже смотрим куда надо — не сорим пакетами

        // СОСТОЯНИЕ ДВИЖЕНИЯ СОХРАНЯЕМ ЦЕЛИКОМ.
        // Обработчик не правит ориентацию, а ЗАМЕЩАЕТ весь m_movementInfo присланным
        // (MovementHandler.cpp:356). Мой «c.Moving ? FORWARD : 0» стирал бы флаги,
        // дополнительные флаги, транспорт, падение, прыжок и тангаж. Берём нынешнее
        // состояние игрока и меняем в нём только положение, поворот и время.
        MovementInfo mi = self->m_movementInfo;
        mi.guid = self->GetGUID();
        Position pos = self->GetPosition();
        pos.SetOrientation(ang);
        mi.pos.Relocate(pos);
        mi.time = GameTime::GetGameTimeMS();
        c.Session->HandleMovementOpcode(CMSG_MOVE_SET_FACING, mi);
    }

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
            c.OwnerFromGroup = false;   // назначено командой — группой не отменяется
            ++n;
        }
        if (off)
            handler->PSendSysMessage("Constellation: %u companions released.", n);
        else
            handler->PSendSysMessage("Constellation: %u companions now follow %s.", n,
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
        handler->PSendSysMessage("Constellation %s: взято %u, боёв %u, СДАНО %u, переходов %u",
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
        handler->PSendSysMessage("  idle %u, following %u, approaching %u, attacking %u",
            idle, following, approaching, attacking);
        handler->PSendSysMessage("Constellation %s: %s — roster %u, in world %u, failed %u",
            CONSTELLATION_VERSION, Cfg().Enable ? "enabled" : "disabled",
            uint32(_companions.size()), inWorld, failed);
        for (Companion const& c : _companions)
            if (c.State == Stage::InWorld && c.Session && c.Session->GetPlayer())
                handler->PSendSysMessage("  %s — %s in world", c.Entry->Name,
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
        BuildSpawnIndex();
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
                // СЧЁТЧИК ЗАВОДИМ ДО ВХОДА, А НЕ ПОСЛЕ.
                //
                // Раньше запись создавалась в первом бою, потом — в BehaveTick, и оба
                // раза поздно: BehaveTick работает только при Stage::InWorld, а это
                // состояние выставляет AdvanceOne, который в обходе идёт ПОСЛЕ него.
                // То есть спутник жил в мире минимум такт без записи, и любой удар с
                // потока карты в это окно обработчики молча выбрасывали — терялось
                // ровно то, ради чего прибор ставится (Кодекс, проход 13).
                //
                // Здесь GUID уже известен, а объекта игрока на карте ещё нет: раньше
                // этой точки урона по нему быть не может.
                Manager::Instance()->RegisterAndSnapshot(c.Guid);

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
                    RepairIfBroken(c.Session->GetPlayer(), "вход в мир");
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
                // ХОЗЯИН ДЕРЖИТСЯ НА ГРУППЕ, А НЕ НА ПАМЯТИ.
                //
                // Оператор, 2026-08-30: «взял в группу, она пошла за мной. я ее выгнал
                // из группы — она продолжает следовать за мной». Так и было: проверка
                // ниже срабатывала, только пока спутник СОСТОИТ в группе, а выгнанный
                // не состоит нигде — и хозяин, записанный при приглашении, оставался
                // навсегда. Группа и есть поводок: нет общей группы — нет хозяина.
                // Одной проверкой покрыты сразу выгнали, распустили и хозяин вышел сам.
                if (c.OwnerFromGroup && !c.Owner.IsEmpty())
                {
                    Group* mine = player->GetGroup();
                    Player* owner = ObjectAccessor::FindConnectedPlayer(c.Owner);
                    if (!mine || !owner || owner->GetGroup() != mine)
                    {
                        TC_LOG_INFO("server.worldserver", "Constellation: {} больше не в группе с хозяином — перестаю следовать",
                            player->GetName());
                        c.Owner.Clear();
                        c.OwnerFromGroup = false;
                    }
                }

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
                        c.OwnerFromGroup = false;
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
                    c.OwnerFromGroup = true;    // поводок — группа; порвётся вместе с ней
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
    std::mutex _blowsLock;                                  // см. Blows: шесть потоков карт
    std::unordered_map<ObjectGuid, Manager::Blows> _blows;  // счёт ударов, по GUID спутника
    bool _debugPairDone = false;
    uint32 _questsTaken = 0;
    uint32 _fightsStarted = 0;
    uint32 _transitions = 0;
    uint32 _questsTurnedIn = 0;
    mutable bool _fightDiagDone = false;
    uint32 _hops = 0;                   // сколько раз дошли до цели промежуточными прыжками
    mutable bool _rejDiagDone = false;
    mutable bool _whyDone = false;
    uint32 _revived = 0;                // сколько раз спутники возвращались из мёртвых
    uint32 _noPath = 0;                 // сколько раз сетка не дала маршрута
    uint32 _noPathLogged = 0;           // из них записано в журнал (потолок 20)
    std::unordered_map<uint32, std::unordered_map<uint32, std::vector<Position>>> _spawns;
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

// ГДЕ УРОН И ЗАМАХИ СЧИТАЮТСЯ, И ЧТО ИМЕННО ЭТИ ЧИСЛА ЗНАЧАТ.
//
// ModifyMeleeDamage зовётся из Unit::CalculateMeleeDamage (Unit.cpp:1377) ДО броска
// исхода (Unit.cpp:1388), поэтому считает и промах, и уклонение, и EVADE. Единственный
// вызов CalculateMeleeDamage во всём ядре — внутри Unit::AttackerStateUpdate
// (Unit.cpp:2300), а тот зовётся только для правой руки, левой руки и дополнительного
// удара. Но «каждый замах» сказать всё же нельзя, и это нашёл Кодекс в проходе 12,
// прямо в доказательстве, которое я ему принёс: внутри AttackerStateUpdate вызов стоит
// под условием отсутствия meleeAttackSpellId — автоудар, подменённый заклинанием, этот
// путь минует. У наших спутников такой подмены нет, но число называть безусловным
// нельзя.
//
// OnDamage зовётся из Unit::DealDamage на ЛЮБОЙ наш урон, а не только на дошедший
// автоудар. Поэтому разницу «замахи минус события урона» НЕЛЬЗЯ читать как промахи —
// сегодня у спутников просто нет другого источника урона, и это единственная причина,
// по которой числа сопоставимы.
class constellation_unitscript : public UnitScript
{
public:
    constellation_unitscript() : UnitScript("constellation_unitscript") { }

    void ModifyMeleeDamage(Unit* /*target*/, Unit* attacker, uint32& /*damage*/) override
    {
        Constellation::Manager::Instance()->NoteSwing(attacker);
    }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        Constellation::Manager::Instance()->NoteDamage(attacker, victim, damage);
    }
};

// КТО ДОБИЛ — ОТВЕЧАЕТ ЯДРО. Unit::Kill зовёт этот крючок ровно для того, чей удар
// оказался последним (Unit.cpp:11558), поэтому чужое убийство нам не запишется.
class constellation_playerscript : public PlayerScript
{
public:
    constellation_playerscript() : PlayerScript("constellation_playerscript") { }

    void OnCreatureKill(Player* killer, Creature* killed) override
    {
        Constellation::Manager::Instance()->NoteKill(killer, killed);
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
            { "repair",  HandleRepair,  rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "wipe",    HandleWipe,    rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "vend",    HandleVend,    rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
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

    static bool HandleRepair(ChatHandler* handler)
    {
        return Constellation::Manager::Instance()->RepairAll(handler);
    }

    static bool HandleVend(ChatHandler* handler)
    {
        return Constellation::Manager::Instance()->VendAll(handler);
    }

    // Без слова подтверждения команда только показывает, что сделает. Это не украшение:
    // отменить вайп можно лишь из резервной копии.
    static bool HandleWipe(ChatHandler* handler, Optional<std::string> confirm)
    {
        return Constellation::Manager::Instance()->WipeAll(handler, confirm.value_or(""));
    }
};

void AddConstellationScripts()
{
    new constellation_worldscript();
    new constellation_unitscript();
    new constellation_playerscript();
    new constellation_commandscript();
}
