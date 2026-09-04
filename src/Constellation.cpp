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
#include "SmartScriptMgr.h"
#include "DisableMgr.h"
#include "TaxiPathGraph.h"
#include "TaxiPackets.h"
#include "PhasingHandler.h"
#include "Plan.h"
#include "WaypointDefines.h"
#include "WaypointManager.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "MotionMaster.h"
#include "DB2Stores.h"
#include "GameObjectPackets.h"
#include "PathGenerator.h"
#include "ObjectAccessor.h"
#include "ConditionMgr.h"
#include "Corpse.h"
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
#include "AreaTriggerPackets.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSession.h"

#include "StringConvert.h"

#include <algorithm>
#include <cmath>
#include <deque>
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
    bool  SkipElites      = true;       // одиночка не дерётся с элитными и редкими
    uint32 MaxAssist      = 2;          // сколько заступников у цели ещё терпимо (0 = не проверять)
    uint32 StarveMs       = 300000;     // и через сколько без боя порог отступает
    float KiteYards       = 30.0f;      // на сколько уводить цель от лагеря (0 = не уводить)
    bool  Flying          = true;       // пользоваться ли полётными путями
    float FlyIfFartherThan = 400.0f;    // ближе этого лететь незачем
    float FlyIfSaves      = 250.0f;     // и экономия должна быть заметной
    uint32 FlyRouteCandidates = 6;      // сколько узлов спросить у графа за раз
    uint32 WalkCapMs      = 900000;     // страховка от вечной дороги: пятнадцать минут ≈ шесть тысяч ярдов
    float GiverSeekRange  = 600.0f;     // квестодатель по карте: дальше — не поход, а переезд (0 = выключено; Кодекс: 600, как у торговца)
    int32 QuestMaxAbove   = 2;          // жёлтое берём; выше на столько уровней — уже красное
    bool  SellByWeaponSkill = false;    // продавать оружие без навыка класса — после чтения строк навыков
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
        SkipElites      = sConfigMgr->GetBoolDefault("Constellation.SkipElites", true);
        MaxAssist       = std::clamp<uint32>(sConfigMgr->GetIntDefault("Constellation.MaxAssist", 2), 0, 20);
        StarveMs        = std::clamp<uint32>(sConfigMgr->GetIntDefault("Constellation.StarveMs", 300000), 60000, 3600000);
        KiteYards       = std::clamp(sConfigMgr->GetFloatDefault("Constellation.KiteYards", 30.0f), 0.0f, 60.0f);
        Flying          = sConfigMgr->GetBoolDefault("Constellation.Flying", true);
        // НАСТРОЙКИ ПРОВЕРЯЕМ, А НЕ ПРИНИМАЕМ ЛЮБЫЕ (Кодекс): отрицательный порог означал бы
        // «лететь всегда», а нулевая экономия — «лететь ради самого полёта».
        FlyIfFartherThan = std::max(100.0f, sConfigMgr->GetFloatDefault("Constellation.FlyIfFartherThan", 400.0f));
        FlyIfSaves      = std::max(50.0f, sConfigMgr->GetFloatDefault("Constellation.FlyIfSaves", 250.0f));
        FlyRouteCandidates = std::clamp<uint32>(sConfigMgr->GetIntDefault("Constellation.FlyRouteCandidates", 6), 1, 20);
        WalkCapMs       = std::clamp<uint32>(sConfigMgr->GetIntDefault("Constellation.WalkCapMs", 900000), 60000, 3600000);
        GiverSeekRange  = sConfigMgr->GetFloatDefault("Constellation.GiverSeekRange", 600.0f);
        QuestMaxAbove   = sConfigMgr->GetIntDefault("Constellation.QuestMaxAbove", 2);
        SellByWeaponSkill = sConfigMgr->GetBoolDefault("Constellation.SellByWeaponSkill", false);
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

enum class Behavior : uint8 { Idle, Recovering, FollowingOwner, Travelling, ApproachingTarget, Attacking, TurningIn, Vending, Gathering, Talking, SeekingGiver, TakingFlight };

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
    float GiverRange = 0.0f;            // радиус, с которого цель передана по приходу (0 = обычный)
    std::map<ObjectGuid, uint32> GiverUnreachable;  // до кого не дойти: лестницы, помосты, геометрия
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
    uint32 TurnInEntry = 0;
    bool TurnInPosFromTable = false;    // точка сдачи из указателя (высоту править можно) или от живого             // кому
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
    ObjectGuid BoundAt;                 // у какого трактирщика привязан камень
    uint32 InnScanMs = 0;               // когда снова смотреть, нет ли рядом трактирщика
    uint32 HearthCooldownMs = 0;        // и когда снова пробовать камень
    uint32 HearthCastMs = 0;            // пока идёт произнесение — стоим смирно, иначе оборвётся
    uint32 TaxiScanMs = 0;              // когда снова смотреть, нет ли рядом полётного мастера
    ObjectGuid FlightMaster;            // к какому мастеру идём
    Position FlightMasterPos;           // и где он стоит по таблице
    uint32 FlightMasterEntry = 0;       // и какого он вида — на месте берём того же
    uint32 FlightFromNode = 0;          // узел, для которого маршрут проверен
    uint32 FlightNode = 0;              // куда летим
    uint32 FlightCooldownMs = 0;        // между попытками улететь
    float FlightSavedYards = 0.0f;      // сколько ярдов пешком экономим — для журнала
    std::set<ObjectGuid> TaxiDone;      // у кого точка получена или уже была — навсегда
    std::map<ObjectGuid, uint32> TaxiRetry;   // а неудача — со сроком: условия бывают временными
    uint32 EnderScanMs = 0;             // когда искать заново
    uint32 LiveEnderMs = 0;             // и когда искать ЖИВОГО принимающего без точки появления
    uint32 IdleScanMs = 0;              // «стою» не перебирает мир на каждом такте
    bool FightDiagDone = false;         // диагностика боевого поиска — по разу на КАЖДОГО
    bool GiverDiagDone = false;         // и то же для поиска квестодателя
    bool GatherDiagDone = false;        // и для отбора точки сбора
    bool TalkDiagDone = false;          // и для самого разговора
    bool RedNoted = false;              // сказали ли хоть раз, что пропускаем красные
    bool ImmuneNoted = false;           // и что цель ещё невосприимчива к игрокам
    Position KiteTo;                    // куда пятимся, уводя цель от лагеря
    bool Kiting = false;                // и пятимся ли сейчас
    uint32 KiteMs = 0;                  // сколько уже пятимся — чтобы не вечно
    uint32 EngageAssists = 0;           // сколько врагов было у цели, когда мы её брали
    Position PackCenter;                // и где был их центр — от него и отходим
    bool PackCenterKnown = false;
    std::vector<Position> KitePath;     // путь отхода, проверенный построителем
    size_t KiteIdx = 0;
    uint32 NoTargetMs = 0;              // сколько уже нет боя: долго — порог стаи отступает
    bool EliteNoted = false;            // и что элитных в одиночку не берём
    bool ToughNoted = false;            // и что слишком крепких тоже
    bool CondNoted = false;             // и что цель не отвечает условиям заклинания
    // ОТМЕТКИ ГИБЕЛЕЙ, А НЕ СЧЁТЧИК С ТАЙМЕРОМ (Кодекс): таймер, который каждая смерть
    // ставит заново, считает тремя за десять минут даже смерти на 0-й, 9-й и 18-й.
    std::deque<uint32> DeathAt;         // время гибелей, мс игрового времени
    uint32 FleeMs = 0;                  // сколько уже отходим от того, с кем не справиться
    bool FleeNoted = false;             // и сказали ли об этом
    bool CorpseRunNoted = false;        // сказали ли, что бежим к своему телу
    uint32 CorpseRunMs = 0;             // сколько уже бежим
    uint32 ReclaimWaitMs = 0;           // сказали ли, что ждём срок подъёма
    bool DeathCounted = false;          // эта гибель уже записана в окно
    uint8 CorpseTries = 0;              // отказов подъёма у тела: две — и к целительнице
    bool GraveWalkNoted = false;        // строка «иду на кладбище» — раз на смерть
    float GraveWalkLast = 0.0f;         // расстояние до кладбища на прошлом такте: прогресс
    bool HealerStepNoted = false;       // «застрял в N ярдах от целительницы» — раз на смерть
    uint8 HealerRings = 0;              // поисков кольца у целительницы в этой смерти: не больше двух
    Position HealerRefused;             // отвергнутая точка кольца — второму поиску её не предлагать
    bool HealerRefusedSet = false;
    bool CorpseGaveUp = false;          // до тела не добежать или подъём не вышел
    bool RevivePicked = false;          // тихое место у тела выбрано
    Position RevivePos;                 // и вот оно
    Position FleeTo;                    // ВЫБРАННАЯ точка отхода — она не двигается за нами
    bool FleeHasPoint = false;
    uint32 FleePauseMs = 0;             // отход не удался — не долбиться
    uint32 FleeTotalMs = 0;             // общий бюджет отхода: смена точки его не обнуляет
    // ОСОБЬ, КОТОРАЯ СЕЙЧАС НЕ ПОДХОДИТ, НО ПОДОЙДЁТ ПОТОМ (Кодекс): условие заклинания
    // бывает временным — ленивый батрак снова засыпает через пять минут. Вечный запрет
    // (TalkUnreachable) вычеркнул бы его навсегда, поэтому здесь запрет со сроком.
    std::map<ObjectGuid, uint32> TalkRetry;
    // КАНДИДАТ, А НЕ ЖИВОЙ УКАЗАТЕЛЬ. Живой объект берётся по приходу, по идентификатору
    // спавна; до этого мы знаем только, ГДЕ он записан на карте.
    uint32 GatherSpawnId = 0;           // идентификатор точки появления объекта
    uint32 GatherEntry = 0;             // и вид объекта — для отчёта
    Position GatherPos;                 // куда идти
    uint32 GatherMs = 0;                // сколько уже идём
    float GatherDist = 0.0f;            // и с какого расстояния начали
    uint32 Gathered = 0;                // сколько объектов обобрано за жизнь
    // ОТСРОЧКА ПО ТОЧКЕ ПОЯВЛЕНИЯ, А НЕ ПО GUID (Кодекс): у одной точки бывает несколько
    // экземпляров — личных, фазовых, — и запрет по GUID промахивается мимо следующего.
    // Причины разные и сроки разные: не дойти, ещё не возродился, не та фаза, брать нечего.
    std::unordered_map<uint32, uint32> GatherBackoff;
    // ЦЕЛЬ, С КОТОРОЙ НАДО ГОВОРИТЬ, А НЕ ДРАТЬСЯ.
    //
    // Замер 2026-09-01: шесть спутников нежити стоят кучей на кладбище с квестом
    // «The Wakening», у которого все три цели записаны типом «существо», но с описанием
    // «Speak with ...». Модуль читал тип и шёл их УБИВАТЬ — а они дружественные, и убить
    // их нельзя. То же держит тауренов с десятью такими целями.
    ObjectGuid TalkCandidate;           // нашлась при поиске боевой цели
    ObjectGuid TalkGuid;                // к кому идём говорить
    uint32 TalkMs = 0;                  // сколько уже идём
    float TalkDist = 0.0f;              // и с какого расстояния начали
    uint32 Talked = 0;                  // сколько разговоров и применений дало зачёт
    uint8 ToolFruitless = 0;            // применений подряд без зачёта (предохранитель)
    uint32 EquipScanMs = 0;             // когда снова смотреть сумки на предмет обновок
    uint32 Equipped = 0;                // сколько вещей надето за жизнь
    std::map<std::pair<ObjectGuid, uint8>, uint32> EquipRefused;
    ObjectGuid WeaponWas;               // что было в руках: сменилось — отказы пересматриваем   // предмет+слот -> сколько ещё не пробовать, мс
    std::map<ObjectGuid, uint32> SellRefused;   // предмет, который ядро отказалось купить -> мс до новой попытки
    std::map<uint32, uint32> VendorNoSell;      // вид торговца, отказавшего во всём -> мс, пока он не продавец
    // ОЖИДАНИЕ ЗАЧЁТА ПОСЛЕ ПОПЫТКИ (Кодекс о применении предметов): заклинание бывает с
    // временем произнесения и ставится ядром в очередь (Player::RequestSpellCast), поэтому
    // результат меряется не в том же вызове, а по окну. Предохранитель считает ОКНА без
    // зачёта, а не отправленные пакеты — отказ по откату или занятости попыткой не является.
    uint32 ToolWaitMs = 0;              // сколько ещё ждать зачёта (0 = попытка не сделана)
    uint32 ToolFruitlessEntry = 0;      // для какого вида считаем отставленных особей
    uint8 ToolGiveUps = 0;              // сколько особей этого вида отставлено подряд
    uint8 ToolActionFruitless = 0;      // отставленных особей подряд по ВСЕМ видам (Легион, 0012)
    uint32 ToolActionMs = 0;            // и пауза всему действию, когда их накопилось шесть
    float WalkBest = 1.0e9f;            // лучшее расстояние до точки в этом режиме
    uint32 WalkStuckMs = 0;             // сколько подряд не приближаемся к ней
    std::vector<std::pair<std::pair<uint32, uint32>, int32>> ToolWas;   // счётчики целей ДО попытки
    uint32 ToolWasEntry = 0;            // по какому виду снят снимок: зачёт, пришедший после окна,
                                        // сверяется с ним перед СЛЕДУЮЩЕЙ попыткой (Кодекс)
    // ТОРГОВЕЦ ПО КАРТЕ: когда в обзоре никого, идём к ближайшему из указателя спавнов —
    // так же, как к принимающему квест. VendorGuid заполняется, когда он показался.
    uint32 VendorEntry = 0;             // к какому виду торговца идём (0 = ни к какому)
    Position VendorPos;                 // и где он стоит по таблице
    float VendorDist = 0.0f;            // с какого расстояния пошли: срок считается от него
    uint32 VendorScanMs = 0;            // обзор сетки у его точки — не чаще раза в 2 с
    // КВЕСТОДАТЕЛЬ ПО КАРТЕ: когда в обзоре никто ничего не предлагает, идём к ближайшему
    // из указателя, у кого ядро дало бы квест по светофору. SeekEntry = 0 — ни к кому.
    uint32 SeekEntry = 0;               // к какому виду квестодателя идём
    ObjectGuid::LowType SeekSpawn = 0;  // и к какой именно его точке
    Position SeekPos;                   // и где она по таблице
    uint32 SeekCooldownMs = 0;          // между походами — пять минут
    std::unordered_map<ObjectGuid::LowType, uint32> SeekBackoff;   // точка -> сколько не ходить к ней снова
    uint32 IdleDiagMs = 0;              // прибор «ПРОСТОЙ» — раз в пять минут
    uint32 LastPathType = 0;            // тип последнего отказа построителя — для строки «не подойти»
    bool RingTried = false;             // обход точек вокруг NPC в этом намерении уже был
    bool FarDiagDone = false;           // «за потолком» — по разу на спутника
    bool RingHeld = false;              // и найденная точка держится до прихода или отказа
    float TurnInDist = 0.0f;            // с какого расстояния пошли сдавать: срок от него
    std::unordered_map<uint32, uint32> TriggerSentMs;   // зона осмотра -> не слать повторно, мс
    float TravelStop = 10.0f;           // на каком расстоянии от точки считать «пришёл»
    std::unordered_map<uint32, uint32> TalkBackoff;   // ВИД -> когда пробовать снова: это
                                        // про «у него нет пункта, дающего зачёт», и такое
                                        // свойство общее у всех особей вида
    std::set<ObjectGuid> TalkUnreachable;   // а «не дойти» — свойство КОНКРЕТНОЙ особи:
                                        // запрет по виду глушил бы и всех остальных, и все
                                        // задания, где этот вид встречается (Кодекс)
    uint32 FrozenMs = 0;                // сколько стоим под запретом движения
    bool FrozenNoted = false;           // и сказали ли об этом хоть раз
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

        // ПЛАН СТРОИТСЯ ЗДЕСЬ: мировой поток, рельеф уже загружен, игрока не нужно.
        if (!_planTried)
        {
            _planTried = true;
            Constellation::Plan::Planner::Instance()->Build();
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
            Creature* vendor = FindVendorNear(c, self, false, false);
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

    // ССЫЛКА НА ТОЧКУ НАЗНАЧЕНИЯ (destFix) — И РАЗРЕШЕНИЕ, И МЕСТО ДЛЯ ПОПРАВКИ.
    //
    // Её передают только те, кто идёт к точке ИЗ ТАБЛИЦЫ: место задания, точка квестодателя,
    // торговец, принимающий, объект сбора, полётный мастер. Тогда шагу разрешено один раз
    // спросить высоту с другого яруса и, если оттуда путь строится, ЗАПИСАТЬ поправку обратно —
    // иначе следующий заход повторил бы тот же поиск, а «дошёл» мог не признаться никогда
    // (Кодекс). К живому существу ничего не правим: его высота — его собственная и верная.
    bool StepToward(Companion& c, Player* self, float tx, float ty, float tz, float stopAt, float dt,
                    Position* destFix = nullptr)
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

        // ЭТОТ ОТКАЗ БЫЛ МОЛЧАЛИВЫМ, И ЭТО ЕГО ГЛАВНАЯ БЕДА.
        //
        // Замер 2026-09-01: у сбора 116 отказов «не дойти» против 19 удач, и в каждом
        // расстояние не менялось ВООБЩЕ — 63 и через двадцать секунд 63, — при нуле
        // отказов построителя маршрута. То есть спутник не шёл и не жаловался. Причина
        // здесь: помеченный как падающий, плавающий или обездвиженный не двигается, а
        // журнал об этом не знает. Считаем и называем причину — по разу на спутника.
        //
        // И лечим то, что лечится: «падаю» у спутника без клиента залипает — клиент бы
        // прислал пакет приземления, а его нет. Если мы под этим флагом стоим на месте
        // дольше трёх секунд, спрашиваем у карты высоту под ногами и встаём на неё.
        // Это тот же приём, что уже вернул на землю зависших в воздухе.
        static uint32 const FORBIDDEN = MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FALLING
            | MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_ROOT;
        // МЕЛКОВОДЬЕ — НЕ ПРЕПЯТСТВИЕ. Здесь стояло IsInWater(), а оно истинно и когда
        // спутник стоит по щиколотку у берега. Замер после включения этой диагностики:
        // «шаг запрещён — вода 1» у всех застрявших на сборе, при нуле прочих причин, и
        // расстояние до цели не менялось ни на ярд за двадцать секунд. Настоящее плавание
        // и так отсекается флагом MOVEMENTFLAG_SWIMMING в списке запрещённых выше.
        bool const badFlags = (self->GetUnitMovementFlags() & FORBIDDEN) != 0;
        if (badFlags || self->GetTransport()
            || self->IsFalling() || self->IsFlying())
        {
            c.FrozenMs += uint32(dt * 1000.0f);
            if (!c.FrozenNoted)
            {
                c.FrozenNoted = true;
                TC_LOG_INFO("server.worldserver",
                    "Constellation ЗАМЕР {}: шаг запрещён — флаги {:X}, транспорт {}, "
                    "падение {}, полёт {} (вода {} — уже не помеха)",
                    self->GetName(), self->GetUnitMovementFlags(),
                    self->GetTransport() ? 1 : 0,
                    self->IsFalling() ? 1 : 0, self->IsFlying() ? 1 : 0,
                    self->IsInWater() ? 1 : 0);
            }

            if (c.FrozenMs > 3000 && (self->IsFalling() || badFlags))
            {
                c.FrozenMs = 0;
                float const gz = self->GetMap()->GetHeight(self->GetPhaseShift(),
                    self->GetPositionX(), self->GetPositionY(), self->GetPositionZ(), true, 50.0f);
                if (gz > INVALID_HEIGHT)
                {
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ЗАМЕР {}: снимаю залипшее падение, {:.1f} -> {:.1f}",
                        self->GetName(), self->GetPositionZ(), gz);
                    Position down(self->GetPositionX(), self->GetPositionY(), gz,
                                  self->GetOrientation());
                    SendMove(c, self, down, 0);
                    c.Moving = false;
                }
            }
            StopMoving(c, self);
            return false;               // не наш случай; выручит срок состояния
        }
        c.FrozenMs = 0;

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
            PathGenerator other(self);      // ЖИВЁТ ДО КОНЦА ВЕТКИ: путь из него используется ниже
            bool built = path.CalculatePath(tx, ty, tz, false)
                && !(path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_SHORTCUT));
            Movement::PointsArray const* pts = built ? &path.GetPath() : nullptr;
            float aimX = tx, aimY = ty;

            // ТА ЖЕ ТОЧКА, НО С ДРУГОГО ЯРУСА — ОДНА ПОПЫТКА (замер: тип 84, «конец пути далеко
            // от полигона», и разница высот в полсотни ярдов при двадцати пяти по плоскости).
            // Высоту цели мы ищем ВНИЗ от своего яруса, и это верно на древе; но стоя на уступе
            // над долиной, вниз находится её дно, куда сетка пути не даёт. Спрашиваем высоту
            // сверху: если цель на нашем ярусе, это её и вернёт.
            if (!built && destFix)
            {
                // ВЫШЕ СЕБЯ НЕ ЦЕЛИМСЯ (Кодекс). MAX_HEIGHT возвращает САМЫЙ ВЕРХНИЙ слой — это та
                // самая ошибка Тельдрассила: к крыше и к ветке путь тоже строится, а стоять там
                // спутнику незачем. Измеренный случай — поверхность НИЖЕ нас — условием сохраняется.
                float const zAbove = self->GetMap()->GetHeight(self->GetPhaseShift(), tx, ty, MAX_HEIGHT);
                if (zAbove > INVALID_HEIGHT && zAbove <= self->GetPositionZ() + 5.0f
                    && std::fabs(zAbove - tz) > 5.0f
                    && other.CalculatePath(tx, ty, zAbove, false)
                    && !(other.GetPathType() & (PATHFIND_NOPATH | PATHFIND_SHORTCUT)))
                {
                    built = true;
                    pts = &other.GetPath();
                    tz = zAbove;
                    destFix->Relocate(tx, ty, zAbove);      // поправка живёт дальше этого шага
                    ++_otherTier;
                }
            }
            if (!built)
            {
                float const whole = self->GetExactDist2d(tx, ty);
                float const ang = self->GetAbsoluteAngle(tx, ty);
                // ДОЛЯМИ ПУТИ, А НЕ ОТ ПЯТНАДЦАТИ ЯРДОВ. Прежний цикл начинался с половины пути
                // и требовал reach >= 15: для цели в 25 ярдах он не выполнялся ни разу, и
                // короткие, но «вертикальные» отказы уходили прямо в тупик (замер).
                static float const parts[3] = { 0.75f, 0.5f, 0.25f };
                float lastReach = -1.0f;
                for (float part : parts)
                {
                    float const reach = std::max(5.0f, std::min(whole * part, 150.0f));
                    if (std::fabs(reach - lastReach) < 0.01f)
                        continue;               // пол в пять ярдов сравнял доли — второй раз незачем
                    lastReach = reach;
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
                c.LastPathType = uint32(path.GetPathType());
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
                        "я {:.0f} {:.0f} {:.1f} -> цель {:.0f} {:.0f} {:.1f}, по плоскости {:.0f}, по высоте {:.1f}"
                        " (ярусом выше помогло уже {} раз, промежуточными точками {})",
                        self->GetName(), uint32(path.GetPathType()), uint32(path.GetPath().size()),
                        self->GetMapId(),
                        self->GetPositionX(), self->GetPositionY(), self->GetPositionZ(),
                        tx, ty, tz, self->GetExactDist2d(tx, ty), tz - self->GetPositionZ(),
                        _otherTier, _hops);
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

        // ЗАБЕГ К ТЕЛУ, И ПОДЪЁМ В ТИХОМ МЕСТЕ РЯДОМ С НИМ.
        //
        // Ядро уже перенесло призрака на кладбище, поэтому вести его туда нечего — он там.
        // Игрок делает обратное: бежит к телу и поднимается на нём, чтобы не пробиваться от
        // кладбища заново через возродившихся мобов. Так и здесь.
        //
        // Подъём — не в самой точке смерти: ядро разрешает поднять тело с 39 ярдов, и в этом
        // круге почти всегда есть место подальше от того, кто нас убил. Точка выбирается один
        // раз за смерть: дюжина кандидатов по кольцу, у каждого проверяется земля, обрыв и
        // вода, выигрывает тот, до чьих ближайших враждебных дальше всего.
        //
        // Условия подъёма сторожит само ядро (MiscHandler.cpp): дух отпущен, тело есть,
        // тридцать секунд после отпускания прошли, и мы в радиусе. Мы доходим и просим.
        // СМЕРТЕЛЬНОЕ МЕСТО НЕ ОТМЕНЯЕТ ЗАБЕГ — ОНО УЖЕСТОЧАЕТ ТРЕБОВАНИЕ К ТОЧКЕ.
        //
        // Первая редакция при трёх гибелях в окне пропускала забег и шла к целительнице —
        // то есть обратно на кладбище, где спутника и убивают. Условие работало против
        // собственной цели: именно там выбор тихой точки и нужен.
        bool const deadly = c.DeathAt.size() >= 3;
        if (!c.CorpseGaveUp && self->HasCorpse()
            && self->GetCorpseLocation().GetMapId() == self->GetMapId())
        {
            WorldLocation const& body = self->GetCorpseLocation();
            float const toBody = self->GetExactDist2d(body.GetPositionX(), body.GetPositionY());

            // ПОДАЛЬШЕ ОТ УБИЙЦЫ, НО В РАДИУСЕ ПОДЪЁМА. Выбираем, только когда тело уже
            // близко: сетка вокруг призрака загружена именно здесь.
            // ВЫБИРАЕМ, ТОЛЬКО ДОЙДЯ ДО ТЕЛА (Кодекс на выходе). Обзор врагов идёт на 90
            // ярдов от НАС; если выбирать за 45 ярдов до тела, враг в тридцати ярдах от
            // кандидата окажется в ста пяти от нас и в обзор не попадёт — «тихая» точка
            // перестанет что-либо значить. С двенадцати ярдов кандидат не дальше 42, враг
            // у него не дальше 72, и всё это внутри обзора.
            if (!c.RevivePicked && toBody < 12.0f)
            {
                std::list<Creature*> near;
                Trinity::AnyUnitInObjectRangeCheck check(self, 90.0f);
                Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, near, check);
                Cell::VisitGridObjects(self, searcher, 90.0f);

                // ВРАЖДЕБЕН ЛИ ОН МНЕ, А НЕ «МОГУ ЛИ Я ЕГО БИТЬ». Точку выбирает ПРИЗРАК, а
                // мёртвый не может атаковать никого — IsValidAttackTarget отвечал «нет» про
                // всех, и любое место читалось тихим: «ближайший враг не найден» там, где
                // прибор гибели в той же точке насчитывал четырнадцать враждебных.
                auto quietness = [&](float x, float y) -> float
                {
                    float worst = 1000.0f;
                    for (Creature* cr : near)
                        if (cr->IsAlive() && cr->IsHostileTo(self))
                            worst = std::min(worst, cr->GetExactDist2d(x, y));
                    return worst;
                };

                // ДВА КОЛЬЦА И ШЕСТНАДЦАТЬ НАПРАВЛЕНИЙ. Радиус подъёма 39 ярдов — это большой
                // круг, и одного кольца из двенадцати точек мало, чтобы в нём нашлось тихое
                // место. Ближнее кольцо предпочтительнее: меньше бежать.
                float bestScore = quietness(body.GetPositionX(), body.GetPositionY());
                Position bestPos(body.GetPositionX(), body.GetPositionY(), body.GetPositionZ());
                for (float ring : { 18.0f, 30.0f })
                    for (int i = 0; i < 16; ++i)
                    {
                        float const ang = float(i) * (2.0f * float(M_PI) / 16.0f);
                        float const rx = body.GetPositionX() + ring * std::cos(ang);
                        float const ry = body.GetPositionY() + ring * std::sin(ang);
                        float const rz = self->GetMap()->GetHeight(self->GetPhaseShift(), rx, ry,
                            body.GetPositionZ() + 5.0f, true);
                        if (rz <= INVALID_HEIGHT || std::fabs(rz - body.GetPositionZ()) > 20.0f)
                            continue;                   // нет земли или обрыв
                        if (self->GetMap()->IsInWater(self->GetPhaseShift(), rx, ry, rz))
                            continue;                   // в воде не поднимаются
                        float const score = quietness(rx, ry);
                        if (score > bestScore + 1.0f)   // +1: при равной тишине ближнее лучше
                            { bestScore = score; bestPos.Relocate(rx, ry, rz); }
                    }

                // ГДЕ УЖЕ УБИВАЛИ ТРИЖДЫ — ГОДИТСЯ ТОЛЬКО ПО-НАСТОЯЩЕМУ ТИХОЕ МЕСТО.
                // Иначе подъём будет означать новую гибель через секунды, и круг продолжится.
                if (deadly && bestScore < 30.0f)
                {
                    c.CorpseGaveUp = true;
                    c.ReviveMs = 0;
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ТЕЛО {}: у тела тихого места нет (ближайший враг в {:.0f}) — иду к целительнице",
                        self->GetName(), bestScore);
                    return;
                }

                c.RevivePos = bestPos;
                c.RevivePicked = true;
                TC_LOG_INFO("server.worldserver",
                    "Constellation ТЕЛО {}: поднимусь в {:.0f} {:.0f} {:.0f} — {:.0f} ярдов от тела, "
                    "ближайший враг {}",
                    self->GetName(), bestPos.GetPositionX(), bestPos.GetPositionY(), bestPos.GetPositionZ(),
                    bestPos.GetExactDist2d(body.GetPositionX(), body.GetPositionY()),
                    bestScore > 900.0f ? std::string("не найден") : Trinity::StringFormat("в {:.0f} ярдах", bestScore));
            }

            float const tx = c.RevivePicked ? c.RevivePos.GetPositionX() : body.GetPositionX();
            float const ty = c.RevivePicked ? c.RevivePos.GetPositionY() : body.GetPositionY();
            float const tz = c.RevivePicked ? c.RevivePos.GetPositionZ() : body.GetPositionZ();
            float const toSpot = self->GetExactDist2d(tx, ty);

            if (toSpot > 6.0f)
            {
                if (!c.CorpseRunNoted)
                {
                    c.CorpseRunNoted = true;
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ТЕЛО {}: бегу к своему телу, {:.0f} ярдов",
                        self->GetName(), toBody);
                }
                StepToward(c, self, tx, ty, tz, 5.0f, dt);
                c.ReviveMs = 0;                 // духом бежим каждый такт, а не раз в две секунды
                // БЕГ — НЕ ПОПЫТКА ВОСКРЕШЕНИЯ (Кодекс на выходе). ReviveTries считает вызовы
                // Revive и рассчитан на одну попытку в две секунды; ежетактный бег исчерпывал
                // его за пять секунд, и спутник уходил ждать пять минут. У бега свой предел
                // по времени и свой счётчик отказов подъёма.
                c.ReviveTries = 0;
                if (!c.Stalled && c.CorpseRunMs < 300000)
                {
                    c.CorpseRunMs += uint32(dt * 1000.0f);
                    return;
                }
                TC_LOG_INFO("server.worldserver",
                    "Constellation ТЕЛО {}: до тела не добежать — иду к целительнице", self->GetName());
                c.CorpseGaveUp = true;
            }
            else
            {
                Corpse* body2 = self->GetCorpse();
                if (!body2)
                {
                    c.CorpseGaveUp = true;      // тела уже нет — только целительница
                    c.ReviveMs = 0;
                    return;
                }

                // РАДИУС ЯДРО МЕРЯЕТ В ПРОСТРАНСТВЕ (Кодекс): кольцо в 28 ярдов плюс перепад
                // высоты до 20 плюс остановка в шести давали до 39.45 при пределе 39. Если
                // мы формально дошли, но по пространству далеко — подходим к самому телу.
                if (!body2->IsWithinDistInMap(self, 35.0f, true))
                {
                    StepToward(c, self, body.GetPositionX(), body.GetPositionY(),
                        body.GetPositionZ(), 4.0f, dt);
                    c.ReviveMs = 0;
                    c.ReviveTries = 0;
                    c.CorpseRunMs += uint32(dt * 1000.0f);
                    if (c.CorpseRunMs < 300000 && !c.Stalled)
                        return;
                    c.CorpseGaveUp = true;
                    return;
                }

                // ЗАДЕРЖКА ПОДЪЁМА — 30, 60 ИЛИ 120 СЕКУНД (Кодекс: Player.cpp:161), и ждать
                // её не значит получить отказ. Спрашиваем ядро о сроке и ждём остаток молча.
                time_t const ready = body2->GetGhostTime()
                    + time_t(self->GetCorpseReclaimDelay(body2->GetType() == CORPSE_RESURRECTABLE_PVP));
                time_t const now = GameTime::GetGameTime();
                if (ready > now)
                {
                    c.ReviveMs = uint32(std::min<time_t>(ready - now, 5) * 1000);
                    c.ReviveTries = 0;
                    if (!c.ReclaimWaitMs)
                    {
                        c.ReclaimWaitMs = 1;
                        TC_LOG_INFO("server.worldserver",
                            "Constellation ТЕЛО {}: жду {} с до подъёма",
                            self->GetName(), uint32(ready - now));
                    }
                    return;
                }

                WorldPacket raw(CMSG_RECLAIM_CORPSE);
                WorldPackets::Misc::ReclaimCorpse reclaim(std::move(raw));
                reclaim.CorpseGUID = body2->GetGUID();
                c.Session->HandleReclaimCorpse(reclaim);
                c.ReviveTries = 0;              // подъём у тела считается своим счётчиком
                if (self->IsAlive())
                {
                    ++_revived;
                    c.ReviveGaveUp = false;
                    c.GraveWalkNoted = false;
                    c.HealerStepNoted = false;
                    c.HealerRings = 0;
                    RepairIfBroken(self, "после смерти");
                    c.BrokenNoted = false;
                    c.TravelCooldownMs = std::max<uint32>(c.TravelCooldownMs, 300000);
                    Switch(c, self, Behavior::Recovering, "поднялся у своего тела");
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ТЕЛО {}: поднялся у своего тела", self->GetName());
                    return;
                }
                // ОТКАЗ, И ЭТО УЖЕ НАСТОЯЩИЙ ОТКАЗ: срок вышел, расстояние проверено.
                // Две такие попытки — и на кладбище (оператор).
                c.ReviveMs = 3000;
                if (++c.CorpseTries >= 2)
                {
                    c.CorpseGaveUp = true;
                    c.ReviveMs = 0;
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ТЕЛО {}: две попытки подняться не прошли — иду к целительнице",
                        self->GetName());
                }
                return;
            }
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
        {
            // ОБРАТНО НА КЛАДБИЩЕ — САМИ. Ядро переносит призрака туда ОДИН раз, при
            // отпускании духа (RepopAtGraveyard); после неудачного бега к телу мы стоим у
            // трупа, и «ждать, пока перенесёт» значит ждать вечно — 26 «не может
            // воскреснуть» за окно, все у своих трупов. Спрашиваем у ядра то же кладбище,
            // которым оно нас переносило (GetClosestGraveyard), и идём пешком: призрак
            // быстр, и его никто не трогает. Дорога не считается попыткой — счётчик
            // остаётся для настоящей беды: на кладбище, а целительницы в 60 ярдах нет.
            // ОТ ТРУПА, А НЕ ОТ СЕБЯ. Ядро выбирает кладбище от места смерти и при отпускании
            // духа (RepopAtGraveyard), и у целительницы (SendSpiritResurrect: corpseGrave).
            // Призрак, забредший в тупик по дороге к телу, от СВОЕЙ позиции выбирал другое
            // кладбище — за водой, на другом ярусе, — до которого пути нет; трое ходили так
            // дважды подряд. От трупа выбирается то, куда его телепортировали, — и потому оно
            // гораздо вероятнее достижимо. Обратная достижимость при этом НЕ доказана (Кодекс):
            // навмеш бывает несвязным по ярусам и воде, а путь вперёд мог включать падение.
            WorldLocation const from = self->HasCorpse() ? self->GetCorpseLocation() : WorldLocation(*self);
            WorldSafeLocsEntry const* grave = sObjectMgr->GetClosestGraveyard(from, self->GetTeam(), self);
            if (!grave || grave->Loc.GetMapId() != self->GetMapId())
                return;                 // кладбища на этой карте ядро не знает — ждём как прежде
            float const toGrave = self->GetExactDist2d(grave->Loc.GetPositionX(), grave->Loc.GetPositionY());
            if (toGrave <= 20.0f)
                return;                 // уже там, а целительницы нет — вот это и есть беда
            if (!c.GraveWalkNoted)
            {
                c.GraveWalkNoted = true;
                c.GraveWalkLast = toGrave;
                c.Stalled = false;      // новое намерение — как в 5459: прежний тупик не наш
                TC_LOG_INFO("server.worldserver",
                    "Constellation ТЕЛО {}: целительницы нет в 60 ярдах — иду на кладбище {} ({:.0f} ярдов); "
                    "труп на карте {} в {:.0f} {:.0f} {:.0f}",
                    self->GetName(), grave->ID, toGrave, from.GetMapId(),
                    from.GetPositionX(), from.GetPositionY(), from.GetPositionZ());
            }
            // ПРОГРЕСС — ПО РАССТОЯНИЮ, НЕ ПО ФЛАГУ. c.Stalled липкий: его ставят застревание
            // и неудачный поиск пути, а снимают только «дошёл» и «новое намерение» — ни того,
            // ни другого здесь не бывает. Первый затор на дороге обнулял бы всю дорогу.
            if (toGrave < c.GraveWalkLast - 1.0f)
            {
                c.GraveWalkLast = toGrave;
                c.ReviveTries = 0;      // стало ближе — это дорога, а не попытка
                c.Stalled = false;
            }
            StepToward(c, self, grave->Loc.GetPositionX(), grave->Loc.GetPositionY(),
                grave->Loc.GetPositionZ(), 4.0f, dt);
            return;
        }

        if (!self->IsWithinDistInMap(healer, INTERACTION_DISTANCE))
        {
            // К ЦЕЛИТЕЛЬНИЦЕ — КАК К ЛЮБОМУ NPC. Ядро пускает с GetCombatReach()+4
            // (Player.cpp:1903). Пятеро стояли в десяти ярдах от целительницы Рэтчета и
            // сдавались молча: шаг к её точке по сетке застревал — NPC часто стоят там, куда
            // путь не строится. У модуля для этого есть свой ход, тот же, что у квестодателей:
            // точка контакта -> застряли -> достижимая точка рядом (кольца 3/5/8 ярдов) -> шаг.
            // Прямой ход мимо сетки здесь стоял и снят (оператор): это обход поломки, не починка.
            float const toHealer = self->GetExactDist(healer);
            // ДОШЛИ ДО ТОЧКИ КОЛЬЦА, А ДИСТАНЦИИ ВСЁ НЕТ — ТОЧКУ ОТПУСКАЕМ (Кодекс): иначе
            // ApproachPoint вечно возвращает её, а StepToward тут же «приходит». Отсюда
            // разрешён ещё один поиск, второй и последний на эту смерть.
            if (c.RingHeld && c.ApproachFor == healer->GetGUID()
                && self->GetExactDist2d(c.ApproachX, c.ApproachY) < 2.5f)
            {
                // достигнутая, но бесполезная точка — тоже отвергнутая (Кодекс): запомнить
                // до того, как отпустить, иначе второй поиск предложит её же
                c.HealerRefused.Relocate(c.ApproachX, c.ApproachY, c.ApproachZ);
                c.HealerRefusedSet = true;
                c.RingHeld = false;
                if (c.HealerRings < 2)
                    c.RingTried = false;
            }
            float ax, ay, az;
            ApproachPoint(c, healer, self, ax, ay, az, ms);
            StepToward(c, self, ax, ay, az, 2.0f, dt);
            if (c.Stalled)
            {
                // ОТКАЗ УДЕРЖАННОЙ ТОЧКИ (Кодекс): застряли по дороге к самой точке кольца —
                // значит, и она не годится; отпускаем, чтобы второй поиск не упёрся в неё же.
                if (c.RingHeld && c.ApproachFor == healer->GetGUID())
                {
                    c.HealerRefused.Relocate(c.ApproachX, c.ApproachY, c.ApproachZ);
                    c.HealerRefusedSet = true;
                    c.RingHeld = false;
                    if (c.HealerRings < 2)
                        c.RingTried = false;
                }
                if (!c.HealerStepNoted)
                {
                    c.HealerStepNoted = true;
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ТЕЛО {}: до целительницы {} ({:.1f} ярдов) шаг по сетке застрял — ищу точку рядом",
                        self->GetName(), healer->GetName(), toHealer);
                }
                if (c.HealerRings < 2 && !c.RingTried)
                {
                    ++c.HealerRings;
                    if (FindReachableApproach(c, self, healer,
                                              c.HealerRefusedSet ? &c.HealerRefused : nullptr))
                        return;         // точка нашлась и удержана — следующий такт шагнёт к ней
                }
                c.ReviveTries += 5;     // колец нет или они исчерпаны — находка о карте, считаем попыткой
            }
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

            // ГИБЕЛИ ПОДРЯД — ЭТО НЕ НЕВЕЗЕНИЕ, ЭТО НЕ ТА ЗОНА.
            //
            // Считаем их в скользящем окне: победа его обнуляет (см. ветку победы), а три
            // гибели за десять минут означают, что спутник возрождается там же, где его
            // убивают, и выйти оттуда сам не может — отдыхать нельзя, он в бою; драться
            // нельзя, всё сломано. Так Эмрик набрал 86 гибелей за четверть часа.
            // НЕ ЗАТИРАЕМ ДЛИННУЮ ОТСРОЧКУ КОРОТКОЙ (Кодекс): если камень уже поставил
            // четверть часа, обычные пять минут после смерти его не укорачивают.
            c.TravelCooldownMs = std::max<uint32>(c.TravelCooldownMs, 300000);
            // и не возвращаться к убийце на половине здоровья — сперва отдышаться
            c.GraveWalkNoted = false;
            c.HealerStepNoted = false;
            c.HealerRings = 0;
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
        // ДАВНО ЛИ НЕ БЫЛО БОЯ: в бою счётчик обнуляется, вне боя растёт. По нему отступает
        // порог стаи, иначе стайные цели стали бы невыполнимы навсегда (разбор).
        if (self->IsInCombat())
            c.NoTargetMs = 0;
        else if (c.NoTargetMs < 3600000)
            c.NoTargetMs += diff;
        if (c.TaxiScanMs)
            c.TaxiScanMs = (c.TaxiScanMs <= diff) ? 0 : c.TaxiScanMs - diff;
        if (c.InnScanMs)
            c.InnScanMs = (c.InnScanMs <= diff) ? 0 : c.InnScanMs - diff;
        if (c.HearthCooldownMs)
            c.HearthCooldownMs = (c.HearthCooldownMs <= diff) ? 0 : c.HearthCooldownMs - diff;
        if (c.FlightCooldownMs)
            c.FlightCooldownMs = (c.FlightCooldownMs <= diff) ? 0 : c.FlightCooldownMs - diff;
        for (auto it = c.TaxiRetry.begin(); it != c.TaxiRetry.end();)
            if (it->second <= diff) it = c.TaxiRetry.erase(it); else { it->second -= diff; ++it; }
        if (c.LiveEnderMs)
            c.LiveEnderMs = (c.LiveEnderMs <= diff) ? 0 : c.LiveEnderMs - diff;
        if (c.IdleScanMs)
            c.IdleScanMs = (c.IdleScanMs <= diff) ? 0 : c.IdleScanMs - diff;
        for (auto it = c.GatherBackoff.begin(); it != c.GatherBackoff.end(); )
        {
            if (it->second <= diff)
                it = c.GatherBackoff.erase(it);
            else
                { it->second -= diff; ++it; }
        }
        for (auto it = c.TalkBackoff.begin(); it != c.TalkBackoff.end(); )
        {
            if (it->second <= diff)
                it = c.TalkBackoff.erase(it);
            else
                { it->second -= diff; ++it; }
        }
        if (c.TalkUnreachable.size() > 40)
            c.TalkUnreachable.clear();
        if (c.EquipScanMs)
            c.EquipScanMs = (c.EquipScanMs <= diff) ? 0 : c.EquipScanMs - diff;
        for (auto it = c.TriggerSentMs.begin(); it != c.TriggerSentMs.end();)
            if (it->second <= diff) it = c.TriggerSentMs.erase(it); else { it->second -= diff; ++it; }
        if (c.ToolActionMs)
            c.ToolActionMs = (c.ToolActionMs <= diff) ? 0 : c.ToolActionMs - diff;
        if (c.SeekCooldownMs)
            c.SeekCooldownMs = (c.SeekCooldownMs <= diff) ? 0 : c.SeekCooldownMs - diff;
        if (c.IdleDiagMs)
            c.IdleDiagMs = (c.IdleDiagMs <= diff) ? 0 : c.IdleDiagMs - diff;
        for (auto it = c.SeekBackoff.begin(); it != c.SeekBackoff.end();)
            if (it->second <= diff) it = c.SeekBackoff.erase(it); else { it->second -= diff; ++it; }
        for (auto it = c.TalkRetry.begin(); it != c.TalkRetry.end();)
            if (it->second <= diff) it = c.TalkRetry.erase(it); else { it->second -= diff; ++it; }
        for (auto it = c.EquipRefused.begin(); it != c.EquipRefused.end();)
            if (it->second <= diff) it = c.EquipRefused.erase(it); else { it->second -= diff; ++it; }
        for (auto it = c.SellRefused.begin(); it != c.SellRefused.end();)
            if (it->second <= diff) it = c.SellRefused.erase(it); else { it->second -= diff; ++it; }
        for (auto it = c.VendorNoSell.begin(); it != c.VendorNoSell.end();)
            if (it->second <= diff) it = c.VendorNoSell.erase(it); else { it->second -= diff; ++it; }
        if (c.FleePauseMs)
            c.FleePauseMs = (c.FleePauseMs <= diff) ? 0 : c.FleePauseMs - diff;
        // ОКНО ГИБЕЛЕЙ ИСТЕКАЕТ САМО (Кодекс на выходе). Чистка только при новой смерти
        // означала, что три давние гибели держат место «смертельным» навсегда: следующая
        // одиночная гибель сразу читала три и пропускала забег к телу.
        if (!c.DeathAt.empty())
        {
            uint32 const nowMs = GameTime::GetGameTimeMS();
            while (!c.DeathAt.empty() && getMSTimeDiff(c.DeathAt.front(), nowMs) > 600000)
                c.DeathAt.pop_front();
        }
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
            // СРОК — У КАЖДОЙ ЗАПИСИ СВОЙ, ДЕСЯТЬ МИНУТ (Кодекс): общая очистка раз в пять минут
            // возвращала в выбор всех разом, и недостижимый выбирался снова по расписанию.
            for (auto it = c.GiverUnreachable.begin(); it != c.GiverUnreachable.end();)
                if (it->second <= diff) it = c.GiverUnreachable.erase(it); else { it->second -= diff; ++it; }


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
            // ГИБЕЛЬ СЧИТАЕТСЯ ЗДЕСЬ, ОДИН РАЗ, И НЕЗАВИСИМО ОТ СПОСОБА ПОДЪЁМА (Кодекс).
            // Раньше отметка ставилась только при подъёме у целительницы, поэтому круг
            // «поднялся у тела -> снова погиб» никогда не доходил до трёх — и ни отход, ни
            // камень не включались вовсе. И сброс забега висел на условии «умер не в стою»,
            // хотя переход живой->мёртвый от режима не зависит.
            if (!c.DeathCounted)
            {
                c.DeathCounted = true;
                uint32 const nowMs = GameTime::GetGameTimeMS();
                c.DeathAt.push_back(nowMs);
                // НОВАЯ СМЕРТЬ — ЧИСТОЕ СОСТОЯНИЕ ПОДХОДА К ЦЕЛИТЕЛЬНИЦЕ (Кодекс): сбрасывать
                // в момент самого перехода, а не по косвенным признакам в ветке воскрешения.
                c.RingHeld = false;
                c.RingTried = false;
                c.ApproachFor.Clear();
                c.HealerRings = 0;
                c.HealerRefusedSet = false;
                c.HealerStepNoted = false;

                // ГДЕ И ОТ КОГО — ПИШЕМ КАЖДУЮ ГИБЕЛЬ, ПОКА НЕ ЗНАЕМ ПРИЧИНЫ.
                //
                // Камень отсюда убран: он переносит к трактиру и спасением из боя не является
                // (оператор; журнал Легиона, задача 0014 — это шаг маршрута, не выход). А что
                // именно добивает спутника на кладбище, пока НЕИЗВЕСТНО, и придумывать
                // следующее средство вслепую значит повторить ту же ошибку. Поэтому здесь
                // прибор: зона, место, и три ближайших враждебных с их уровнями.
                {
                    std::string who;
                    std::list<Creature*> near;
                    Trinity::AnyUnitInObjectRangeCheck check(self, 50.0f);
                    Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, near, check);
                    Cell::VisitGridObjects(self, searcher, 50.0f);
                    std::vector<std::pair<float, Creature*>> hostiles;
                    for (Creature* cr : near)
                        if (cr->IsAlive() && cr->IsHostileTo(self))
                            hostiles.push_back({ self->GetExactDist(cr), cr });
                    std::sort(hostiles.begin(), hostiles.end(),
                        [](auto const& a, auto const& b) { return a.first < b.first; });
                    for (size_t i = 0; i < hostiles.size() && i < 3; ++i)
                        who += Trinity::StringFormat("{} ({}, сырой ур {} / для нас {}, {:.0f} ярд){}",
                            hostiles[i].second->GetName(), hostiles[i].second->GetEntry(),
                            uint32(hostiles[i].second->GetLevel()),
                            uint32(hostiles[i].second->GetLevelForTarget(self)), hostiles[i].first,
                            i + 1 < hostiles.size() && i < 2 ? "; " : "");
                    // КТО БИЛ — ИЗ СВОЕЙ ЖЕ ПАМЯТИ, А НЕ ИЗ ОСМОТРА ПОСЛЕ СМЕРТИ.
                    //
                    // Осмотр говорил «рядом враждебных 0 — никого» в 32 гибелях из 48, и это
                    // читалось как «не бой». Журнал рядом говорил обратное: «ПОГИБ — у него 15%».
                    // Разгадка в расстоянии: лагерь троллей на y≈318-360, а Бренна погибла на
                    // y=452 — моб сбросил бой и ушёл домой раньше, чем прибор осмотрелся.
                    // Осмотр мира после смерти в принципе не может назвать убийцу; c.TargetGuid
                    // может, им же пользуется LogFightOutcome ниже.
                    // НАША ЦЕЛЬ — это НАМЕРЕНИЕ, а не убийца (Кодекс): спутник мог её ещё не
                    // ударить, а прилететь могло от второго моба или от обрыва. Кто именно бил и
                    // чем кончилось, говорит соседняя строка БОЙ — она ведётся по ходу боя, а не
                    // осмотром после смерти, и потому знает то, чего осмотр знать не может.
                    std::string foe = "цели не было";
                    if (Creature* t = ObjectAccessor::GetCreature(*self, c.TargetGuid))
                        foe = Trinity::StringFormat("{} ({}, {:.0f} ярд, у него {:.0f}%)",
                            t->GetName(), t->GetEntry(), self->GetExactDist(t), t->GetHealthPct());
                    else if (!c.TargetGuid.IsEmpty())
                        foe = "цель уже недоступна";

                    // ОКРУЖЕНИЕ ТРУПА — именно окружение, а не причина урона (Кодекс).
                    // Землю ищем от своего Z с запасом: трассировка от MAX_HEIGHT цепляет мост
                    // или платформу НАД трупом и даёт отрицательную бессмыслицу. Ненайденную
                    // поверхность (INVALID_HEIGHT) называем словом, а не числом в сто тысяч.
                    ZLiquidStatus const liq = self->GetLiquidStatus();
                    float const ground = self->GetMap()->GetHeight(self->GetPhaseShift(),
                        self->GetPositionX(), self->GetPositionY(), self->GetPositionZ() + 2.0f);
                    std::string const under = ground <= INVALID_HEIGHT
                        ? std::string("земля не найдена")
                        : Trinity::StringFormat("до земли {:.1f}", self->GetPositionZ() - ground);
                    char const* water = (liq & LIQUID_MAP_UNDER_WATER) ? "под водой"
                                      : (liq & LIQUID_MAP_IN_WATER)    ? "в воде"
                                      : (liq & LIQUID_MAP_ABOVE_WATER) ? "над водой" : "без воды";
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ГИБЕЛЬ {} (ур {}): зона {}, место {:.0f} {:.0f} {:.0f}, "
                        "цель модуля {}, {}, {}, гибелей в окне {}, рядом враждебных {} — {}",
                        self->GetName(), uint32(self->GetLevel()), self->GetZoneId(),
                        self->GetPositionX(), self->GetPositionY(), self->GetPositionZ(),
                        foe, water, under,
                        uint32(c.DeathAt.size()), uint32(hostiles.size()),
                        who.empty() ? std::string("никого") : who);
                }
                while (!c.DeathAt.empty() && getMSTimeDiff(c.DeathAt.front(), nowMs) > 600000)
                    c.DeathAt.pop_front();      // скользящее окно в десять минут
                c.CorpseRunNoted = false;
                c.CorpseRunMs = 0;
                c.CorpseTries = 0;
                c.CorpseGaveUp = false;
                c.RevivePicked = false;
                c.ReclaimWaitMs = 0;
            }
            if (c.Mode != Behavior::Idle)
            {
                if (c.Mode == Behavior::Attacking || c.Mode == Behavior::ApproachingTarget)
                    LogFightOutcome(self, ObjectAccessor::GetCreature(*self, c.TargetGuid), "ПОГИБ", c);
                // ПОГИБ ПО ДОРОГЕ НА СДАЧУ — не идти той же дорогой четверть часа. Бриенна
                // 5-го уровня шла сдавать «Hero's Call: Westfall!» в Западный край, гибла
                // и через минуту шла снова: минутная отсрочка короче самой дороги.
                if (c.Mode == Behavior::TurningIn && c.TurnInQuest)
                    c.TurnInBackoff[c.TurnInQuest] = 900000;
                Switch(c, self, Behavior::Idle, "погиб");
            }
            // умерли по дороге — не бежать той же дорогой снова (Кодекс, проход 5):
            // без этого смерть возвращала в «стою», откуда та же цель выбиралась опять
            c.TravelCooldownMs = 300000;
            Revive(c, self, dt);
            return;
        }
        c.DeathCounted = false;             // живы — следующая гибель будет новой
        // ЛЕТИМ — НЕ ТРОГАЕМ НИЧЕГО. Ядро ведёт персонажа по маршруту, любые наши шаги и пакеты
        // движения в это время спорили бы с ним.
        if (self->IsInFlight())
            return;

        // КАМЕНЬ ЧИТАЕТСЯ ДЕСЯТЬ СЕКУНД И РВЁТСЯ ДВИЖЕНИЕМ, ПОЭТОМУ МЫ СТОИМ — НО ТОЛЬКО ЗДЕСЬ,
        // ПОСЛЕ СМЕРТИ И ПОЛЁТА (Кодекс). Раньше эта пауза стояла в самом начале такта и на те же
        // десять секунд глушила обработку гибели и нападения. И снимаем её сразу, как только
        // читать нечего: бой, чужой каст оборвал, или каста уже нет.
        if (c.HearthCastMs)
        {
            c.HearthCastMs = (c.HearthCastMs <= diff) ? 0 : c.HearthCastMs - diff;
            if (self->IsInCombat() || !self->IsNonMeleeSpellCast(false))
                c.HearthCastMs = 0;             // оборвалось или уже закончилось — живём дальше
            else
                return;                          // читаем — не мешаем себе же
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

                // ОБНОВКИ — ПЕРВЫМ ДЕЛОМ И НЕЧАСТО: раз в полминуты, одна за проход.
                // Ничего не переключает, просто улучшает всё, что будет дальше.
                if (idleScan && !c.EquipScanMs)
                {
                    c.EquipScanMs = 30000 + (c.Guid.GetCounter() % 5000u);
                    if (!EquipBags(c, self))
                        EquipUpgrades(c, self);
                }
                // В БОЮ, А ДРАТЬСЯ НЕЧЕМ — ОТХОДИМ, А НЕ СТОИМ.
                //
                // Замер: спутник шестого уровня набрал 46 гибелей за восемь минут, стоя у
                // целительницы душ со сломанным оружием. Он в бою, поэтому отдых отвергается;
                // драться нечем, поэтому цель не ищется; уйти он не умел. Камень возвращения
                // тут не спасает: его каст длится десять секунд и рвётся от первого удара.
                //
                // Поэтому первым делом — уйти. Направление берём от того, кто на нас напал:
                // прочь от ближайшего. Мобы отцепляются на своём поводке, бой спадает, и
                // дальше работают и отдых, и камень, и всё остальное.
                if (self->IsInCombat() && BrokenForFight(c, self) && !c.FleePauseMs)
                {
                    Unit* nearest = nullptr;
                    float best = 1000.0f;
                    for (Unit* a : self->getAttackers())
                    {
                        float const d = self->GetExactDist(a);
                        if (d < best)
                            { best = d; nearest = a; }
                    }
                    if (!nearest)
                        nearest = self->getAttackerForHelper();
                    if (!nearest)
                    {
                        // БОЙ БЕЗ НАПАДАЮЩИХ — не пустая ветка, а выход из него (Кодекс):
                        // иначе спутник вечно «в бою», не отдыхает и не уходит.
                        self->CombatStop(true);
                        c.FleeMs = 0;
                        c.FleeHasPoint = false;
                    }
                    else
                    {
                        c.FleeMs += slice;
                        if (!c.FleeNoted)
                        {
                            c.FleeNoted = true;
                            TC_LOG_INFO("server.worldserver",
                                "Constellation ОТХОД {}: в бою с {} ({}), драться нечем — отхожу",
                                self->GetName(), nearest->GetName(), nearest->GetEntry());
                        }
                        // ТОЧКА ВЫБИРАЕТСЯ ОДИН РАЗ И НЕ ДВИГАЕТСЯ ЗА НАМИ (Кодекс). Прежняя
                        // «сорок ярдов от текущего места» была убегающим горизонтом: цель
                        // смещалась на каждом шаге, и путь строился заново.
                        //
                        // Направление тоже не одно: восемь кандидатов веером от «прочь от
                        // нападающего», и берётся первый, у которого есть высота, нет воды и
                        // рядом нет других враждебных. Это строка 9 каталога Легиона —
                        // «безопасное направление» — в honest минимальном виде.
                        c.FleeTotalMs += slice;     // ОБЩИЙ бюджет отхода, его смена точки не трогает
                        if (c.FleeTotalMs >= 90000)
                        {
                            c.FleePauseMs = 300000;
                            c.FleeHasPoint = false;
                            c.FleeMs = c.FleeTotalMs = 0;
                            TC_LOG_INFO("server.worldserver",
                                "Constellation ОТХОД {}: полторы минуты не отрываюсь — жду пять минут",
                                self->GetName());
                            return;
                        }
                        if (!c.FleeHasPoint || c.FleeMs >= 30000)
                        {
                            c.FleeMs = c.FleeHasPoint ? 0 : c.FleeMs;   // сменили точку — время заново
                            c.FleeHasPoint = false;
                            float const away = nearest->GetAbsoluteAngle(self);
                            static float const fan[8] = { 0.0f, 0.6f, -0.6f, 1.2f, -1.2f, 1.9f, -1.9f, 2.6f };
                            for (float off : fan)
                            {
                                float const ang = away + off;
                                float const fx = self->GetPositionX() + 45.0f * std::cos(ang);
                                float const fy = self->GetPositionY() + 45.0f * std::sin(ang);
                                float const fz = self->GetMap()->GetHeight(self->GetPhaseShift(),
                                    fx, fy, self->GetPositionZ() + 5.0f, true);
                                if (fz <= INVALID_HEIGHT || std::fabs(fz - self->GetPositionZ()) > 20.0f)
                                    continue;               // нет земли или обрыв
                                if (self->GetMap()->IsInWater(self->GetPhaseShift(), fx, fy, fz))
                                    continue;               // в воду не бежим
                                // и не в другой лагерь: рядом с точкой не должно быть враждебных
                                Position cand(fx, fy, fz);
                                bool crowded = false;
                                std::list<Creature*> near;
                                Trinity::AnyUnitInObjectRangeCheck check(self, 60.0f);
                                Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, near, check);
                                Cell::VisitGridObjects(self, searcher, 60.0f);
                                for (Creature* cr : near)
                                    if (cr->IsAlive() && self->IsValidAttackTarget(cr)
                                        && cr->GetExactDist2d(fx, fy) < 15.0f)
                                        { crowded = true; break; }
                                if (crowded)
                                    continue;
                                c.FleeTo = cand;
                                c.FleeHasPoint = true;
                                break;
                            }
                            if (!c.FleeHasPoint)
                            {
                                // ни одно направление не годится — не мечемся, ждём
                                c.FleePauseMs = 300000;
                                c.FleeMs = 0;
                                TC_LOG_INFO("server.worldserver",
                                    "Constellation ОТХОД {}: некуда отойти — жду пять минут", self->GetName());
                                return;
                            }
                        }
                        if (c.FleeHasPoint)
                        {
                            StepToward(c, self, c.FleeTo.GetPositionX(), c.FleeTo.GetPositionY(),
                                c.FleeTo.GetPositionZ(), 0.0f, dt);
                            // НАСТОЯЩИЙ ПРЕДЕЛ (Кодекс): минута отхода — и пять минут покоя,
                            // а не бесконечный шаг после «истечения» срока.
                            if (c.Stalled || c.FleeMs >= 60000)
                            {
                                c.FleePauseMs = 300000;
                                c.FleeHasPoint = false;
                                c.FleeMs = 0;
                            }
                            return;
                        }
                    }
                }
                else if (!self->IsInCombat() && (c.FleeMs || c.FleeNoted || c.FleeHasPoint))
                {
                    // ВЫШЛИ ИЗ БОЯ — пробуем камень сразу, не дожидаясь следующей гибели (Кодекс)
                    c.FleeMs = 0; c.FleeTotalMs = 0; c.FleeNoted = false; c.FleeHasPoint = false;
                }

                // СТОИМ ВНУТРИ ЗОНЫ ОСМОТРА? Трое людей стояли у самой шахты Фаргодип с
                // квестом «побывать в шахте» — ядро ждало пакет клиента, которого нет.
                if (idleScan)
                {
                    TouchAreaTriggers(c, self);
                    ReconcileLateCredit(c, self);   // поздний рост счётчика — до любого нового «бесплодно»
                    LearnTaxiNode(c, self);         // мимо полётного мастера не проходим молча
                    BindAtInn(c, self);             // и мимо трактирщика тоже: камень должен вести сюда
                }

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
                    // ХЛАМ — ТРЕТИЙ ПОВОД. Два замера подряд: походов ноль, потому что до «сумки
                    // почти полны» состав не доходит, а правило продажи так и не срабатывает.
                    // Шесть и больше стопок, которые правило продало бы, — идём продавать.
                    uint32 const sellable = (!stuffed && !helpless) ? SellableCount(c, self) : 0;
                    bool const clutter = sellable >= 6;                                  // продавец в обзоре
                    bool const clutterFar = sellable >= std::max<uint32>(12, BagCapacity(self) / 4);   // поход по карте

                    // МИМОХОДОМ: изношен, но дееспособен — только если торговец уже рядом.
                    // Двадцать ярдов это «прохожу мимо», а не «схожу-ка я за тридцать».
                    bool passingBy = false;
                    if (worn && !helpless && !stuffed)
                        if (Creature* near = FindVendorNear(c, self, false, true))
                            passingBy = self->IsWithinDistInMap(near, 20.0f);

                    if (helpless || stuffed || clutter || passingBy)
                    {
                        // ИЩЕМ ТОГО, КТО УМЕЕТ НУЖНОЕ. Полные сумки требуют продавца,
                        // поломка — ремонтника; идти к тому, кто не умеет, значит вернуться
                        // ни с чем и повторить через минуту (Кодекс).
                        if (Creature* vendor = FindVendorNear(c, self, stuffed || clutter, helpless || passingBy))
                        {
                            c.VendorGuid = vendor->GetGUID();
                            Switch(c, self, Behavior::Vending,
                                helpless ? "бить нечем, иду чиниться"
                                         : stuffed ? "сумки полны, иду продавать"
                                                   : clutter ? "хлам в сумках, иду продавать"
                                                             : "торговец рядом, чинюсь мимоходом");
                            return;
                        }
                        // НИКОГО В ОБЗОРЕ — ИДЁМ ПО КАРТЕ, если поход того стоит: ради
                        // сломанного оружия или полных сумок, не ради потёртости. Пятеро
                        // людей стояли так часами: сломаны, ремонтник за 150-300 ярдов.
                        if ((helpless || stuffed || clutterFar) && FindMenderByMap(c, self, stuffed || clutterFar, helpless, &c.VendorEntry, &c.VendorPos))
                        {
                            c.VendorGuid.Clear();
                            c.VendorScanMs = 0;
                            c.VendorDist = self->GetExactDist2d(c.VendorPos.GetPositionX(), c.VendorPos.GetPositionY());
                            TC_LOG_INFO("server.worldserver",
                                "Constellation ТОРГ {}: в обзоре никого — иду к {} за {:.0f} ярдов ({})",
                                self->GetName(), c.VendorEntry, c.VendorDist, helpless ? "чиниться" : "продавать");
                            Switch(c, self, Behavior::Vending,
                                helpless ? "бить нечем, иду чиниться по карте"
                                         : stuffed ? "сумки полны, иду продавать по карте"
                                                   : "хлам в сумках, иду продавать по карте");
                            return;
                        }
                        // и по карте никого: считаем и молчим пять минут
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
                // БИТЬ НЕКОГО, НО МОЖЕТ БЫТЬ ЕСТЬ С КЕМ ПОГОВОРИТЬ.
                //
                // Кандидата отметил поиск боевой цели: цель задания, которую ядро не
                // считает атакуемой и у которой есть беседа. Выше боя намеренно не ставим —
                // если по заданию надо кого-то убить, это полезнее; но если убить некого,
                // разговор закрывает цель, которую иначе не закрыть ничем.
                if (!c.TalkCandidate.IsEmpty())
                {
                    if (Creature* who = ObjectAccessor::GetCreature(*self, c.TalkCandidate))
                    {
                        c.TalkGuid = c.TalkCandidate;
                        c.TalkCandidate.Clear();
                        c.TalkMs = 0;
                        c.TalkDist = self->GetExactDist(who);
                        Switch(c, self, Behavior::Talking, "надо поговорить, а не драться");
                        return;
                    }
                    c.TalkCandidate.Clear();
                }

                // БИТЬ НЕКОГО — МОЖЕТ, НУЖНОЕ ПРОСТО ЛЕЖИТ НА ЗЕМЛЕ.
                //
                // Ниже боя намеренно: если задание закрывается убийством, драться полезнее —
                // это и опыт, и добыча. Сбор идёт, когда драться не за что.
                // под тем же флагом, что и сдача: это квестовая работа, а не добыча ради
                // добычи (Кодекс — сдача под Cfg().Quests, бой под Cfg().Fight, а сбор был
                // не под чем)
                if (idleScan && Cfg().Quests && FindGatherCandidate(c, self))
                {
                    c.GatherMs = 0;
                    c.GatherDist = self->GetExactDist(c.GatherPos);
                    Switch(c, self, Behavior::Gathering, "нужное лежит на земле");
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
                        // ДАЛЕКО — СНАЧАЛА СМОТРИМ, НЕ БЫСТРЕЕ ЛИ ПО ВОЗДУХУ (оператор: полётные
                        // мастера не опрашиваются и никто не летает).
                        if (Cfg().Flying && HearthTowards(c, self, c.TravelPos))
                            return;             // камень уносит домой, дорога продолжится оттуда
                        if (Cfg().Flying && PlanFlight(c, self, c.TravelPos))
                        {
                            Switch(c, self, Behavior::TakingFlight, "лечу к месту задания");
                            return;
                        }
                        Switch(c, self, Behavior::Travelling, "иду за целью задания");
                        return;
                    }
                    c.TravelScanMs = 2000;  // впустую — не перебирать точки каждый такт
                }
                // ДЕЛАТЬ НЕЧЕГО — СКАЗАТЬ ПОЧЕМУ, И ПОЙТИ ЗА КВЕСТОМ ПО КАРТЕ.
                //
                // Сюда доходит тот, у кого нет ни готового к сдаче, ни цели, ни собеседника, ни
                // точки сбора, ни дороги к цели. Замер 2026-09-02: таких 33 из 122, и все стояли
                // молча. Прибор — раз в пять минут; поход — к ближайшему квестодателю карты, у
                // которого ядро дало бы квест по светофору (в обзоре таких уже нет).
                if (Cfg().Quests && Cfg().TakeQuests && idleScan)
                {
                    uint32 unmetNow = 0;
                    std::set<uint32> wantedNow;
                    WantedEntries(self, wantedNow, nullptr, nullptr, nullptr, &unmetNow);
                    if (!unmetNow && c.TalkCandidate.IsEmpty())
                    {
                        if (!c.IdleDiagMs)
                        {
                            c.IdleDiagMs = 300000;
                            LogIdle(c, self);
                        }
                        if (!c.SeekCooldownMs && Cfg().GiverSeekRange > 0.0f)
                        {
                            uint32 qid = 0;
                            if (FindGiverByMap(c, self, &c.SeekEntry, &c.SeekSpawn, &c.SeekPos, &qid))
                            {
                                if (Cfg().Flying && PlanFlight(c, self, c.SeekPos))
                                {
                                    Switch(c, self, Behavior::TakingFlight, "лечу к квестодателю");
                                    return;
                                }
                                Quest const* q = sObjectMgr->GetQuestTemplate(qid);
                                CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(c.SeekEntry);
                                TC_LOG_INFO("server.worldserver",
                                    "Constellation ПОХОД {}: в обзоре никто ничего не предлагает — иду к {} ({}) за {:.0f} ярдов: у него {} '{}'",
                                    self->GetName(), ct ? ct->Name : std::string("?"), c.SeekEntry,
                                    self->GetExactDist2d(c.SeekPos.GetPositionX(), c.SeekPos.GetPositionY()),
                                    qid, q ? q->GetLogTitle() : std::string(""));
                                Switch(c, self, Behavior::SeekingGiver, "иду к квестодателю по карте");
                                return;
                            }
                            c.SeekCooldownMs = 300000;      // никого подходящего — не перебирать карту каждые пять секунд
                        }
                    }
                }
                // ОЧЕРЕДИ ПО РАССТОЯНИЮ ЗДЕСЬ БОЛЬШЕ НЕТ (оператор, 2026-09-02: «если то
                // что ты писал про расстояние — откати, теперь есть светофор»).
                //
                // Она была лечением симптома: Бриенна с готовым «Hero's Call: Westfall!»
                // шла сдавать его в Западный край первым делом и гибла по дороге. Причина
                // же не в расстоянии, а в том, что такое задание вообще не по ней — и это
                // теперь решается при ВЗЯТИИ, цветом. Сдаём готовое сразу, как игрок;
                // от бесконечных походов остались бюджет по прогрессу и отсрочка при гибели.
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
                Creature* vendor = c.VendorGuid.IsEmpty() ? nullptr : ObjectAccessor::GetCreature(*self, c.VendorGuid);
                if (vendor && !vendor->IsAlive())
                    vendor = nullptr;
                // ПО КАРТЕ: торговца ещё не видно — идём к его точке, у точки ищем в обзоре.
                // Тот же порядок, что у принимающего квест: точка из указателя, обзор в
                // сорок ярдов раз в две секунды, срок от расстояния.
                if (!vendor && c.VendorEntry)
                {
                    if (c.VendorScanMs)
                        c.VendorScanMs = (c.VendorScanMs <= slice) ? 0 : c.VendorScanMs - slice;
                    float const d = self->GetExactDist2d(c.VendorPos.GetPositionX(), c.VendorPos.GetPositionY());
                    if (!c.VendorScanMs && d < 60.0f)
                    {
                        c.VendorScanMs = 2000;
                        std::list<Creature*> near;
                        Trinity::AnyUnitInObjectRangeCheck check(self, 40.0f);
                        Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, near, check);
                        Cell::VisitGridObjects(self, searcher, 40.0f);
                        float best = 100000.0f;
                        for (Creature* cr : near)
                        {
                            if (cr->GetEntry() != c.VendorEntry || !cr->IsAlive())
                                continue;
                            float const d3 = self->GetExactDist(cr);
                            if (d3 < best)
                                { best = d3; vendor = cr; }
                        }
                        if (vendor)
                        {
                            c.VendorGuid = vendor->GetGUID();
                            c.VendorEntry = 0;
                        }
                    }
                    if (!vendor)
                    {
                        if (d > 6.0f)
                        {
                            StepToward(c, self, c.VendorPos.GetPositionX(), c.VendorPos.GetPositionY(),
                                c.VendorPos.GetPositionZ(), 5.0f, dt, &c.VendorPos);
                            // бюджет по прогрессу, как у сдачи: полминуты без приближения — конец
                            if (d < c.WalkBest - 1.0f)
                                { c.WalkBest = d; c.WalkStuckMs = 0; }
                            else
                                c.WalkStuckMs += slice;
                            if (c.Stalled || c.WalkStuckMs > 30000 || c.ModeMs > Cfg().WalkCapMs)
                            {
                                c.VendCooldownMs = 300000;
                                c.VendorEntry = 0;
                                Switch(c, self, Behavior::Idle, "до торговца по карте не дойти");
                            }
                        }
                        else
                        {
                            // пришли на точку, а его нет: не та фаза, не возродился, ушёл маршрутом
                            c.VendCooldownMs = 300000;
                            c.VendorEntry = 0;
                            Switch(c, self, Behavior::Idle, "торговца на его точке нет");
                        }
                        return;
                    }
                }
                if (!vendor)
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
                    bool const vendorDone = c.Stalled || c.ModeMs > 120000;
                    if (vendorDone && FindReachableApproach(c, self, vendor))
                        { c.ModeMs = 0; return; }       // нашли обход — даём дойти до него
                    if (vendorDone)
                    {
                        LogApproachFailure(c, self, vendor, "торговцу");
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

            // ПОХОД К КВЕСТОДАТЕЛЮ ПО КАРТЕ — та же форма, что у торговца по карте: точка из
            // указателя, бюджет по прогрессу, тупик. Дошли в обзор — дальше обычный порядок:
            // Hello, меню ядра, светофор. Вид на десять минут в отсрочку, чтобы не выбрать его
            // же снова, если у самого квестодателя окажется нечего брать.
            case Behavior::TakingFlight:
            {
                // УЖЕ ЛЕТИМ — НЕ МЕШАЕМ ЯДРУ. Оно ведёт персонажа само; наша работа кончилась.
                if (self->IsInFlight())
                    return;

                Creature* master = c.FlightMaster.IsEmpty() ? nullptr
                                 : ObjectAccessor::GetCreature(*self, c.FlightMaster);
                if (master && !master->IsAlive())
                    master = nullptr;
                if (!master)
                {
                    c.FlightMaster.Clear();
                    float const d = self->GetExactDist2d(c.FlightMasterPos.GetPositionX(), c.FlightMasterPos.GetPositionY());
                    if (d > 20.0f)
                    {
                        StepToward(c, self, c.FlightMasterPos.GetPositionX(), c.FlightMasterPos.GetPositionY(),
                            c.FlightMasterPos.GetPositionZ(), 15.0f, dt, &c.FlightMasterPos);
                        if (d < c.WalkBest - 1.0f)
                            { c.WalkBest = d; c.WalkStuckMs = 0; }
                        else
                            c.WalkStuckMs += slice;
                        if (c.Stalled || c.WalkStuckMs > 30000 || c.ModeMs > Cfg().WalkCapMs)
                        {
                            TC_LOG_INFO("server.worldserver",
                                "Constellation ПОЛЁТ {}: до полётного мастера не дойти — осталось {:.0f} ярдов",
                                self->GetName(), d);
                            c.FlightCooldownMs = 600000;
                            Switch(c, self, Behavior::Idle, "до полётного мастера не дойти");
                        }
                        return;
                    }
                    // пришли на точку — ищем его вживую
                    std::list<Creature*> near;
                    Trinity::AnyUnitInObjectRangeCheck check(self, 30.0f);
                    Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, near, check);
                    Cell::VisitGridObjects(self, searcher, 30.0f);
                    float best = 100000.0f;
                    for (Creature* cr : near)
                        if (cr->IsAlive() && cr->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER)
                            && cr->GetEntry() == c.FlightMasterEntry)   // ТОТ ЖЕ, для кого считали (Кодекс)
                        {
                            float const dd = self->GetExactDist(cr);
                            if (dd < best)
                                { best = dd; master = cr; }
                        }
                    if (!master)
                    {
                        c.FlightCooldownMs = 600000;
                        Switch(c, self, Behavior::Idle, "полётного мастера на точке нет");
                        return;
                    }
                    c.FlightMaster = master->GetGUID();
                }

                // ЯДРО РЕШАЕТ, ДОСТАТОЧНО ЛИ БЛИЗКО — тем же вопросом, что задаёт обработчик.
                if (!self->GetNPCIfCanInteractWith(c.FlightMaster, UNIT_NPC_FLAG_FLIGHTMASTER, UNIT_NPC_FLAG_2_NONE))
                {
                    float ax, ay, az;
                    ApproachPoint(c, master, self, ax, ay, az, diff);
                    StepToward(c, self, ax, ay, az, master->GetCombatReach() + 2.0f, dt);
                    bool const done = c.Stalled || c.ModeMs > 120000;
                    if (done && FindReachableApproach(c, self, master))
                        { c.ModeMs = 0; return; }
                    if (done)
                    {
                        LogApproachFailure(c, self, master, "полётному мастеру");
                        c.FlightCooldownMs = 600000;
                        Switch(c, self, Behavior::Idle, "к полётному мастеру не подойти");
                    }
                    return;
                }

                // УЗЕЛ ВЫВОДИМ ЗАНОВО, ИЗ ЖИВОЙ ПОЗИЦИИ (Кодекс). Обработчик берёт его от того, кто
                // стоит перед нами, а план считал по строке таблицы: мастер мог сдвинуться, а
                // рядом стоять другой. Разошлось — перепроверяем маршрут, и только потом летим.
                uint32 const liveFrom = sObjectMgr->GetNearestTaxiNode(master->GetPositionX(), master->GetPositionY(),
                                                                       master->GetPositionZ(), master->GetMapId(), self->GetTeam());
                if (!liveFrom || !self->m_taxi.IsTaximaskNodeKnown(liveFrom))
                {
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ПОЛЁТ {}: у живого мастера узел {} (ждали {}) — не наш, не лечу",
                        self->GetName(), liveFrom, c.FlightFromNode);
                    c.FlightCooldownMs = 600000;
                    Switch(c, self, Behavior::Idle, "узел мастера не наш");
                    return;
                }
                if (liveFrom != c.FlightFromNode)
                {
                    std::vector<uint32> again;
                    TaxiNodesEntry const* from = sTaxiNodesStore.LookupEntry(liveFrom);
                    TaxiNodesEntry const* to = sTaxiNodesStore.LookupEntry(c.FlightNode);
                    if (!from || !to || TaxiPathGraph::GetCompleteNodeRoute(from, to, self, again) < 2)
                    {
                        TC_LOG_INFO("server.worldserver",
                            "Constellation ПОЛЁТ {}: живой узел {} не тот, что в плане ({}), и маршрута к {} от него нет",
                            self->GetName(), liveFrom, c.FlightFromNode, c.FlightNode);
                        c.FlightCooldownMs = 600000;
                        Switch(c, self, Behavior::Idle, "маршрута от живого узла нет");
                        return;
                    }
                    c.FlightFromNode = liveFrom;
                }

                // НАЗЕМНОЕ ДВИЖЕНИЕ КОНЧАЕТСЯ ЗДЕСЬ (Кодекс): после взлёта любой наш пакет
                // движения спорил бы с маршрутом, который ведёт ядро.
                StopMoving(c, self);
                c.Waypoints.clear();
                c.WaypointIndex = 0;
                c.Moving = false;

                // ТОТ ЖЕ ПАКЕТ, ЧТО ШЛЁТ КЛИЕНТ. Свой узел ядро выведет из позиции мастера само.
                WorldPacket raw(CMSG_ACTIVATE_TAXI);
                WorldPackets::Taxi::ActivateTaxi taxi(std::move(raw));
                taxi.Vendor = c.FlightMaster;
                taxi.Node = c.FlightNode;
                uint64 const moneyWas = self->GetMoney();
                c.Session->HandleActivateTaxiOpcode(taxi);

                // УСПЕХ — СОСТОЯНИЕ, А НЕ ОТПРАВКА: сокета нет, ответа не будет.
                if (self->IsInFlight())
                {
                    ++_flights;
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ПОЛЁТ {}: полетел к узлу {}, заплатил {} мед., экономия пешком {:.0f} ярдов; всего перелётов {}",
                        self->GetName(), c.FlightNode, uint32(moneyWas - self->GetMoney()),
                        c.FlightSavedYards, _flights);
                    c.FlightCooldownMs = 60000;
                    Switch(c, self, Behavior::Idle, "лечу");
                    return;
                }
                TC_LOG_INFO("server.worldserver",
                    "Constellation ПОЛЁТ {}: ядро не отправило к узлу {} (денег {}, узел наш {})",
                    self->GetName(), c.FlightNode, moneyWas,
                    self->m_taxi.IsTaximaskNodeKnown(c.FlightNode) ? 1 : 0);
                c.FlightCooldownMs = 600000;
                Switch(c, self, Behavior::Idle, "улететь не вышло");
                return;
            }

            case Behavior::SeekingGiver:
            {
                float const d = self->GetExactDist2d(c.SeekPos.GetPositionX(), c.SeekPos.GetPositionY());
                if (d <= 25.0f)
                {
                    c.SeekBackoff[c.SeekSpawn] = 600000;
                    // СРОК СТАВИТСЯ И ПРИ УДАЧНОМ ПРИХОДЕ (Кодекс): иначе, если у пришедшего
                    // нечего взять, следующий поход начинался бы тут же к соседней точке —
                    // цепочка походов вместо дела.
                    c.SeekCooldownMs = 300000 + (c.Guid.GetCounter() % 61) * 1000;
                    c.QuestMs = Cfg().QuestIntervalMs;      // спросить квестодателя на следующем же такте
                    c.IdleDiagMs = 0;                       // и прибор ПРОСТОЙ — сразу по приходу, не через 5 мин
                    // ТОЧКА СПАВНА — НЕ МЕСТО NPC (замер: пришли в 25 ярдов к точке, а Chip Endale
                    // стоит в 35 — за радиусом отбора). Ищем предлагающего в 60 ярдах и отдаём его
                    // подходу; дальше обычный путь: подойти, Hello, меню, взять.
                    if (Creature* found = NearestQuestGiver(c, self, 60.0f))
                    {
                        c.GiverGuid = found->GetGUID();
                        c.GiverMs = 0;
                        c.GiverDist = self->GetExactDist(found);
                        c.GiverRange = 60.0f;               // порог сброса в QuestTick считает от него
                        c.TalkDiagDone = false;             // и прибор РАЗГОВОР — снова, у этой цели
                        TC_LOG_INFO("server.worldserver", "Constellation ПОХОД {}: у точки {} ({}) стоит в {:.0f} ярдах — иду к нему",
                            self->GetName(), found->GetName(), found->GetEntry(), c.GiverDist);
                    }
                    Switch(c, self, Behavior::Idle, "дошёл до квестодателя по карте");
                    return;
                }
                StepToward(c, self, c.SeekPos.GetPositionX(), c.SeekPos.GetPositionY(),
                    c.SeekPos.GetPositionZ(), 20.0f, dt, &c.SeekPos);
                if (d < c.WalkBest - 1.0f)
                    { c.WalkBest = d; c.WalkStuckMs = 0; }
                else
                    c.WalkStuckMs += slice;
                if (c.Stalled || c.WalkStuckMs > 30000 || c.ModeMs > Cfg().WalkCapMs)
                {
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ПОХОД {}: до квестодателя {} по карте не дойти — осталось {:.0f} ярдов, тупик {}, без прогресса {} с, всего {} с",
                        self->GetName(), c.SeekEntry, d, c.Stalled ? 1 : 0, c.WalkStuckMs / 1000, c.ModeMs / 1000);
                    c.SeekBackoff[c.SeekSpawn] = 600000;
                    c.SeekCooldownMs = 300000;
                    Switch(c, self, Behavior::Idle, "до квестодателя по карте не дойти");
                }
                return;
            }

            case Behavior::Travelling:
            {
                TouchAreaTriggers(c, self);
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
                if (self->GetExactDist2d(c.TravelPos.GetPositionX(), c.TravelPos.GetPositionY()) <= c.TravelStop + 2.0f)
                {
                    c.TravelBackoff[c.TravelQuest] = 600000;
                    Switch(c, self, Behavior::Idle, "пришёл — целей нет, отложил квест");
                    return;
                }
                StepToward(c, self, c.TravelPos.GetPositionX(), c.TravelPos.GetPositionY(),
                    c.TravelPos.GetPositionZ(), c.TravelStop, dt, &c.TravelPos);
                // СРОК щедрый: до цели бывает и двести ярдов, а бежим мы шагами по 4 Гц
                // ПРОДВИЖЕНИЕ, А НЕ ЧАСЫ (оператор: цепочка ведёт в соседнюю зону, и это норма).
                // Прежние три минуты означали потолок примерно в тысячу двести ярдов — а метка
                // следующего хаба бывает и дальше. Судим по тому же, по чему судит поход к
                // торговцу: приближаемся ли. Часы остаются страховкой от вечного цикла.
                float const dTravel = self->GetExactDist2d(c.TravelPos.GetPositionX(), c.TravelPos.GetPositionY());
                if (dTravel < c.WalkBest - 1.0f)
                    { c.WalkBest = dTravel; c.WalkStuckMs = 0; }
                else
                    c.WalkStuckMs += slice;
                if (c.Stalled || c.WalkStuckMs > 30000 || c.ModeMs > Cfg().WalkCapMs)
                {
                    c.TravelCooldownMs = 120000;
                    Switch(c, self, Behavior::Idle,
                        c.Stalled ? "до места задания не дойти"
                                  : (c.WalkStuckMs > 30000 ? "полминуты без приближения к месту задания"
                                                           : "в пути слишком долго"));
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

                // ОТВОД: УТАЩИТЬ ЦЕЛЬ ОТ ЛАГЕРЯ И ТАМ ДОБИТЬ.
                //
                // Начинаем, только если при выборе у цели были соседи: тащить одиночку незачем.
                // Точку берём один раз — прочь от того места, где мы её зацепили, на расчётное
                // расстояние; идём спиной, лицом к цели, поэтому удары не прерываются.
                if (Cfg().KiteYards > 0.0f && c.EngageAssists > 0 && self->IsInCombat()
                    && !c.Kiting && c.KiteMs == 0)
                {
                    // ПРОЧЬ ОТ ЦЕНТРА ПАЧКИ, А НЕ ПРОСТО ОТ ЦЕЛИ (разбор), и точку обязан
                    // одобрить ПОСТРОИТЕЛЬ МАРШРУТА: шаги по прямой протащили бы сквозь
                    // непроходимое. Идём потом по его же точкам, только спиной.
                    float const ang = c.PackCenterKnown
                        ? c.PackCenter.GetAbsoluteAngle(self->GetPositionX(), self->GetPositionY())
                        : self->GetAbsoluteAngle(target) + float(M_PI);
                    float const kx = self->GetPositionX() + std::cos(ang) * Cfg().KiteYards;
                    float const ky = self->GetPositionY() + std::sin(ang) * Cfg().KiteYards;
                    if (MapManager::IsValidMapCoord(self->GetMapId(), kx, ky))
                    {
                        float const kz = self->GetMap()->GetHeight(self->GetPhaseShift(), kx, ky,
                                                                   self->GetPositionZ() + 3.0f);
                        PathGenerator back(self);
                        if (kz > INVALID_HEIGHT && std::fabs(kz - self->GetPositionZ()) < 12.0f
                            && back.CalculatePath(kx, ky, kz, false)
                            && !(back.GetPathType() & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE)))
                        {
                            c.KitePath.clear();
                            for (G3D::Vector3 const& v : back.GetPath())
                                c.KitePath.emplace_back(v.x, v.y, v.z);
                            c.KiteIdx = c.KitePath.size() > 1 ? 1 : 0;
                            c.KiteTo.Relocate(kx, ky, kz);
                            c.Kiting = !c.KitePath.empty();
                            c.KiteMs = 1;
                            c.WalkBest = 100000.0f;
                            if (c.Kiting)
                                TC_LOG_INFO("server.worldserver",
                                    "Constellation ОТВОД {}: у {} ({}) было {} соседей — увожу на {:.0f} ярдов, точек пути {}",
                                    self->GetName(), target->GetName(), target->GetEntry(),
                                    c.EngageAssists, Cfg().KiteYards, uint32(c.KitePath.size()));
                        }
                    }
                    if (!c.Kiting)
                        c.KiteMs = 0xFFFFFFFF;      // некуда пятиться — больше не пробуем в этом бою
                }
                if (c.Kiting)
                {
                    c.KiteMs += uint32(dt * 1000.0f);
                    // ЗАСТРЕВАНИЕ ВО ВРЕМЯ ОТВОДА СУДИМ ТЕМ ЖЕ, ЧЕМ И ОБЫЧНЫЙ ШАГ (разбор):
                    // сдвинулись ли мы на самом деле.
                    if (self->GetExactDist2d(c.LastX, c.LastY) > 1.0f)
                    {
                        c.LastX = self->GetPositionX();
                        c.LastY = self->GetPositionY();
                        c.StuckMs = 0;
                    }
                    else
                        c.StuckMs += uint32(dt * 1000.0f);

                    float const left = self->GetExactDist2d(c.KiteTo.GetPositionX(), c.KiteTo.GetPositionY());
                    // ДОШЛИ, ЗАСТРЯЛИ ИЛИ ЗАТЯНУЛОСЬ — ДЕРЁМСЯ ЗДЕСЬ. И ОБЯЗАТЕЛЬНО ОСТАНАВЛИВАЕМСЯ
                    // (разбор): иначе сервер продолжает видеть «иду назад».
                    if (left <= 2.0f || c.KiteMs > 12000 || c.Stalled || c.StuckMs > 3000
                        || c.KiteIdx >= c.KitePath.size())
                    {
                        c.Kiting = false;
                        StopMoving(c, self);
                        TC_LOG_INFO("server.worldserver",
                            "Constellation ОТВОД {}: отошёл, осталось {:.0f} ярдов ({}), дерусь здесь",
                            self->GetName(), left,
                            c.StuckMs > 3000 ? "застрял" : (c.KiteMs > 12000 ? "долго" : "дошёл"));
                    }
                    else
                    {
                        // ДАЛЬНОБОЙНЫЙ ЧИТАЕТ СТОЯ (оператор и разбор): несмгновенное заклинание во
                        // время движения не прочитается. Идёт каст — стоим и не мешаем себе; ядро
                        // само скажет, идёт ли он.
                        if (self->IsNonMeleeSpellCast(false))
                        {
                            StopMoving(c, self);
                            return;
                        }
                        // ШАГ ПО ТОЧКАМ ПОСТРОИТЕЛЯ, А НЕ ПО ПРЯМОЙ (разбор), но спиной — лицом к
                        // цели, иначе ядро отвергнет каждый замах.
                        Position const& wp = c.KitePath[c.KiteIdx];
                        if (self->GetExactDist2d(wp.GetPositionX(), wp.GetPositionY()) < 1.5f)
                            ++c.KiteIdx;
                        else
                            StepBackFacing(c, self, wp, target, dt);
                        if (closeEnough)
                            TryAttack(c, self, target);
                        return;
                    }
                }
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
                    bool const enderDone = c.Stalled || c.ModeMs > 60000;
                    if (enderDone && FindReachableApproach(c, self, ender))
                        { c.ModeMs = 0; return; }
                    if (enderDone)
                    {
                        LogApproachFailure(c, self, ender, "принимающему");
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
                        c.TurnInPos.GetPositionZ(), 5.0f, dt,
                        c.TurnInPosFromTable ? &c.TurnInPos : nullptr);
                    // БЮДЖЕТ ПО ПРОГРЕССУ, А НЕ ПО ПРЯМОЙ (Кодекс, и журнал Легиона 0006:640):
                    // прямая — не длина маршрута, и срок «от расстояния» там уже откатывали.
                    // Мера — приближаемся ли: полминуты без нового лучшего расстояния —
                    // дороги нет; и общий потолок четыре минуты. Плоская минута не давала
                    // дойти дальше 250 ярдов, срок от прямой обманул бы на извилистом пути.
                    if (d < c.WalkBest - 1.0f)
                        { c.WalkBest = d; c.WalkStuckMs = 0; }
                    else
                        c.WalkStuckMs += slice;
                    if (c.Stalled || c.WalkStuckMs > 30000 || c.ModeMs > Cfg().WalkCapMs)
                    {
                        c.TurnInBackoff[c.TurnInQuest] = 300000;   // пять минут: дорога, а не рывок
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

            // СБОР С ОБЪЕКТА НА ЗЕМЛЕ — фаза 5 плана `constellation-liveness.md`.
            //
            // Форма ровно та же, что у похода к принимающему: идём, границы дороги (нет
            // продвижения за 20 с / всего 30 с), не дошли — в чёрный список и заняться
            // другим. Дистанцию взаимодействия НЕ ВЫДУМЫВАЕМ: ядро само отвечает
            // GetGameObjectIfCanInteractWith, и оно же решает про полёт, значок и состояние.
            //
            // Открыв объект, добычу забирает ОБЩАЯ процедура TakeOpenLoot — та же, что у
            // трупа: Use() наполняет тот же m_AELootView.
            case Behavior::Gathering:
            {
                // ИДЁМ ПО СЧИСЛЕНИЮ, ПРАВДУ СПРАШИВАЕМ ПО ПРИХОДУ.
                //
                // Пока мы в пути, мир не опрашивается вовсе: место известно из указателя.
                // У самой точки берём живой объект ПО ИДЕНТИФИКАТОРУ СПАВНА — прямое
                // обращение к карте вместо обхода сетки (Кодекс: Map::GetGameObjectBySpawnId).
                float const near = self->GetExactDist(c.GatherPos);
                GameObject* go = near <= 12.0f
                    ? self->GetMap()->GetGameObjectBySpawnId(c.GatherSpawnId) : nullptr;

                if (!go || !go->isSpawned() || !self->GetGameObjectIfCanInteractWith(go->GetGUID()))
                {
                    // ещё не дошли — идём; дошли, а его нет — отсрочка с причиной
                    if (near > 4.0f)
                    {
                        StepToward(c, self, c.GatherPos.GetPositionX(), c.GatherPos.GetPositionY(),
                                   c.GatherPos.GetPositionZ(), 0.25f, dt, &c.GatherPos);

                        // МАРШРУТА НЕТ — БЕРЁМ ДРУГУЮ ТОЧКУ, А НЕ ЖДЁМ.
                        //
                        // Здесь сталкивались два моих же отсчёта. При отказе построителя шаг
                        // уходит в отступание на 3-24 секунды и молча стоит, а сбор отсчитывает
                        // свои 20 секунд «без продвижения» и сдаётся. Замер это и показал: 127
                        // отказов из 181 захода, и в них «было 56, стало 56» — расстояние не
                        // менялось ВООБЩЕ, то есть спутник не шёл, а ждал по одному правилу,
                        // пока его судили по другому.
                        //
                        // Точек в указателе 13 281. Если к этой дороги нет — это повод взять
                        // следующую, а не стоять двадцать секунд у той же.
                        if (c.NoPathMs > 0)
                        {
                            TC_LOG_INFO("server.worldserver",
                                "Constellation СБОР {}: к точке {} (вид {}) маршрута нет, беру другую",
                                self->GetName(), c.GatherSpawnId, c.GatherEntry);
                            c.GatherBackoff[c.GatherSpawnId] = 300000;
                            c.GatherSpawnId = 0;
                            Switch(c, self, Behavior::Idle, "к объекту нет дороги");
                            return;
                        }
                        c.GatherMs += slice;
                        bool const noProgress = c.GatherMs >= 20000 && near > c.GatherDist - 1.0f;
                        if (c.Stalled || noProgress || c.GatherMs >= 45000)
                        {
                            TC_LOG_INFO("server.worldserver",
                                "Constellation СБОР {}: до точки {} (вид {}) не дойти за {} с, было {:.0f}, стало {:.0f}",
                                self->GetName(), c.GatherSpawnId, c.GatherEntry,
                                c.GatherMs / 1000, c.GatherDist, near);
                            c.GatherBackoff[c.GatherSpawnId] = 300000;   // не дойти — надолго
                            c.GatherSpawnId = 0;
                            Switch(c, self, Behavior::Idle, "до объекта не дойти");
                        }
                        return;
                    }

                    // ПРИШЛИ, А ОБЪЕКТА НЕТ. Это не ошибка и не навсегда: в пуле живёт один
                    // член из многих, событие может быть выключено, а взятый объект
                    // возрождается по своему сроку (Кодекс перечислил все эти случаи).
                    TC_LOG_INFO("server.worldserver",
                        "Constellation СБОР {}: пришёл к точке {} (вид {}), а объекта там нет",
                        self->GetName(), c.GatherSpawnId, c.GatherEntry);
                    c.GatherBackoff[c.GatherSpawnId] = 120000;
                    c.GatherSpawnId = 0;
                    Switch(c, self, Behavior::Idle, "объекта на месте нет");
                    return;
                }

                // ОТКРЫВАЕМ ТЕМ ЖЕ ОПКОДОМ, КАКИМ ЭТО ДЕЛАЕТ КЛИЕНТ ПО КЛИКУ.
                // Пакет несёт ровно один GUID (GameObjectPackets.h), обработчик зовёт
                // GetGameObjectIfCanInteractWith и затем obj->Use(player).
                std::string const name = go->GetName();
                uint32 const entry = go->GetEntry();

                // ДОСТУПНОСТЬ СУНДУКА ЗАВИСИТ ОТ САМОГО ИГРОКА, И ЭТО НАДО СПРОСИТЬ.
                //
                // Кодекс называл это среди того, чего строка в таблице не знает: для
                // сундуков и точек сбора ядро проверяет квестовое состояние игрока через
                // GameObject::ActivateToQuest, и без этого добыча для него не создаётся
                // вовсе. Замер на боевом после переделки: 106 заходов, из них НИ ОДНОГО
                // с добычей — «открыл, но ничего не легло». Проверки не было.
                //
                // Здесь же — права на добычу: IsLootAllowedFor знает про уже опустошённое,
                // разрешённых собирателей, личную добычу и список захвативших.
                if (!go->ActivateToQuest(self))
                {
                    TC_LOG_INFO("server.worldserver",
                        "Constellation СБОР {}: {} ({}) для меня не активен по квесту",
                        self->GetName(), name, entry);
                    c.GatherBackoff[c.GatherSpawnId] = 300000;
                    c.GatherSpawnId = 0;
                    Switch(c, self, Behavior::Idle, "объект не мой по квесту");
                    return;
                }
                if (!go->IsLootAllowedFor(self))
                {
                    TC_LOG_INFO("server.worldserver",
                        "Constellation СБОР {}: {} ({}) уже занят или обобран другим",
                        self->GetName(), name, entry);
                    c.GatherBackoff[c.GatherSpawnId] = 120000;
                    c.GatherSpawnId = 0;
                    Switch(c, self, Behavior::Idle, "добыча не наша");
                    return;
                }
                uint32 const spaceBefore = FreeBagSpace(self);
                {
                    WorldPacket raw(CMSG_GAME_OBJ_USE);
                    WorldPackets::GameObject::GameObjUse use(std::move(raw));
                    use.Guid = go->GetGUID();
                    c.Session->HandleGameObjectUseOpcode(use);
                }

                // Считаем ЛЁГШЕЕ, а не запрошенное: предмет, ушедший в имеющуюся стопку,
                // места не занимает, и по свободным ячейкам удачный сбор выглядел бы пустым.
                // ЧАСТЬ СУНДУКОВ КЛАДЁТ ДОБЫЧУ СРАЗУ, БЕЗ ОКНА (Кодекс).
                //
                // При флаге chestPushLoot ядро складывает предметы прямо внутри Use, и
                // окна добычи не появляется вовсе. Прежний счёт считал такой заход
                // неудачей — вид пуст, значит «ничего не легло», — и ставил объекту
                // отсрочку, хотя предмет реально лежал в сумке. Поэтому меряем по месту
                // в сумках вокруг самого Use, а не только по окну.
                uint32 const landed = self->GetAELootView().empty()
                    ? (spaceBefore > FreeBagSpace(self) ? spaceBefore - FreeBagSpace(self) : 0u)
                    : TakeOpenLoot(c, self, go->GetGUID(), name, entry);
                if (landed)
                {
                    ++c.Gathered;
                    TC_LOG_INFO("server.worldserver",
                        "Constellation СБОР {}: обобрал {} ({}), предметов легло {}; всего объектов {}",
                        self->GetName(), name, entry, landed, c.Gathered);
                    c.GatherBackoff[c.GatherSpawnId] = 60000;   // взят — вернёмся не сразу
                }
                else
                {
                    TC_LOG_INFO("server.worldserver",
                        "Constellation СБОР {}: открыл {} ({}), но ничего не легло",
                        self->GetName(), name, entry);
                    c.GatherBackoff[c.GatherSpawnId] = 120000;
                }
                c.GatherSpawnId = 0;
                Switch(c, self, Behavior::Idle, "собрал");
                return;
            }
            // РАЗГОВОР КАК СПОСОБ ЗАКРЫТЬ ЦЕЛЬ ЗАДАНИЯ.
            //
            // Такие цели записаны в базе типом «существо» — тем же, что «убить N волков», —
            // и отличаются только описанием «Speak with ...», которого модуль не читает.
            // Поэтому шесть спутников нежити ходили УБИВАТЬ Лилиан Восс и двух её товарищей
            // и стояли кучей на кладбище. Признак берём у ядра: цель задания, которую
            // IsValidAttackTarget отвергает и у которой есть флаг беседы.
            //
            // Зачёт выдаёт ВЫБОР ПУНКТА, а не сам подход: у всех троих правило
            // «выбран пункт меню -> применить заклинание на говорящего». Поэтому здесь
            // ровно то, что делает клиент: подойти, открыть беседу, перебрать пункты.
            case Behavior::Talking:
            {
                Creature* who = ObjectAccessor::GetCreature(*self, c.TalkGuid);
                if (!who || !who->IsAlive())
                {
                    c.TalkGuid.Clear();
                    Switch(c, self, Behavior::Idle, "собеседник исчез");
                    return;
                }

                // КАК ЗАКРЫВАЕТСЯ ЦЕЛЬ — ГОВОРЯТ ДАННЫЕ СУЩЕСТВА, И ПО НИМ ЖЕ МЕРЯЕТСЯ «ДОШЁЛ».
                //
                // Замер 2026-09-02: четверо людей 4-го уровня 36 раз подряд «иду
                // взаимодействовать -> стою (до собеседника не дойти)» у раненых пехотинцев
                // «Fear No Evil»: подходили на 7 ярдов и стояли 45 секунд. Приход мерился
                // вопросом ядра «можно ли говорить» — а у пехотинца нет ни беседы, ни
                // квестов: его правило добавляет ему флаг КЛИКА (SmartAI: ADD_NPC_FLAG
                // 0x1000000), и зачёт даёт заклинание клика (npc_spellclick_spells: 50047 ->
                // 93072 при взятом квесте). Значит «дошёл?» спрашивается по-разному:
                //   * беседа/квесты — у ядра, тем же вопросом, что задаёт обработчик;
                //   * клик — дистанция взаимодействия (INTERACTION_DISTANCE), как у клиента;
                //   * предмет — дальность ЕГО заклинания или радиус его области.
                bool const gossip = who->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP) || who->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER);
                bool const click = !gossip && who->HasNpcFlag(UNIT_NPC_FLAG_SPELLCLICK);
                uint32 clickCastMs = 0;
                bool clickTied = false;
                std::set<uint32> wantedNow;
                if (click)
                    WantedEntries(self, wantedNow);
                if (click && !ClickGivesCredit(who->GetEntry(), wantedNow, self, &clickCastMs, &clickTied))
                {
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ПРИМЕНЕНИЕ {}: клик по {} ({}) не даёт зачёта или небезопасен — не трогаю",
                        self->GetName(), who->GetName(), who->GetEntry());
                    c.TalkBackoff[who->GetEntry()] = 600000;
                    c.TalkGuid.Clear();
                    Switch(c, self, Behavior::Idle, "клик не по правилам");
                    return;
                }
                uint32 toolSpell = 0, toolQuest = 0;
                std::set<uint32> toolCredits;
                Item* tool = (!gossip && !click)
                    ? QuestToolFor(self, who->GetEntry(), &toolSpell, who, &toolQuest, &toolCredits) : nullptr;
                SpellInfo const* toolInfo = tool ? sSpellMgr->GetSpellInfo(toolSpell, DIFFICULTY_NONE) : nullptr;
                if (!gossip && !click && !toolInfo)
                {
                    // ЭТО СВОЙСТВО ОСОБИ, А НЕ ВИДА. Правило раненого пехотинца СНИМАЕТ с него
                    // флаг клика, как только его подняли: поднятый кем-то другим выглядит
                    // «незакрываемым», а рядом стоят семнадцать целых. Запрет по виду глушил
                    // весь квест на десять минут — так и вышло на живом у четверых людей.
                    // Отставляем особь; вид — только когда подряд не вышло с четырьмя.
                    c.TalkUnreachable.insert(c.TalkGuid);
                    if (c.ToolFruitlessEntry != who->GetEntry())
                        { c.ToolFruitlessEntry = who->GetEntry(); c.ToolGiveUps = 0; }
                    bool const wholeKind = ++c.ToolGiveUps >= 4;
                    if (wholeKind)
                    {
                        c.ToolGiveUps = 0;
                        c.TalkBackoff[who->GetEntry()] = 600000;
                    }
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ПРИМЕНЕНИЕ {}: у {} ({}) ни беседы, ни клика, ни предмета от квеста — отставляю {}",
                        self->GetName(), who->GetName(), who->GetEntry(), wholeKind ? "вид" : "особь");
                    c.TalkGuid.Clear();
                    c.ToolWaitMs = 0;
                    Switch(c, self, Behavior::Idle, "закрыть нечем");
                    return;
                }

                // ДИСТАНЦИЯ ПРИХОДА. У предмета с явной целью — дальность заклинания минус
                // запас; у предмета-области — часть её радиуса: «Spray Water» (80208) на
                // пожары виноградника бьёт по области с условием «цель — триггер пожара»
                // (conditions: 13/80208 -> 31/3/42940), а сам триггер невыбираем
                // (UNIT_FLAG_UNINTERACTIBLE) — клиент шлёт его без цели, стоя рядом.
                float reach = who->GetCombatReach() + 2.0f;
                bool toolUnit = false;
                if (toolInfo)
                {
                    toolUnit = toolInfo->NeedsExplicitUnitTarget();
                    if (toolUnit)
                        reach = std::clamp(toolInfo->GetMaxRange(false, self) - 2.0f, 3.0f, 25.0f);
                    else
                    {
                        float const radius = toolInfo->GetEffects().empty() ? 0.0f
                            : toolInfo->GetEffect(EFFECT_0).CalcRadius(self);
                        reach = radius > 1.0f ? std::clamp(radius * 0.6f, 2.0f, 15.0f) : 4.0f;
                    }
                }
                else if (click)
                    reach = INTERACTION_DISTANCE - 0.5f;

                // ДОШЁЛ — для клика и предмета ТОЧНОЕ расстояние до самой цели, без прибавки
                // радиусов (Кодекс: IsWithinDist3d прибавляет радиус игрока). И идём тогда
                // к САМОЙ цели, а не к точке подхода: та лежит в ~4.5 ярдах от цели, и
                // остановка «в reach от неё» оставляла бы до цели вдвое больше.
                bool const arrived = gossip
                    ? (self->CanInteractWithQuestGiver(who) || self->GetNPCIfCanInteractWith(
                          c.TalkGuid, UNIT_NPC_FLAG_GOSSIP, UNIT_NPC_FLAG_2_NONE) != nullptr)
                    : self->GetExactDist(who) <= reach;
                if (!arrived && !c.ToolWaitMs)
                {
                    float tx, ty, tz;
                    if (gossip)
                        ApproachPoint(c, who, self, tx, ty, tz, diff);
                    else
                    {
                        tx = who->GetPositionX();
                        ty = who->GetPositionY();
                        tz = who->GetPositionZ();
                    }
                    StepToward(c, self, tx, ty, tz, gossip ? reach : std::max(1.0f, reach - 1.0f), dt);

                    if (c.NoPathMs > 0)
                    {
                        TC_LOG_INFO("server.worldserver",
                            "Constellation РЕЧЬ {}: к {} ({}) маршрута нет",
                            self->GetName(), who->GetName(), who->GetEntry());
                        c.TalkUnreachable.insert(c.TalkGuid);
                        c.TalkGuid.Clear();
                        Switch(c, self, Behavior::Idle, "к собеседнику нет дороги");
                        return;
                    }
                    c.TalkMs += slice;
                    float const now = ProgressDist(c, self, who);
                    bool const noProgress = c.TalkMs >= 20000 && now > c.TalkDist - 1.0f;
                    bool const talkDone = c.Stalled || noProgress || c.TalkMs >= 45000;
                    if (talkDone && FindReachableApproach(c, self, who))
                        { c.TalkMs = 0; c.TalkDist = ProgressDist(c, self, who); return; }
                    if (talkDone)
                    {
                        LogApproachFailure(c, self, who, "собеседнику");
                        TC_LOG_INFO("server.worldserver",
                            "Constellation РЕЧЬ {}: до {} ({}) не дойти за {} с, было {:.0f}, стало {:.0f}",
                            self->GetName(), who->GetName(), who->GetEntry(),
                            c.TalkMs / 1000, c.TalkDist, now);
                        c.TalkUnreachable.insert(c.TalkGuid);
                        c.TalkGuid.Clear();
                        Switch(c, self, Behavior::Idle, "до собеседника не дойти");
                    }
                    return;
                }

                // ПРИШЛИ. Поворачиваемся — так делает игрок, и это видно в клиенте.
                self->SetFacingToObject(who);

                std::string const name = who->GetName();
                uint32 const entry = who->GetEntry();

                // ОКНО ОЖИДАНИЯ ЗАЧЁТА — общее для клика и предмета. Успех меряется ростом
                // счётчика цели, а не отсутствием ошибки; ждём до трёх секунд (время
                // произнесения плюс очередь ядра). Окно без зачёта — бесплодная попытка.
                if (c.ToolWaitMs)
                {
                    c.ToolWaitMs = (c.ToolWaitMs <= slice) ? 0 : c.ToolWaitMs - slice;
                    bool credited = false;
                    for (auto const& [key, before] : c.ToolWas)
                        if (self->GetQuestObjectiveData(key.first, key.second) > before)
                            { credited = true; break; }
                    if (credited)
                    {
                        ++c.Talked;
                        c.ToolFruitless = 0;
                        c.ToolGiveUps = 0;
                        c.ToolActionFruitless = 0;
                        c.ToolWaitMs = 0;
                        c.ToolWas.clear();
                        TC_LOG_INFO("server.worldserver",
                            "Constellation ПРИМЕНЕНИЕ {}: {} ({}) — зачёт; всего {}",
                            self->GetName(), name, entry, c.Talked);
                        c.TalkGuid.Clear();
                        Switch(c, self, Behavior::Idle, "закрыл цель");
                        return;
                    }
                    if (c.ToolWaitMs)
                        return;             // окно ещё идёт
                    // ПРЕДОХРАНИТЕЛЬ ИЗ ЛЕГИОНА, С ПОПРАВКОЙ КОДЕКСА: считаем окна без зачёта,
                    // не отправки. Два окна на одной особи — отставляем ЕЁ (пожар уже потушен,
                    // пехотинец уже поднят); четыре особи подряд — отставляем вид на две минуты.
                    // На Легионе шесть ботов сутки лупили дубинкой БОДРСТВУЮЩИХ батраков:
                    // 606 успешных применений, ноль продвижения.
                    if (++c.ToolFruitless >= 2)
                    {
                        c.ToolFruitless = 0;
                        c.TalkUnreachable.insert(c.TalkGuid);
                        if (c.ToolFruitlessEntry != entry)
                            { c.ToolFruitlessEntry = entry; c.ToolGiveUps = 0; }
                        if (++c.ToolGiveUps >= 4)
                        {
                            c.ToolGiveUps = 0;
                            c.TalkBackoff[entry] = 120000;
                            TC_LOG_INFO("server.worldserver",
                                "Constellation ПРИМЕНЕНИЕ {}: четыре особи {} ({}) без зачёта — отставляю вид",
                                self->GetName(), name, entry);
                        }
                        else
                            TC_LOG_INFO("server.worldserver",
                                "Constellation ПРИМЕНЕНИЕ {}: {} ({}) два окна без зачёта — отставляю особь",
                                self->GetName(), name, entry);
                        // ПРЕДОХРАНИТЕЛЬ НА ВСЁ ДЕЙСТВИЕ (Легион, 0012; Кодекс): шесть
                        // отставленных особей подряд по любым видам — пять минут без разговоров
                        // и применений вовсе, чтобы одна ошибочная связка не ходила по кругу.
                        if (++c.ToolActionFruitless >= 6)
                        {
                            c.ToolActionFruitless = 0;
                            c.ToolActionMs = 300000;
                            TC_LOG_INFO("server.worldserver",
                                "Constellation ПРИМЕНЕНИЕ {}: шесть особей подряд без зачёта — пять минут без взаимодействий",
                                self->GetName());
                        }
                        c.TalkGuid.Clear();
                        Switch(c, self, Behavior::Idle, "без зачёта");
                        return;
                    }
                    // первое окно без зачёта: остаёмся и пробуем ещё раз с этой же особью
                }

                // СТАРЫЙ СНИМОК СВЕРЯЕТСЯ ДО НОВОГО (Кодекс): рост после окна гасит предохранители,
                // но зачётом не считается — причина не доказана. Попытка идёт своим чередом.
                ReconcileLateCredit(c, self);

                if (click)
                {
                    SnapshotObjectives(self, entry, c.ToolWas);
                    c.ToolWasEntry = entry;
                    WorldPacket raw(CMSG_SPELL_CLICK);
                    WorldPackets::Spells::SpellClick sc(std::move(raw));
                    sc.SpellClickUnitGuid = c.TalkGuid;
                    sc.TryAutoDismount = false;
                    c.Session->HandleSpellClick(sc);
                    c.ToolWaitMs = std::max<uint32>(4000, clickCastMs + 2500);   // окно от времени произнесения
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ПРИМЕНЕНИЕ {}: клик по {} ({}) с {:.1f} ярдов, связь с зачётом {}",
                        self->GetName(), name, entry, self->GetExactDist(who), clickTied ? "прямая" : "через сценарий");
                    return;
                }

                if (toolInfo)
                {
                    // ПРЕДВАРИТЕЛЬНЫЕ УСЛОВИЯ вместо подсчёта отказов попытками (Кодекс):
                    // занят другим заклинанием, общий откат, откат предмета — ждём, не шлём.
                    // УСЛОВИЯ ЗАКЛИНАНИЯ ИЗ БАЗЫ — ТОТ ЖЕ ВОПРОС, ЧТО ЗАДАЁТ ЯДРО ПЕРЕД КАСТОМ.
                    //
                    // Замер: 79 применений ведра пробуждения по ленивым батракам и три зачёта.
                    // Условие лежит в данных: заклинание 19938 требует, чтобы на ЦЕЛИ висела
                    // аура сна 17743 (conditions 17/19938 -> тип 1, цель 1). Спящий её имеет,
                    // бодрствующий нет, и по бодрствующему каст просто не проходит. Ядро
                    // спрашивает это в Spell::CheckCast; спросим и мы — до отправки, а не
                    // после. Правило общее: любое условие на цель у любого квестового предмета.
                    {
                        ConditionSourceInfo cond(self, who);
                        if (!sConditionMgr->IsObjectMeetingNotGroupedConditions(
                                CONDITION_SOURCE_TYPE_SPELL, toolInfo->Id, cond))
                        {
                            if (!c.CondNoted)
                            {
                                c.CondNoted = true;
                                TC_LOG_INFO("server.worldserver",
                                    "Constellation ПРИМЕНЕНИЕ {}: {} ({}) не отвечает условиям заклинания {} — не трачу",
                                    self->GetName(), name, entry, toolSpell);
                            }
                            c.TalkRetry[c.TalkGuid] = 60000;    // условие временное — и запрет тоже
                            c.TalkGuid.Clear();
                            Switch(c, self, Behavior::Idle, "цель не по условиям");
                            return;
                        }
                    }
                    if (self->IsNonMeleeSpellCast(false) || self->GetSpellHistory()->HasGlobalCooldown(toolInfo)
                        || !self->GetSpellHistory()->IsReady(toolInfo, tool->GetEntry()))
                    {
                        c.TalkMs += slice;
                        if (c.TalkMs >= 60000)
                        {
                            c.TalkBackoff[entry] = 60000;
                            c.TalkGuid.Clear();
                            Switch(c, self, Behavior::Idle, "предмет не готов минуту");
                        }
                        return;
                    }
                    // СНИМОК — ПО ЗАСЧИТЫВАЕМЫМ ЦЕЛЯМ, А НЕ ПО НОМЕРУ СУЩЕСТВА: у раненого горца
                    // номер 37080, а зачёт идёт маркеру 37079 — снимок по 37080 был бы пуст, и
                    // всякий успех читался бы как «без зачёта».
                    if (toolQuest)
                        SnapshotQuestObjectives(self, toolQuest, toolCredits, c.ToolWas);
                    else
                        SnapshotObjectives(self, entry, c.ToolWas);
                    c.ToolWasEntry = entry;
                    // ЛИЧНОСТЬ ПРЕДМЕТА — ДО ВЫЗОВА: успешное применение может израсходовать
                    // и уничтожить его (Spell::TakeCastItem), и указатель после обработчика
                    // трогать нельзя (Кодекс: use-after-free в первой редакции).
                    uint32 const toolEntry = tool->GetEntry();
                    WorldPacket raw(CMSG_USE_ITEM);
                    WorldPackets::Spells::UseItem use(std::move(raw));
                    use.PackSlot = tool->GetBagSlot();
                    use.Slot = tool->GetSlot();
                    use.CastItem = tool->GetGUID();
                    use.Cast.CastID = ObjectGuid::Create<HighGuid::Cast>(
                        SPELL_CAST_SOURCE_NORMAL, self->GetMapId(), toolSpell,
                        self->GetMap()->GenerateLowGuid<HighGuid::Cast>());
                    use.Cast.SpellID = int32(toolSpell);
                    // ЦЕЛЬ — ПО КОНТРАКТУ ЗАКЛИНАНИЯ, а не «всегда существо»: явная цель,
                    // точка на земле или вовсе без цели (область вокруг себя).
                    if (toolUnit)
                    {
                        use.Cast.Target.Flags = TARGET_FLAG_UNIT;
                        use.Cast.Target.Unit = c.TalkGuid;
                    }
                    else if (toolInfo->GetExplicitTargetMask() & TARGET_FLAG_DEST_LOCATION)
                    {
                        use.Cast.Target.Flags = TARGET_FLAG_DEST_LOCATION;
                        WorldPackets::Spells::TargetLocation loc;
                        loc.Location = who->GetPosition();
                        use.Cast.Target.DstLocation = loc;
                    }
                    else
                        use.Cast.Target.Flags = TARGET_FLAG_NONE;
                    tool = nullptr;
                    uint32 const castMs = toolInfo->CalcCastTime();
                    c.Session->HandleUseItemOpcode(use);
                    c.ToolWaitMs = std::max<uint32>(4000, castMs + 2500);   // окно от времени произнесения
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ПРИМЕНЕНИЕ {}: предмет {} (закл. {}, {}) на {} ({}) с {:.1f} ярдов",
                        self->GetName(), toolEntry, toolSpell,
                        toolUnit ? "по цели" : (use.Cast.Target.Flags == TARGET_FLAG_DEST_LOCATION ? "по месту" : "без цели"),
                        name, entry, self->GetExactDist(who));
                    return;
                }

                // ПЕРЕБИРАТЬ ПУНКТЫ БЕСЕДЫ НЕЛЬЗЯ. ЭТО БЫЛ ИСПОЛНИТЕЛЬ ЧЕГО УГОДНО.
                //
                // Первая версия выбирала подряд все пункты меню, пока цель не закроется.
                // Кодекс отказал в выкладке и перечислил, что один такой пакет исполняет
                // НЕМЕДЛЕННО, без всякого следующего: снятие денег за пункт, отключение
                // получения опыта, произвольный сценарий существа — а значит телепорт,
                // уничтожение предметов и даже убийство персонажа. Я собирался запустить
                // это на ста двадцати двух живых персонажах.
                //
                // Правильный отбор берётся из САМИХ ДАННЫХ, а не из моей эвристики. У
                // существа есть его правила (SmartAIMgr отдаёт их модулю), и среди них
                // видно, какой пункт даёт зачёт: событие «выбран пункт меню» с действием
                // «применить заклинание» или «выдать зачёт убийства». Пара «отправитель +
                // действие» из правила совпадает с такими же полями пункта меню — по ним
                // и опознаём. Всё остальное не трогаем ВООБЩЕ: телепорт, торговля, обучение,
                // плата за пункт в этот список по построению не попадут.
                std::set<std::pair<uint32, uint32>> allowed, scripted;
                for (SmartScriptHolder const& e :
                     sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
                {
                    if (e.GetEventType() != SMART_EVENT_GOSSIP_SELECT)
                        continue;
                    uint32 const act = e.GetActionType();
                    // ВСЕ пары со сценарием — чтобы знать, на что НЕ нажимать по дороге
                    scripted.insert({ e.event.gossip.sender, e.event.gossip.action });
                    if (act != SMART_ACTION_CALL_KILLEDMONSTER && act != SMART_ACTION_SELF_CAST)
                        continue;
                    allowed.insert({ e.event.gossip.sender, e.event.gossip.action });
                }
                if (allowed.empty())
                {
                    TC_LOG_INFO("server.worldserver",
                        "Constellation РЕЧЬ {}: у {} ({}) нет пункта, дающего зачёт — не трогаю",
                        self->GetName(), name, entry);
                    c.TalkBackoff[entry] = 600000;
                    c.TalkGuid.Clear();
                    Switch(c, self, Behavior::Idle, "говорить не о чем");
                    return;
                }

                // ТОЧНОЕ ПРОДВИЖЕНИЕ, А НЕ «ВИД ИСЧЕЗ ИЗ СПИСКА» (Кодекс).
                //
                // Прежняя проверка смотрела, остался ли вид в общем наборе нужных. Она лгала
                // в обе стороны: цель могла закрыться в тот же такт по другой причине, а при
                // двух заданиях на один вид не менялась вовсе. Запоминаем счётчики именно
                // тех целей, что ссылаются на это существо, и сверяем их же.
                std::vector<std::pair<std::pair<uint32, uint32>, int32>> before;
                for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                {
                    uint32 const qid = self->GetQuestSlotQuestId(slot);
                    if (!qid || self->GetQuestStatus(qid) != QUEST_STATUS_INCOMPLETE)
                        continue;
                    Quest const* q = sObjectMgr->GetQuestTemplate(qid);
                    if (!q)
                        continue;
                    for (QuestObjective const& obj : q->GetObjectives())
                        if (obj.Type == QUEST_OBJECTIVE_MONSTER && uint32(obj.ObjectID) == entry)
                            before.push_back({ { qid, obj.ID }, self->GetQuestObjectiveData(qid, obj.ID) });
                }

                {
                    WorldPacket raw(CMSG_TALK_TO_GOSSIP);
                    WorldPackets::NPC::Hello hello(std::move(raw));
                    hello.Unit = c.TalkGuid;
                    c.Session->HandleGossipHelloOpcode(hello);
                }

                // Меню читаем ТЕКУЩЕЕ и сразу: снимок устаревает, а устаревший номер пункта
                // может попасть в сценарий уже другим действием (Кодекс).
                GossipMenu const& menu = self->PlayerTalkClass->GetGossipMenu();
                uint32 const menuId = menu.GetMenuId();
                int32 pick = -1;
                // РАЗГОВОР ДВУХШАГОВЫЙ, И ЭТО ВИДНО В ДАННЫХ.
                //
                // Существо привязано к одному меню, а зачёт лежит в другом: Лилиан Восс
                // открывает 12483, а правило ждёт выбора в 12484; у Маршала Редпата 12485
                // против 12486, у Валдреда Морея 12487 против 12489. Первый шаг разговора —
                // это переход в следующее окно, и только там нужный пункт.
                //
                // Прежний защитный фильтр отвергал пункты с переходом в подменю — то есть
                // ровно тот шаг, без которого до цели не дойти. Замер: «разрешённых пунктов
                // 1, выбран -1» у всех восьмидесяти разговоров.
                //
                // Переход разрешаем, но НЕ любой: только в то меню, которое само числится в
                // разрешённых по данным. Такой пункт ничего не делает, кроме открытия
                // следующего окна, и увести в торговлю или телепорт не может.
                //
                // Сверяем при этом то же, что сверяет ядро: обработчик передаёт сценарию
                // номер меню и OrderIndex пункта (NPCHandler.cpp), а сценарий сличает их со
                // своими sender и action (SmartScript.cpp:3563) — не поля Sender/Action
                // самого пункта, как я решил вначале по созвучию имён.
                std::set<uint32> wantMenus;
                for (auto const& [m, o] : allowed)
                    wantMenus.insert(m);

                auto safeOption = [](GossipMenuItem const& item) -> bool
                {
                    return !item.BoxCoded && item.BoxMoney == 0 && item.ActionPoiID == 0
                        && !item.SpellID && item.OptionNpc == GossipOptionNpc::None;
                };

                bool done = false;
                pick = -1;
                for (uint8 hop = 0; hop < 3 && !done; ++hop)
                {
                    GossipMenu const& menu = self->PlayerTalkClass->GetGossipMenu();
                    uint32 const menuId = menu.GetMenuId();

                    // 1) есть ли прямо здесь пункт, дающий зачёт
                    pick = -1;
                    for (GossipMenuItem const& item : menu.GetMenuItems())
                        if (allowed.count({ menuId, item.OrderIndex }) && safeOption(item)
                            && item.ActionMenuID == 0)
                            { pick = item.GossipOptionID; break; }

                    if (pick >= 0)
                    {
                        WorldPacket raw(CMSG_GOSSIP_SELECT_OPTION);
                        WorldPackets::NPC::GossipSelectOption sel(std::move(raw));
                        sel.GossipUnit = c.TalkGuid;
                        sel.GossipID = menuId;
                        sel.GossipOptionID = pick;
                        c.Session->HandleGossipSelectOptionOpcode(sel);

                        for (auto const& [key, was] : before)
                            if (self->GetQuestObjectiveData(key.first, key.second) > was)
                                { done = true; break; }
                        break;
                    }

                    // 2) иначе ищем переход ИМЕННО в нужное меню
                    // ПЕРЕХОД БЫВАЕТ НЕ ОДИН. У Валдреда Морея цепочка 12487 -> 12488 -> 12489,
                    // и прямого пункта в нужное меню из первого окна просто нет. Замер: у него
                    // одного 24 отказа «выбран -1», при двенадцати удачных разговорах с теми,
                    // у кого переход соседний.
                    //
                    // Поэтому разрешаем и промежуточный переход, но с жёстким условием: на
                    // этот пункт НЕ навешено ни одного правила сценария. Тогда он физически
                    // не может сделать ничего, кроме открытия следующего окна — ни каста, ни
                    // телепорта, ни платы. Сначала всё же пробуем прямой путь в нужное меню.
                    int32 step = -1;
                    uint32 stepMenu = menuId;
                    for (GossipMenuItem const& item : menu.GetMenuItems())
                        if (item.ActionMenuID != 0 && wantMenus.count(item.ActionMenuID)
                            && safeOption(item))
                            { step = item.GossipOptionID; break; }
                    if (step < 0)
                        for (GossipMenuItem const& item : menu.GetMenuItems())
                            if (item.ActionMenuID != 0 && safeOption(item)
                                && !scripted.count({ menuId, item.OrderIndex }))
                                { step = item.GossipOptionID; break; }
                    if (step < 0)
                        break;                  // дальше идти некуда — не тычемся наугад

                    WorldPacket raw(CMSG_GOSSIP_SELECT_OPTION);
                    WorldPackets::NPC::GossipSelectOption sel(std::move(raw));
                    sel.GossipUnit = c.TalkGuid;
                    sel.GossipID = stepMenu;
                    sel.GossipOptionID = step;
                    c.Session->HandleGossipSelectOptionOpcode(sel);
                }

                if (done)
                {
                    ++c.Talked;
                    TC_LOG_INFO("server.worldserver",
                        "Constellation РЕЧЬ {}: поговорил с {} ({}), пункт {}; всего разговоров {}",
                        self->GetName(), name, entry, pick, c.Talked);
                }
                else
                {
                    TC_LOG_INFO("server.worldserver",
                        "Constellation РЕЧЬ {}: {} ({}) — разрешённых пар {}, выбран {}, зачёта нет",
                        self->GetName(), name, entry, uint32(allowed.size()), pick);
                    c.TalkBackoff[entry] = 120000;
                }
                c.TalkGuid.Clear();
                Switch(c, self, Behavior::Idle, done ? "поговорил" : "разговор без толку");
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
                    {
                        how = "ПОБЕДА";
                        c.DeathAt.clear();          // выиграл — значит зона по нему
                    }
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
        if (c.RingHeld && c.ApproachFor == target->GetGUID())
        {
            x = c.ApproachX; y = c.ApproachY; z = c.ApproachZ;   // точка обхода — до прихода или отказа
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
    Creature* FindVendorNear(Companion const& c, Player* self, bool needSell, bool needRepair) const
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
            if (needSell && c.VendorNoSell.count(cr->GetEntry()))
                continue;                           // недавно отказал во всём — не продавец
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
    // ПРОДАЖА ПО ПРАВИЛУ ОПЕРАТОРА (2026-09-02): «неподходящие предметы тоже надо продавать;
    // оставлять подходящие предметы только +2 уровня от персонажа». До этого продавалось
    // только серое: за двадцать минут ноль продаж при 456 стопках в сумках у состава.
    //
    //   защищено  = нужно активному квесту | начинает квест | класс «квест» | камень
    //               возвращения | сумка | надето | расходник (пока модуль их не применяет)
    //   продаётся = есть цена И не защищено И (серое | непригодно никогда | нужен уровень > наш+2)
    //
    // «Непригодно никогда» спрашивается у ядра ЕГО вопросом — CanUseItem по ШАБЛОНУ с
    // пропущенной проверкой уровня: класс, раса, навык брони и оружия, нужное заклинание,
    // фракция. Перегрузка по предмету проверяет текущий уровень и назвала бы непригодным всё,
    // что выше нас, — а такое по правилу остаётся, если оно в пределах +2.
    //
    // Защита старше причины: серый предмет, начинающий квест, не продаётся. Тип брони — вопрос
    // надевания, не продажи: воин в тряпках оставит их по этому правилу и перерастёт по
    // правилу брони (задача 0023, п. 5). Товары ремесла продаются: ни ремесла, ни банка нет.
    // ВЕРДИКТ ПО ПРЕДМЕТУ — ОДИН ДЛЯ ПРОДАЖИ И ДЛЯ ПОДСЧЁТА ХЛАМА. Две функции с одним
    // правилом разошлись бы; здесь правило записано один раз и спрашивается дважды.
    enum class SellVerdict : uint8 { Quest, StartsQuest, Hearthstone, Consumable, Fit, BagKeep, Unsellable, Sell };

    // КАКИЕ НЕНАДЕТЫЕ СУМКИ ОСТАВИТЬ — считается один раз за проход, а не по ходу обхода
    // (Кодекс: раньше держались первые четыре по порядку, а при пустом слоте — все). Самые
    // большие — по одной на каждый пустой слот и на каждую надетую, которую они превосходят.
    std::set<ObjectGuid> BagsToKeep(Player* self) const
    {
        std::vector<Item*> loose;
        std::vector<uint32> worn;
        uint32 emptySlots = 0;
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* w = self->GetBagByPos(b))
                worn.push_back(GetBagSize(w));
            else
                ++emptySlots;
        auto collect = [&](Item* it) { if (it && it->IsBag() && !it->IsEquipped()) loose.push_back(it); };
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            collect(self->GetItemByPos(INVENTORY_SLOT_BAG_0, i));
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = self->GetBagByPos(b))
                for (uint32 j = 0; j < GetBagSize(bag); ++j)
                    collect(GetItemInBag(bag, j));
        std::sort(loose.begin(), loose.end(), [](Item* a, Item* b)
            { return a->GetTemplate()->GetContainerSlots() > b->GetTemplate()->GetContainerSlots(); });
        std::sort(worn.begin(), worn.end());
        std::set<ObjectGuid> keep;
        size_t wornIdx = 0;
        for (Item* it : loose)
        {
            if (emptySlots > 0)
                { keep.insert(it->GetGUID()); --emptySlots; continue; }
            if (wornIdx < worn.size() && it->GetTemplate()->GetContainerSlots() > worn[wornIdx])
                { keep.insert(it->GetGUID()); ++wornIdx; continue; }
            break;                                  // дальше идут не лучше — им к торговцу
        }
        return keep;
    }

    SellVerdict ClassifyForSale(Player* self, Item* it, std::set<ObjectGuid> const& keepBags) const
    {
        ItemTemplate const* tpl = it->GetTemplate();
        uint32 const myLevel = self->GetLevel();
        // --- защищённое, в порядке важности — и РАНЬШЕ сумочной ветки: сумка, нужная квесту
        // или начинающая его, — не сумка, а квест
        if (self->HasQuestForItem(tpl->GetId()) || tpl->GetClass() == ITEM_CLASS_QUEST)
            return SellVerdict::Quest;
        if (tpl->GetStartQuest())
            return SellVerdict::StartsQuest;
        if (it->IsBag())
        {
            if (keepBags.count(it->GetGUID()))
                return SellVerdict::BagKeep;        // наденется или заменит надетую (BagsToKeep)
            return tpl->GetSellPrice() ? SellVerdict::Sell : SellVerdict::Unsellable;
        }
        if (tpl->GetId() == 6948)
            return SellVerdict::Hearthstone;
        if (tpl->GetClass() == ITEM_CLASS_CONSUMABLE)
            return SellVerdict::Consumable;             // будущая ценность (каталог, стр. 42)
        // --- причины продать
        bool const grey = tpl->GetQuality() == ITEM_QUALITY_POOR;
        // «НИКОГДА» — ДВА ВОПРОСА, И ОБА У ЯДРА. CanUseItem по шаблону отвечает за класс,
        // расу, фракцию, требуемое заклинание и явный навык — но не за навык подкласса оружия
        // и не за тип брони: это делает перегрузка по предмету (Player.cpp:11296-11304),
        // которая заодно проверяет уровень. Повторяем те же два вопроса; «навык выучен»
        // заменён на «навык существует для расы и класса», иначе то, что ещё выучится у
        // тренера, ушло бы к торговцу. Маски классов прочитаны из пробного мира 2026-09-02.
        bool neverUsable = self->CanUseItem(tpl, /*skipRequiredLevelCheck=*/true) != EQUIP_ERR_OK;
        if (!neverUsable && Cfg().SellByWeaponSkill && tpl->GetClass() == ITEM_CLASS_WEAPON)
            if (uint32 const skill = tpl->GetSkill())
                if (!sDB2Manager.GetSkillRaceClassInfo(skill, self->GetRace(), self->GetClass()))
                    neverUsable = true;                 // жезл у воина, двуручный меч у мага
        if (!neverUsable && tpl->GetClass() == ITEM_CLASS_ARMOR && tpl->GetInventoryType() != INVTYPE_CLOAK)
            if (ChrClassesEntry const* cls = sChrClassesStore.LookupEntry(self->GetClass()))
                if ((cls->ArmorTypeMask & 0x21u) == 0x21u
                    && !(cls->ArmorTypeMask & (1u << tpl->GetSubClass())))
                    neverUsable = true;                 // ткань и кожа у воина, латы и щит у мага
        bool const tooHigh = tpl->GetBaseRequiredLevel() > int32(myLevel) + 2;
        if (!grey && !neverUsable && !tooHigh)
            return SellVerdict::Fit;                    // пригодное и по уровню — оставляем
        return tpl->GetSellPrice() ? SellVerdict::Sell : SellVerdict::Unsellable;
    }

    // СЕЗОННОЕ — ПО СОРТИРОВКЕ КВЕСТА, А НЕ ПО ПРЕДИКАТУ ЯДРА (замер, 2026-09-03).
    //
    // Quest::IsSeasonal() требует ещё и «не повторяемый», а еженедельным ядро ставит
    // повторяемость само (ObjectMgr.cpp:4799) — поэтому «Time Trials» (сортировка -22, флаг
    // еженедельного) сезонным для него не является, и первая редакция этой правки не сработала
    // вовсе. Берём тот же список сортировок, что перечислен в ядре, без оговорки.
    static bool SeasonalKind(Quest const* q)
    {
        switch (-q->GetZoneOrSort())
        {
            case QUEST_SORT_SEASONAL:
            case QUEST_SORT_SPECIAL:
            case QUEST_SORT_LUNAR_FESTIVAL:
            case QUEST_SORT_MIDSUMMER:
            case QUEST_SORT_BREWFEST:
            case QUEST_SORT_LOVE_IS_IN_THE_AIR:
            case QUEST_SORT_NOBLEGARDEN:
                return true;
            default:
                return false;
        }
    }

    // КВЕСТ ПРОФЕССИИ, КОТОРОЙ У НАС НЕТ (оператор, 2026-09-03: «брать квесты профессий только
    // под профессии, которые есть у персонажа; а сейчас таких нет»).
    //
    // Навык называет само ядро: SkillByQuestSort по сортировке квеста (SharedDefines.h:6190), и
    // оно же сверяет по ней RequiredSkillId при загрузке (ObjectMgr.cpp:4858). Второй источник —
    // сам RequiredSkillId. Дальше вопрос к персонажу: есть ли у него этот навык.
    static bool ProfessionWeLack(Player* self, Quest const* q)
    {
        uint32 skill = SkillByQuestSort(-q->GetZoneOrSort());
        if (!skill)
            skill = q->GetRequiredSkill();
        return skill != 0 && !self->HasSkill(skill);
    }

    // ГОДИТСЯ ЛИ ВЕЩЬ ЭТОМУ ПЕРСОНАЖУ ВООБЩЕ — те же два вопроса, что задаёт продажа, вынесены
    // отдельно, потому что теперь их задаёт и выбор награды. Проверка по шаблону отвечает за
    // класс, расу, фракцию и требуемое заклинание, но НЕ за навык подкласса оружия и не за тип
    // брони: это делает перегрузка по предмету, которой у награды ещё нет — предмета не
    // существует, пока его не выдали.
    bool UsableKind(Player* self, ItemTemplate const* tpl) const
    {
        if (!tpl)
            return false;
        if (self->CanUseItem(tpl, /*skipRequiredLevelCheck=*/true) != EQUIP_ERR_OK)
            return false;
        if (tpl->GetClass() == ITEM_CLASS_WEAPON)
            if (uint32 const skill = tpl->GetSkill())
                if (!sDB2Manager.GetSkillRaceClassInfo(skill, self->GetRace(), self->GetClass()))
                    return false;
        if (tpl->GetClass() == ITEM_CLASS_ARMOR && tpl->GetInventoryType() != INVTYPE_CLOAK)
            if (ChrClassesEntry const* cls = sChrClassesStore.LookupEntry(self->GetClass()))
                if ((cls->ArmorTypeMask & 0x21u) == 0x21u
                    && !(cls->ArmorTypeMask & (1u << tpl->GetSubClass())))
                    return false;
        return true;
    }

    // КАКУЮ НАГРАДУ ВЫБРАТЬ, КОГДА ПРЕДЛАГАЮТ НЕСКОЛЬКО (оператор, 2026-09-03).
    //
    // Раньше бралась первая из списка — не выбор, а заглушка, чтобы ядро не отвергло сдачу с
    // нулём. Игрок смотрит иначе: годится ли вещь ему по классу, встанет ли она в слот, и лучше
    // ли того, что там уже надето. Ровно это и спрашиваем; уровень предмета берём у ядра
    // (ItemTemplate::GetBaseItemLevel), а не выводим из цены. Ничего годного — берём самое
    // дорогое: его продадут.
    uint32 PickReward(Player* self, Quest const* quest, LootItemType* typeOut) const
    {
        uint32 const count = quest->GetRewChoiceItemsCount();
        if (typeOut)
            *typeOut = LootItemType::Item;
        if (!count)
            return 0;
        uint32 best = 0, bestScore = 0, bestPrice = 0;
        uint32 dearest = 0, dearestPrice = 0, first = 0;
        for (uint32 i = 0; i < count && i < QUEST_REWARD_CHOICES_COUNT; ++i)
        {
            // ЗАПАСНОЙ ПУТЬ ТОЖЕ ОБЯЗАН БЫТЬ ПРЕДМЕТОМ (Кодекс): среди наград бывают валюты, а
            // пакет уходит с типом «предмет» — несовпадение ядро отвергнет.
            if (!quest->RewardChoiceItemId[i] || quest->RewardChoiceItemType[i] != LootItemType::Item)
                continue;
            if (!first)
                first = quest->RewardChoiceItemId[i];
            ItemTemplate const* tpl = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[i]);
            if (!tpl)
                continue;
            uint32 const price = tpl->GetSellPrice();
            if (price > dearestPrice)
                { dearestPrice = price; dearest = quest->RewardChoiceItemId[i]; }
            if (!UsableKind(self, tpl) || tpl->GetInventoryType() == INVTYPE_NON_EQUIP)
                continue;
            // НАСКОЛЬКО ЭТО ЛУЧШЕ НАДЕТОГО — ПО ХУДШЕМУ ИЗ ПОДХОДЯЩИХ СЛОТОВ (Кодекс). У колец и
            // аксессуаров слота два, и новое меняет ХУДШЕЕ; максимум по виду слота отверг бы
            // годное кольцо из-за второго, хорошего. Пустой подходящий слот — всегда улучшение.
            // Сравниваем уровень ШАБЛОНА с уровнем ШАБЛОНА надетого: награды ещё не существует,
            // её масштабирование неизвестно, и мерить её базовый уровень против действующего
            // уровня надетого значило бы сравнивать разные величины.
            uint32 worn = 0xFFFFFFFF;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            {
                Item* have = self->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (have && have->GetTemplate()->GetInventoryType() == tpl->GetInventoryType())
                    worn = std::min(worn, have->GetTemplate()->GetBaseItemLevel());
            }
            if (worn == 0xFFFFFFFF)
                worn = 0;                       // такого слота не занято — надеть некуда мешать
            uint32 const mine = tpl->GetBaseItemLevel();
            uint32 const score = mine > worn ? (mine - worn) + 1000 : 1;    // апгрейд впереди всего
            if (score > bestScore || (score == bestScore && price > bestPrice))
                { bestScore = score; bestPrice = price; best = quest->RewardChoiceItemId[i]; }
        }
        return best ? best : (dearest ? dearest : first);
    }

    // СКОЛЬКО СТОПОК ПРАВИЛО ПРОДАЛО БЫ — повод идти к торговцу, не дожидаясь полных сумок.
    uint32 SellableCount(Companion const& c, Player* self) const
    {
        uint32 count = 0;
        std::set<ObjectGuid> const keepBags = BagsToKeep(self);
        auto look = [&](Item* it)
        {
            if (it && !it->IsEquipped() && it->GetTemplate()
                && !c.SellRefused.count(it->GetGUID())     // ядро уже отказало — не повод для похода
                && ClassifyForSale(self, it, keepBags) == SellVerdict::Sell)
                ++count;
        };
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            look(self->GetItemByPos(INVENTORY_SLOT_BAG_0, i));
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = self->GetBagByPos(b))
                for (uint32 j = 0; j < GetBagSize(bag); ++j)
                    look(GetItemInBag(bag, j));
        return count;
    }

    uint32 BagCapacity(Player* self) const
    {
        uint32 cap = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START;
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* w = self->GetBagByPos(b))
                cap += GetBagSize(w);
        return cap;
    }

    uint32 SellJunkTo(Companion& c, Player* self, Creature* vendor)
    {
        std::vector<Item*> junk;
        uint32 seenStacks = 0, seenPieces = 0, keptQuest = 0, keptStart = 0, keptStone = 0,
               keptConsumable = 0, keptFit = 0, unsellable = 0;
        // СУМКИ ВНЕ СЛОТОВ (оператор: «ненадетые продавать, надевать более вместительные»).
        // Пустой слот есть — сумку наденет EquipBags на следующем проходе, не продаём; она
        // больше наименьшей надетой — ждёт замены (замена требует сперва опустошить меньшую,
        // это задача 0023, п. 10), не продаём; всё остальное — к торговцу.
        std::set<ObjectGuid> const keepBags = BagsToKeep(self);
        uint32 keptBag = 0, skippedRefused = 0;
        auto consider = [&](Item* it)
        {
            if (!it || it->IsEquipped() || !it->GetTemplate())
                return;
            ++seenStacks;
            seenPieces += it->GetCount();
            if (c.SellRefused.count(it->GetGUID()))
                { ++skippedRefused; return; }       // недавно отказано — не повторяем
            switch (ClassifyForSale(self, it, keepBags))
            {
                case SellVerdict::Quest:       ++keptQuest; return;
                case SellVerdict::StartsQuest: ++keptStart; return;
                case SellVerdict::Hearthstone: ++keptStone; return;
                case SellVerdict::Consumable:  ++keptConsumable; return;
                case SellVerdict::Fit:         ++keptFit; return;
                case SellVerdict::BagKeep:     ++keptBag; return;
                case SellVerdict::Unsellable:  ++unsellable; return;
                case SellVerdict::Sell:        junk.push_back(it); return;
            }
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
            {
                ++refused;              // остался у нас — значит не продан
                c.SellRefused[itemGuid] = 600000;   // десять минут не считать его поводом (Кодекс)
            }
            else
                ++sold;
        }
        // ОТКАЗАЛ ВО ВСЁМ — этот торговец десять минут не продавец: без выкупа или не берёт
        // такое; иначе поход к нему повторялся бы по кругу
        if (!junk.empty() && sold == 0)
            c.VendorNoSell[vendor->GetEntry()] = 600000;
        uint64 const earned = self->GetMoney() > before ? self->GetMoney() - before : 0;
        c.VendSold += sold;
        c.VendEarned += earned;
        // ОДНА СТРОКА НА ВИЗИТ, ВКЛЮЧАЯ ПУСТОЙ: что просмотрено, что продано, что и почему
        // оставлено. Без неё «ноль продаж» неотличим от «продавать было нечего».
        TC_LOG_INFO("server.worldserver",
            "Constellation ТОРГ {}: у {} ({}) просмотрено {} стопок/{} вещей; продано {} за {} мед.; "
            "оставлено: квест {}, начинает квест {}, камень {}, расходники {}, пригодные <=+2 {}, сумки {}; "
            "нельзя продать {}; ядро отказало {} (ранее отказанных пропущено {})",
            self->GetName(), vendor->GetName(), vendor->GetEntry(), seenStacks, seenPieces,
            sold, earned, keptQuest, keptStart, keptStone, keptConsumable, keptFit, keptBag, unsellable, refused, skippedRefused);
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
    // ЗАБРАТЬ ИЗ УЖЕ ОТКРЫТОГО ВИДА ДОБЫЧИ.
    //
    // Вынесено из LootFromCorpse без единого изменения смысла: труп и объект на земле
    // отличаются только тем, ЧЕМ вид открывается — CMSG_LOOT_UNIT против CMSG_GAME_OBJ_USE.
    // Дальше ядро наполняет один и тот же m_AELootView, поэтому деньги, предметы, отчёт и
    // освобождение вида — общие. Второй такой процедуры заводить нельзя (ступень 2
    // лестницы: сперва ищем, что уже написано).
    //
    // Возвращает, сколько предметов было ЗАПРОШЕНО; сколько реально легло, считает и
    // печатает сама — по свободному месту до и после, а не по числу запросов.
    // Возвращает, сколько предметов РЕАЛЬНО ЛЕГЛО (не запрошено): Кодекс верно указал, что
    // счётчик по запросам называет успехом заход, с которого ничего не взяли.
    uint32 TakeOpenLoot(Companion& c, Player* self, ObjectGuid src, std::string const& what, uint32 entry)
    {
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
        uint32 asked = 0, got = 0;
        std::set<uint32> askedIds;      // ЧТО именно просили — чтобы сосчитать пришедшее
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
                askedIds.insert(item.itemid);
                ++asked;
            }
        }
        if (asked)
        {
            // СЧИТАЕМ ВЗЯТОЕ, А НЕ ЗАПРОШЕННОЕ. Тот же урок, что и в продаже: ядро может
            // отказать (переполнение стопки, уникальность, полные сумки), и запрос не
            // равен предмету в рюкзаке. Меряем по свободному месту до и после — это
            // ровно то, что изменилось бы, если предмет действительно лёг.
            // СЧИТАЕМ ПО КОЛИЧЕСТВУ ПРЕДМЕТОВ, А НЕ ПО СВОБОДНЫМ ЯЧЕЙКАМ.
            //
            // Оговорка про стопки стояла тут же, в старом сообщении: предмет, ушедший в
            // УЖЕ ИМЕЮЩУЮСЯ стопку, места не занимает. Пока это число было только строкой
            // отчёта, ошибка ничего не стоила. Но сбор с объектов принимает по нему
            // решение — «ничего не легло, объект в отсрочку», — и удачный сбор второго
            // цветка того же вида объявлялся пустым заходом. Нашёл стоп-разбор Кодекса.
            //
            // Спрашиваем у ядра, СКОЛЬКО ЭТИХ предметов у нас было и стало: стопки при
            // таком счёте видны, а чужое добро в счёт не идёт.
            uint32 countBefore = 0;
            for (uint32 id : askedIds)
                countBefore += self->GetItemCount(id, true);
            uint32 const spaceBefore = FreeBagSpace(self);
            c.Session->HandleAutostoreLootItemOpcode(take);
            uint32 const spaceAfter = FreeBagSpace(self);
            uint32 countAfter = 0;
            for (uint32 id : askedIds)
                countAfter += self->GetItemCount(id, true);

            uint32 const landed = countAfter > countBefore ? countAfter - countBefore : 0;
            uint32 const slotsUsedUp = spaceBefore > spaceAfter ? spaceBefore - spaceAfter : 0;
            got = landed;
            c.LootItems += landed;
            if (landed != asked)
                TC_LOG_INFO("server.worldserver",
                    "Constellation ЛУТ {}: запрошено {}, легло {} (ячеек занято {}) — "
                    "разница это стопки и отказы ядра",
                    self->GetName(), asked, landed, slotsUsedUp);
        }

        TC_LOG_INFO("server.worldserver",
            "Constellation ЛУТ {}: обобрал {} ({}) — денег {}, предметов запрошено {}; "
            "за всё время трупов {}, предметов {}, денег {}",
            self->GetName(), what, entry,
            anyGold ? "да" : "нет", asked,
            c.LootOpened, c.LootItems, c.LootMoney);

        // 4. ОТПУСТИТЬ. Иначе вид остаётся открытым и следующий труп не откроется.
        {
            WorldPacket raw(CMSG_LOOT_RELEASE);
            WorldPackets::Loot::LootRelease done(std::move(raw));
            done.Unit = src;
            c.Session->HandleLootReleaseOpcode(done);
        }
        return got;
    }

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

        TakeOpenLoot(c, self, c.LootTarget, corpse->GetName(), corpse->GetEntry());
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
        // ВЫХОД ИЗ РАЗГОВОРА ЛЮБЫМ ПУТЁМ — окно ожидания и снимок счётчиков его не переживают
        // (Кодекс): иначе следующий разговор с другой целью начинался бы со старым окном и
        // зачёл бы чужой рост счётчика или записал бы бесплодное окно новой особи. Здесь, а
        // не в каждом из девяти выходов, потому что и гибель проходит через Switch.
        if (c.Mode == Behavior::Talking && to != Behavior::Talking)
        {
            c.ToolWaitMs = 0;           // окно — не переживает; снимок ToolWas остаётся: он помечен
            c.ToolFruitless = 0;        // видом и нужен, чтобы увидеть зачёт, пришедший после окна
        }
        if (c.Mode == Behavior::Vending && to != Behavior::Vending)
            c.VendorEntry = 0;          // поход по карте кончился — любым исходом
        if (c.Mode == Behavior::SeekingGiver && to != Behavior::SeekingGiver)
            c.SeekEntry = 0;            // и поход к квестодателю по карте — тоже
        c.WalkBest = 1.0e9f;            // бюджет прогресса начинается заново в каждом режиме
        c.WalkStuckMs = 0;
        c.Mode = to;
        c.ModeMs = 0;
        if (to != Behavior::TurningIn)
            c.TurnInGuid.Clear();       // другое намерение — найденный принимающий не наш
        if (to != Behavior::Attacking)
            { c.VictimHp = 0; c.NoDamageMs = 0; c.DamageVictim.Clear(); }
        c.Stalled = false;              // новое намерение — новая попытка дойти
        c.RingTried = false;            // и обход точек вокруг NPC снова доступен (помост)
        c.RingHeld = false;
        c.Kiting = false;               // и отвод начинается заново в следующем бою
        c.KiteMs = 0;
        c.KitePath.clear();
        c.KiteIdx = 0;
        c.PackCenterKnown = false;
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
            case Behavior::Gathering:         return "иду собирать";
            case Behavior::Talking:           return "иду взаимодействовать";
            case Behavior::SeekingGiver:      return "иду к квестодателю по карте";
            case Behavior::TakingFlight:      return "иду к полётному мастеру";
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
                || self->GetExactDist(going) > std::max(Cfg().QuestGiverRange, c.GiverRange) + 15.0f)
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
                float const now = ProgressDist(c, self, going);
                bool const noProgress = c.GiverMs >= 20000 && now > c.GiverDist - 1.0f;
                bool const giverDone = c.Stalled || noProgress || c.GiverMs >= 30000;
                if (giverDone && FindReachableApproach(c, self, going))
                    { c.GiverMs = 0; c.GiverDist = ProgressDist(c, self, going); return; }
                if (giverDone)
                {
                    LogApproachFailure(c, self, going, "квестодателю");
                    TC_LOG_INFO("server.worldserver",
                        "Constellation: {} — до квестодателя {} ({}) не дойти за {} с, "
                        "было {:.1f} ярдов, стало {:.1f}; больше не пробую",
                        self->GetName(), going->GetName(), going->GetEntry(),
                        c.GiverMs / 1000, c.GiverDist, now);
                    c.GiverUnreachable[c.GiverGuid] = 600000;   // иначе выберем его снова
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
                c.GiverRange = 0.0f;
            }
            return;
        }
        c.GiverGuid.Clear();                        // дошли и говорим — цель больше не нужна

        // ПЛАН: консультация точки ДО запроса меню — запись снимка с совместимостью по фазе
        Constellation::Plan::Planner::Instance()->OnConsult(self, giver);

        // «подойти и заговорить» — тот же опкод, что шлёт клиент по клику
        WorldPacket rawHello(CMSG_QUEST_GIVER_HELLO);
        WorldPackets::Quest::QuestGiverHello hello(std::move(rawHello));
        hello.QuestGiverGUID = giver->GetGUID();
        c.Session->HandleQuestgiverHelloOpcode(hello);

        // У NPC СО СЦЕНАРИЕМ БЕСЕДЫ ЭТОГО ОПКОДА МАЛО — И ЭТО НЕ ДОГАДКА, А СТРОКА ЯДРА.
        //
        //     _player->PlayerTalkClass->ClearMenus();
        //     if (creature->AI()->OnGossipHello(_player))
        //         return;                              <-- выход ДО подготовки меню
        //     _player->PrepareQuestMenu(creature->GetGUID());
        //
        // То есть если у существа есть свой обработчик приветствия, меню квестов не
        // готовится вовсе. Замер это и показал: прибор напечатал «предложил пунктов 0» у
        // всех, кто дошёл до Milly Osworth, — не «ядро не даёт взять», не «отказные», а
        // ПУСТО. Спутник стучался не в ту дверь.
        //
        // Живой игрок в этот момент видит окно беседы, и его клиент шлёт CMSG_GOSSIP_HELLO.
        // Тот путь идёт через Player::PrepareGossipMenu, который меню квестов заполняет
        // (Player.cpp: два вызова PrepareQuestMenu внутри). Поэтому: пусто после первого
        // опкода — шлём второй, ровно как клиент, и читаем снова. Своих внутренних вызовов
        // ядра по-прежнему нет.
        if (self->PlayerTalkClass->GetQuestMenu().GetMenuItemCount() == 0)
        {
            // опкод в этом ядре зовётся CMSG_TALK_TO_GOSSIP (Opcodes.cpp), а не
            // CMSG_GOSSIP_HELLO — имя проверено по таблице обработчиков, не по памяти
            WorldPacket rawGossip(CMSG_TALK_TO_GOSSIP);
            WorldPackets::NPC::Hello gossip(std::move(rawGossip));
            gossip.Unit = giver->GetGUID();
            c.Session->HandleGossipHelloOpcode(gossip);
        }

        // читаем то, что ядро только что собрало для клиента, в его же порядке
        QuestMenu const& menu = self->PlayerTalkClass->GetQuestMenu();

        // ПЛАН: снимок точки и сверка настоящего меню с зеркалом ворот (только наблюдение)
        Constellation::Plan::Planner::Instance()->OnMenuRead(self, giver, menu,
            [this](Player* p, Quest const* q) { return FirstFailingGate(p, q); });

        // ЧТО ИМЕННО ПРЕДЛОЖИЛИ И ЧТО ИЗ ЭТОГО ОТВЕРГНУТО — ПО РАЗУ НА КАЖДОГО.
        //
        // Прибор выше показал, что 56 спутников из 122 квестодателя ВЫБИРАЮТ, при этом за
        // все часы взят один квест и ни одной строки «не дойти». Значит срыв здесь, и без
        // этой строки различить «меню пустое» и «CanTakeQuest всё отверг» нельзя.
        if (!c.TalkDiagDone)
        {
            c.TalkDiagDone = true;
            uint32 refused = 0, noTemplate = 0, cannotTake = 0;
            for (uint8 i = 0; i < menu.GetMenuItemCount(); ++i)
            {
                uint32 const qid = menu.GetItem(i).QuestId;
                if (c.QuestRefused.count(qid))
                    { ++refused; continue; }
                Quest const* q = sObjectMgr->GetQuestTemplate(qid);
                if (!q)
                    { ++noTemplate; continue; }
                if (!self->CanTakeQuest(q, false))
                    ++cannotTake;
            }
            TC_LOG_INFO("server.worldserver",
                "Constellation РАЗГОВОР {}: {} ({}) предложил пунктов {}, из них ранее отказных {}, "
                "без шаблона {}, ядро не даёт взять {}",
                self->GetName(), giver->GetName(), giver->GetEntry(),
                uint32(menu.GetMenuItemCount()), refused, noTemplate, cannotTake);
        }

        // ПОРЯДОК ВЫБОРА — ПО СВЕТОФОРУ, А НЕ ПО ПОРЯДКУ В МЕНЮ (оператор, 2026-09-02).
        //
        // Ядро отдаёт пункты в своём порядке, и первая версия брала первый попавшийся. Игрок
        // так не делает: он смотрит на цвет. Берём лучший цвет из предложенных, красное не
        // берём вовсе — «не доросли». При равном цвете сохраняется порядок ядра.
        // ОДНОРАЗОВЫЕ ВПЕРЕДИ ПОВТОРЯЕМЫХ (Кодекс: защита от голодания). Повторяемое задание
        // можно брать бесконечно, и серое повторяемое навсегда заслонило бы жёлтое одноразовое —
        // цепочка встала бы. Поэтому ключ выбора двойной: сперва одноразовые, внутри — по цвету.
        uint32 questId = 0;
        Quest const* quest = nullptr;
        uint8 bestColour = 4, bestRank = 255;
        uint32 skippedRed = 0, skippedUnknown = 0, offered = 0;
        for (uint8 i = 0; i < menu.GetMenuItemCount(); ++i)
        {
            uint32 const qid = menu.GetItem(i).QuestId;
            if (c.QuestRefused.count(qid))
                continue;                       // уже пробовали и не вышло
            Quest const* q = sObjectMgr->GetQuestTemplate(qid);
            if (!q || !self->CanTakeQuest(q, false))
                continue;
            // СЕЗОННОЕ — НЕ НАША РАБОТА (оператор, 2026-09-03: «это категория сезонных квестов,
            // нас такое сейчас не интересует»). Признак — сортировка квеста; таких в базе 422,
            // и вне своего события они висят в журнале мёртвым грузом, как 55660 у всех 122.
            if (SeasonalKind(q))
                continue;
            // ПРОФЕССИЯ, КОТОРОЙ У НАС НЕТ: её цели — созданные ремеслом предметы, которых в мире
            // не существует. Семеро пандаренов держат три таких квеста (алхимия, сортировка -181).
            if (ProfessionWeLack(self, q))
                continue;
            ++offered;
            uint8 const colour = QuestColour(self, q);
            if (colour == 4)
                { ++skippedUnknown; continue; } // уровень не определён — откладываем
            if (colour == 3)
                { ++skippedRed; continue; }     // красное — не по нам
            uint8 const rank = uint8((q->IsRepeatable() || q->IsDailyOrWeekly() ? 10 : 0) + colour);
            if (rank < bestRank)
                { bestRank = rank; bestColour = colour; questId = qid; quest = q; }
        }
        if ((skippedRed || skippedUnknown) && !c.RedNoted)
        {
            c.RedNoted = true;
            TC_LOG_INFO("server.worldserver",
                "Constellation СВЕТОФОР {} (ур. {}): у {} ({}) пропущено красных {}, неизвестных {}, взято {}",
                self->GetName(), uint32(self->GetLevel()), giver->GetName(), giver->GetEntry(),
                skippedRed, skippedUnknown, questId);
        }
        // ВЕСЬ ПРИЛАВОК НЕ ПО НАМ — К ЭТОМУ КВЕСТОДАТЕЛЮ БОЛЬШЕ НЕ ХОДИМ (Кодекс). Иначе
        // ближайший, у которого всё красное, выбирался бы снова и снова вместо соседнего с
        // подходящим заданием. Запрет по особи и не навсегда: уровень растёт, цвет меняется.
        if (!quest && offered)
            c.GiverUnreachable[giver->GetGUID()] = 600000;
        if (quest)
        {
            WorldPacket rawAccept(CMSG_QUEST_GIVER_ACCEPT_QUEST);
            WorldPackets::Quest::QuestGiverAcceptQuest accept(std::move(rawAccept));
            accept.QuestGiverGUID = giver->GetGUID();
            accept.QuestID = questId;
            c.Session->HandleQuestgiverAcceptQuestOpcode(accept);

            QuestStatus st = self->GetQuestStatus(questId);
            if (st == QUEST_STATUS_INCOMPLETE || st == QUEST_STATUS_COMPLETE)
            {
                static char const* const colourName[4] = { "серое", "зелёное", "жёлтое", "красное" };
                TC_LOG_INFO("server.worldserver", "Constellation: {} взял квест {} '{}' у {} — {}, ур. задания {} при своём {}",
                    self->GetName(), questId, quest->GetLogTitle(), giver->GetName(),
                    colourName[bestColour < 4 ? bestColour : 3], self->GetQuestLevel(quest), uint32(self->GetLevel()));
                ++_questsTaken;
                Constellation::Plan::Planner::Instance()->OnTakeOrTurnIn(self);
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
                c.TurnInPosFromTable = false;
                c.TurnInEntry = 0;              // 0 = сдать самому себе, никуда не идти
                c.TurnInPos = self->GetPosition();
                c.TurnInDist = 0.0f;
                return true;
            }

            bool anyEnder = false, summoned = false;
            for (auto const& [_, enderEntry] : sObjectMgr->GetCreatureQuestInvolvedRelationReverseBounds(qid))
            {
                anyEnder = true;            // принимающий В МИРЕ есть — приговора не будет
                if (!_spawnedSomewhere.count(enderEntry))
                    summoned = true;        // его нигде не ставят — значит призывают
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
                c.TurnInPosFromTable = true;
                c.TurnInDist = bestDist;
                return true;
            }

            // ПРИНИМАЮЩИЙ, КОТОРОГО ПРИЗЫВАЮТ, СТОИТ РЯДОМ, А НЕ НА ТОЧКЕ.
            //
            // Тариндрелла (49480) принимает «The Woodland Protector» у семерых ночных эльфов и
            // не имеет в мире НИ ОДНОЙ точки появления: её призывает Дентария заклинанием при
            // сдаче предыдущего квеста, а держит spell_area (аура 92237 на площади 257, пока
            // квест в журнале). Такой NPC ходит за игроком — и найти его можно только живым.
            // Обзор дорогой, поэтому: лишь для видов, которых нет в мире нигде, не чаще раза в
            // десять секунд, и с отсрочкой квесту, если рядом никого.
            if (summoned && !c.LiveEnderMs)
            {
                c.LiveEnderMs = 10000;
                std::list<Creature*> near;
                Trinity::AnyUnitInObjectRangeCheck check(self, 60.0f);
                Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, near, check);
                Cell::VisitGridObjects(self, searcher, 60.0f);
                Creature* live = nullptr;
                float bestLive = 100000.0f;
                for (Creature* cr : near)
                {
                    if (!cr->IsAlive())
                        continue;
                    // ЧУЖОЙ ЛИЧНЫЙ ПРИЗЫВ — НЕ НАШ ПРИНИМАЮЩИЙ. Такой NPC принадлежит другому
                    // игроку, и ядро всё равно не даст с ним говорить (WorldObject::_privateObjectOwner).
                    if (cr->IsPrivateObject() && cr->GetPrivateObjectOwner() != self->GetGUID())
                        continue;
                    bool mine = false;
                    for (auto const& [_, enderEntry] : sObjectMgr->GetCreatureQuestInvolvedRelationReverseBounds(qid))
                        if (cr->GetEntry() == enderEntry && !_spawnedSomewhere.count(enderEntry))
                            { mine = true; break; }
                    if (!mine)
                        continue;
                    float const d = self->GetExactDist(cr);
                    if (d < bestLive)
                        { bestLive = d; live = cr; }
                }
                if (live)
                {
                    c.TurnInQuest = qid;
                    c.TurnInEntry = live->GetEntry();
                    c.TurnInGuid = live->GetGUID();
                    c.TurnInPos = live->GetPosition();
                    c.TurnInPosFromTable = false;   // это ЖИВОЕ существо: высота у него своя (Кодекс)
                    c.TurnInDist = bestLive;
                    TC_LOG_INFO("server.worldserver",
                        "Constellation СДАЧА {}: принимающий {} ({}) квеста {} нигде не появляется — он призван и стоит в {:.0f} ярдах, иду к нему",
                        self->GetName(), live->GetName(), live->GetEntry(), qid, bestLive);
                    return true;
                }
                c.TurnInBackoff[qid] = 300000;      // призванного рядом нет — не искать каждый обзор
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
                // МОДУЛЬ НЕ БРОСАЕТ КВЕСТЫ — НИКАКИЕ (оператор, 2026-09-03).
                //
                // Здесь стоял единственный сброс: выполненный праздничный квест без принимающего.
                // Условие было из пяти пунктов и подходило во всей базе к 227 квестам — заглушкам
                // REUSE и <NYI>, подаркам праздников. Сработало оно за всё время РОВНО на одном,
                // 55660 'Time Trials', 97 раз, и каждый раз ядро возвращало квест обратно, потому
                // что раздаёт такие само (QUEST_FLAGS_EX_AUTO_PUSH, Player::PushQuests).
                //
                // После того как 55660 закрыли условием уровня в базе мира, живого случая у
                // правила не осталось — остался только теоретический вред: выполненный праздничный
                // квест, награду которого выдаёт сценарий, при сбросе потерял бы выполнение.
                // Мёртвый код, умеющий разрушать прогресс, хуже отсутствия кода.
                //
                // Вместо сброса — пометка ниже. Спутник перестаёт ходить к такому квесту, журнал
                // говорит почему, слот занят одним из двадцати пяти, и НИЧЕГО не разрушено.
                if (c.Impossible.insert(qid).second)
                    TC_LOG_INFO("server.worldserver", "Constellation: {} — квест {} '{}' закрыть нечем: {}",
                        self->GetName(), qid, quest->GetLogTitle(),
                        SeasonalKind(quest) ? "сезонный, вне своего события" : "ни принимающего, ни флага самосдачи");
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

        // КТО ВООБЩЕ ГДЕ-НИБУДЬ ПОЯВЛЯЕТСЯ. Нужен, чтобы отличить «принимающий на другой
        // карте» (подождём, дойдём) от «принимающего нет в мире ни одной точки» (его
        // призывают, и искать его надо живым рядом, а не по таблице).
        for (auto const& [mapId, byEntry] : _spawns)
            for (auto const& [entry, points] : byEntry)
                _spawnedSomewhere.insert(entry);

        // ЗАЧЁТ ЧЕРЕЗ ДРУГОЕ СУЩЕСТВО. Player::KilledMonsterCredit смотрит KillCredit[0..1]
        // шаблона убитого: цель квеста 39262 не появляется в мире никогда, её засчитывают
        // 39260 и 39261. Обратный указатель нужен для дороги к цели (у самой цели спавнов нет).
        uint32 credits = 0;
        for (auto const& [entry, tpl] : sObjectMgr->GetCreatureTemplates())
            for (uint32 i = 0; i < MAX_KILL_CREDIT; ++i)
                if (tpl.KillCredit[i])
                    { _creditedBy[tpl.KillCredit[i]].push_back(entry); ++credits; }
        TC_LOG_INFO("server.loading", "Constellation: указатель зачёта через других — {} связок у {} целей",
            credits, uint32(_creditedBy.size()));

        // ЗОНЫ ОСМОТРА. Цель «побывать в …» (тип 10) ядро засчитывает ТОЛЬКО по пакету
        // клиента CMSG_AREA_TRIGGER (HandleAreaTriggerOpcode), сверив IsInAreaTrigger; само
        // вхождение сервер не замечает. У спутника клиента нет — пакет шлёт модуль, когда
        // стоит внутри. Обратный указатель квест -> зоны, из тех же таблиц, что у ядра.
        // ТОЛЬКО ЧИСТЫЕ ЗОНЫ. Обработчик пакета делает не одну вещь: до зачёта он зовёт
        // сценарий зоны (areatrigger_scripts), после — телепорт (areatrigger_teleport) и
        // PvP-зоны. Зона, которая одновременно и квестовая, и телепортная, переместила бы
        // персонажа (Кодекс). Такие в указатель не попадают вовсе.
        uint32 t = 0, skipped = 0;
        for (AreaTriggerEntry const* at : sAreaTriggerStore)
            if (std::unordered_set<uint32> const* qs = sObjectMgr->GetQuestsForAreaTrigger(at->ID))
            {
                if (sObjectMgr->GetAreaTrigger(at->ID) || sObjectMgr->GetAreaTriggerScriptId(at->ID))
                    { ++skipped; continue; }
                for (uint32 q : *qs)
                    { _questTriggers[q].push_back(at); ++t; }
            }
        TC_LOG_INFO("server.loading", "Constellation: зоны осмотра — {} связок у {} квестов, отброшено телепортных/скриптовых {}",
            t, uint32(_questTriggers.size()), skipped);

        // ТОРГОВЦЫ И РЕМОНТНИКИ — ПО КАРТЕ. Обзор сетки на сто ярдов находил их только
        // тем, кто и так стоит рядом; трое людей 5-го уровня со сломанным оружием стояли
        // у шахты Фаргодип часами, а ремонтник гарнизона Вестбрук — в трёхстах ярдах.
        uint32 m = 0;
        for (auto const& [mapId, byEntry] : _spawns)
            for (auto const& [entry, points] : byEntry)
            {
                CreatureTemplate const* tpl = sObjectMgr->GetCreatureTemplate(entry);
                if (!tpl)
                    continue;
                bool const sells = (tpl->npcflag & uint64(UNIT_NPC_FLAG_VENDOR)) != 0;
                bool const fixes = (tpl->npcflag & uint64(UNIT_NPC_FLAG_REPAIR)) != 0;
                if (!sells && !fixes)
                    continue;
                for (Position const& pos : points)
                    { _menders[mapId].push_back(Mender{ entry, tpl->faction, pos, sells, fixes }); ++m; }
            }
        TC_LOG_INFO("server.loading", "Constellation: указатель торговцев — {} точек на {} картах",
            m, uint32(_menders.size()));

        // КВЕСТОДАТЕЛИ — ПО КАРТЕ. Обзор в тридцать ярдов находит только тех, кто и так рядом;
        // опустевший хаб оставлял спутника стоять при квестодателе в 52 ярдах (Теронис,
        // Аэлдон Санбранд). Связи «существо -> квесты» здесь выбирают КУДА ИДТИ; что взять,
        // по-прежнему решается у самого квестодателя по меню, собранному ядром.
        // ПОЛЁТНЫЕ МАСТЕРА — ПО ТОЧКАМ, С УЗЛОМ, КОТОРЫЙ ЯДРО ВЫВЕДЕТ ИЗ ИХ ПОЗИЦИИ. Номер узла
        // считаем ровно тем же вызовом, что и обработчик такси, и один раз при загрузке.
        uint32 fm = 0;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            CreatureTemplate const* tpl = sObjectMgr->GetCreatureTemplate(data.id);
            if (!tpl || !(tpl->npcflag & uint64(UNIT_NPC_FLAG_FLIGHTMASTER)))
                continue;
            _flightMasters[data.mapId].push_back(FlightPoint{ data.id, tpl->faction, data.spawnPoint });
            ++fm;
        }
        TC_LOG_INFO("server.loading", "Constellation: указатель полётных мастеров — {} точек на {} картах",
            fm, uint32(_flightMasters.size()));

        // ПО ТОЧКАМ СПАВНА, А НЕ ПО ВИДАМ (Кодекс): отсрочка после неудачи относится к одной
        // точке — плохая точка одного вида не должна запрещать другую того же вида.
        uint32 gq = 0;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            CreatureTemplate const* tpl = sObjectMgr->GetCreatureTemplate(data.id);
            if (!tpl || !(tpl->npcflag & uint64(UNIT_NPC_FLAG_QUESTGIVER)))
                continue;
            auto const rel = sObjectMgr->GetCreatureQuestRelations(data.id);
            if (rel.begin() == rel.end())
                continue;
            CreatureAddon const* addon = sObjectMgr->GetCreatureAddon(spawnId);
            _givers[data.mapId].push_back(Giver{ data.id, tpl->faction, data.spawnPoint, spawnId, addon ? addon->PathId : 0 });
            ++gq;
        }
        TC_LOG_INFO("server.loading", "Constellation: указатель квестодателей — {} точек на {} картах",
            gq, uint32(_givers.size()));


        // КАРТА — ЭТО ДАННЫЕ. МИР СПРАШИВАЮТ В МОМЕНТ КАСАНИЯ.
        //
        // Решение оператора, 2026-09-01: «зачем искать что собирать?? там простые условия —
        // квест, кого-то убил, у ресурсов руды и травы свои точки спавна», «да даже скан на
        // ближайших торговцев и квестодателей должен быть осознанным — если согласно карты
        // он тут должен быть».
        //
        // Прежняя версия обходила сетку вокруг каждого спутника, чтобы узнать то, что
        // записано в таблице и не меняется. Здесь это заменено указателем, который строится
        // один раз: предмет задания -> виды объектов, дающих его -> где эти объекты стоят.
        //
        // ХРАНИМ spawnId, А НЕ ОДНУ КООРДИНАТУ (Кодекс). По идентификатору спавна живой
        // объект берётся прямым обращением к карте, без единого обхода сетки; по одной лишь
        // координате пришлось бы снова искать перебором.
        //
        // И указатель — ПЛАНИРОВЩИК, А НЕ ИСТОЧНИК ПРАВДЫ. Кодекс перечислил, чего строка в
        // таблице не знает: в пуле из многих записей в мире живёт одна; фазы, личные спавны,
        // группы появления, игровые события, разные сложности; объекты, созданные сценарием
        // на лету, в таблице отсутствуют вовсе; а доступность сундука зависит ещё и от самого
        // игрока (ActivateToQuest). Поэтому правда берётся у мира — но ОДИН РАЗ, по приходу.
        uint32 g = 0, links = 0, locked = 0, noItems = 0, rejectedLogged = 0;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllGameObjectData())
        {
            std::vector<uint32> const* qi = sObjectMgr->GetGameObjectQuestItemList(data.id);
            if (!qi || qi->empty())
                { ++noItems; continue; }    // объект не даёт ни одного предмета задания
            GameObjectTemplate const* tpl = sObjectMgr->GetGameObjectTemplate(data.id);
            if (!tpl || !OpenableByHand(tpl))
            {
                // СОСТАВ ОТВЕРГНУТЫХ ЗАМКОВ — ЧТОБЫ ПРАВИЛО ВЫВЕСТИ, А НЕ УГАДАТЬ.
                //
                // Этот фильтр дважды за день выключил способность целиком. Замер: отсеяно
                // 7068 точек, и у каждого спутника с целью-предметом видов объектов НОЛЬ —
                // отсеяно ровно нужное. Печатаем первое условие замка простыми числами.
                //
                // Прошлая версия этой же строки роняла пробный мир: она собирала текст в
                // цикле через StringFormat прямо при загрузке. Ворота её и не пустили.
                // Здесь только числа и никакой сборки строк.
                if (tpl && rejectedLogged < 12 && sObjectMgr->GetGameObjectQuestItemList(data.id))
                {
                    uint32 const lockId = tpl->GetLockId();
                    if (LockEntry const* lock = lockId ? sLockStore.LookupEntry(lockId) : nullptr)
                    {
                        ++rejectedLogged;
                        TC_LOG_INFO("server.loading",
                            "Constellation ЗАМОК: вид {} замок {} — ключи {} {} {}, условия {} {} {}, навыки {} {} {}",
                            data.id, lockId,
                            uint32(lock->Type[0]), uint32(lock->Type[1]), uint32(lock->Type[2]),
                            lock->Index[0], lock->Index[1], lock->Index[2],
                            uint32(lock->Skill[0]), uint32(lock->Skill[1]), uint32(lock->Skill[2]));
                    }
                }
                ++locked; continue;         // заперто профессией/ключом — не наш случай
            }
            _goSpawns[data.mapId][data.id].push_back(GatherSpawn{ spawnId, data.id, data.spawnPoint,
                data.phaseUseFlags, uint16(data.phaseId), data.phaseGroup });
            for (uint32 item : *qi)
                { _itemFromGo[item].insert(data.id); ++links; }
            ++g;
        }
        TC_LOG_INFO("server.loading",
            "Constellation: указатель объектов сбора — {} точек на {} картах, {} связей предмет->объект; "
            "отсеяно замком {}, без предметов задания {}",
            g, uint32(_goSpawns.size()), links, locked, noItems);
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
        // ВЫБОР НАГРАДЫ — ОСМЫСЛЕННЫЙ (оператор): годное по классу и лучшее для слота, при
        // равенстве — дороже; ничего годного — самое дорогое, его продадут.
        LootItemType rewardType = LootItemType::Item;
        uint32 const reward = PickReward(self, quest, &rewardType);
        pick.Choice.Item.ItemID = reward;
        pick.Choice.LootItemType = rewardType;
        c.Session->HandleQuestgiverChooseRewardOpcode(pick);

        if (self->IsQuestRewarded(c.TurnInQuest))
        {
            TC_LOG_INFO("server.worldserver", "Constellation: {} сдал квест {} '{}' (уровень {}){}",
                self->GetName(), c.TurnInQuest, quest->GetLogTitle(), uint32(self->GetLevel()),
                reward ? Trinity::StringFormat(", выбрал награду {} из {}", reward, quest->GetRewChoiceItemsCount()) : "");
            ++_questsTurnedIn;
            Constellation::Plan::Planner::Instance()->OnTakeOrTurnIn(self);
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
        std::set<uint32> wanted, wantedItems, proxyOk;
        WantedEntries(self, wanted, nullptr, nullptr, nullptr, nullptr, &wantedItems, &proxyOk);
        if (wanted.count(entry))
            return true;
        // ПРОКСИ ВСЁ ЕЩЁ НУЖЕН (Кодекс): иначе бой с тем, кто засчитывает цель, обрывался бы
        // на первой же проверке как «цель набрана» — ещё до убийства.
        if (!proxyOk.empty())
            if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(entry))
                for (uint32 i = 0; i < MAX_KILL_CREDIT; ++i)
                    if (ct->KillCredit[i] && proxyOk.count(ct->KillCredit[i]))
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
        std::set<uint32>* wantedItems = nullptr, std::set<uint32>* proxyOk = nullptr) const
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
                {
                    wanted.insert(uint32(obj.ObjectID));
                    // ЗАЧЁТ ЧЕРЕЗ ДРУГОЕ СУЩЕСТВО ядро выдаёт не всегда: у квеста бывает флаг
                    // «без зачёта прокси» (Кодекс). Такие цели закрываются только прямо.
                    if (proxyOk && !quest->HasFlagEx(QUEST_FLAGS_EX_NO_CREDIT_FOR_PROXY))
                        proxyOk->insert(uint32(obj.ObjectID));
                }
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
                // ТРИ ВИДА ЦЕЛЕЙ, К КОТОРЫМ ЕСТЬ ДОРОГА: убить (номер существа), собрать
                // (номер предмета — метка квеста на карте показывает, где он падает) и
                // побывать (зона осмотра — её координаты знает таблица зон, тип 10).
                bool const isMonster = obj.Type == QUEST_OBJECTIVE_MONSTER && obj.ObjectID > 0;
                bool const isItem = obj.Type == QUEST_OBJECTIVE_ITEM && obj.ObjectID > 0;
                bool const isTrigger = obj.Type == QUEST_OBJECTIVE_AREATRIGGER;
                if (!isMonster && !isItem && !isTrigger)
                    continue;
                if (self->GetQuestObjectiveData(obj) >= std::max<int32>(obj.Amount, 1))
                    continue;

                Position dest;
                bool got = false;
                float stop = 10.0f;
                if (isTrigger)
                {
                    auto tit = _questTriggers.find(questId);
                    float bestT = 100000.0f;
                    if (tit != _questTriggers.end())
                        for (AreaTriggerEntry const* at : tit->second)
                        {
                            if (at->ContinentID != self->GetMapId())
                                continue;
                            float const dd = self->GetExactDist2d(at->Pos.X, at->Pos.Y);
                            if (dd < bestT)
                                { bestT = dd; dest.Relocate(at->Pos.X, at->Pos.Y, at->Pos.Z); got = true; }
                        }
                    stop = 3.0f;                // в самую зону, а не к её краю
                }
                else if (poi)
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
                if (!got && isMonster)
                    got = SpawnDestination(self, uint32(obj.ObjectID), &dest);
                if (!got && isMonster && !quest->HasFlagEx(QUEST_FLAGS_EX_NO_CREDIT_FOR_PROXY))
                {
                    // среди засчитывающих видов — БЛИЖАЙШИЙ спавн, а не первый по порядку (Кодекс)
                    auto cb = _creditedBy.find(uint32(obj.ObjectID));
                    if (cb != _creditedBy.end())
                    {
                        float nearest = 100000.0f;
                        for (uint32 crediting : cb->second)
                        {
                            Position cand;
                            if (!SpawnDestination(self, crediting, &cand))
                                continue;
                            float const dd = self->GetExactDist2d(cand.GetPositionX(), cand.GetPositionY());
                            if (dd < nearest)
                                { nearest = dd; dest = cand; got = true; }
                        }
                    }
                }
                if (!got)
                    continue;
                float d = self->GetExactDist2d(dest.GetPositionX(), dest.GetPositionY());
                if (d < bestDist)
                    { bestDist = d; best = dest; found = true; c.TravelQuest = questId; c.TravelStop = stop; }
            }
        }
        // ближе FightRange идти незачем: там цель и так увидит обычный поиск
        if (!found || bestDist < Cfg().FightRange)
            return false;
        c.TravelPos = best;
        return true;
    }

    // НАДЕТЬ ЛУЧШЕЕ ИЗ ТОГО, ЧТО ЛЕЖИТ В СУМКАХ. Задача 0009, часть А, по журналу Легиона.
    //
    // Там это записано как рецепт, который строится первым и без внешних данных: для
    // каждого слота обойти сумки и взять лучший предмет, у которого ядро само сказало
    // «можно» (CanUseItem), который не сломан и уровнем выше надетого. Спутник до этого
    // собирал добычу и не надевал ничего — поднимался с 5 на 7 в стартовом тряпье.
    //
    // И там же урок, из-за которого сравнивать ОДИН уровень предмета нельзя: на Легионе
    // так одели рогов и хантеров в оружие не того типа, и умения отказали —
    // SPELL_FAILED_EQUIPPED_ITEM_CLASS. Поэтому занятый оружейный слот меняем только на
    // оружие того же вида (тип в инвентаре и подкласс совпадают): меч остаётся мечом.
    // lazy: настоящая проверка — Item::IsFitToSpellRequirements против выбранного
    // умения из палитры; это часть Б, вместе с ролью и веткой талантов.
    //
    // Пустой слот заполняем чем угодно пригодным: что угодно лучше, чем ничего.
    // Одна обновка за проход — проход дешёвый, но незачем делать его тяжёлым.
    bool EquipUpgrades(Companion& c, Player* self)
    {
        // ОРУЖИЕ СМЕНИЛОСЬ — ОТКАЗЫ УСТАРЕЛИ (Кодекс). Главный из них, «двуручное в руках»,
        // снимается ровно этим; ждать десяти минут после смены оружия незачем.
        Item* const inHand = self->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        ObjectGuid const nowHand = inHand ? inHand->GetGUID() : ObjectGuid::Empty;
        if (nowHand != c.WeaponWas)
        {
            c.WeaponWas = nowHand;
            c.EquipRefused.clear();
        }

        auto consider = [&](Item* item) -> bool
        {
            if (!item || item->IsBroken())
                return false;
            if (item->GetTemplate()->GetInventoryType() == INVTYPE_BAG)
                return false;           // сумку меряют вместимостью, а не уровнем (Кодекс)
            if (self->CanUseItem(item) != EQUIP_ERR_OK)
                return false;
            uint8 dst = self->FindEquipSlot(item, NULL_SLOT, true);
            if (dst == NULL_SLOT)
                return false;

            Item* worn = self->GetItemByPos(INVENTORY_SLOT_BAG_0, dst);
            if (worn)
            {
                if (item->GetItemLevel(self) <= worn->GetItemLevel(self))
                    return false;
                bool const weaponSlot = dst == EQUIPMENT_SLOT_MAINHAND
                    || dst == EQUIPMENT_SLOT_OFFHAND || dst == EQUIPMENT_SLOT_RANGED;
                if (weaponSlot
                    && (item->GetTemplate()->GetInventoryType() != worn->GetTemplate()->GetInventoryType()
                        || item->GetTemplate()->GetSubClass() != worn->GetTemplate()->GetSubClass()))
                    return false;           // меч остаётся мечом (урок Легиона)
            }

            uint32 const wasLevel = worn ? worn->GetItemLevel(self) : 0;
            uint32 const entry = item->GetEntry();
            uint8 const bagSlot = item->GetBagSlot(), slot = item->GetSlot();

            // ТОТ ЖЕ ВОПРОС, ЧТО ЗАДАЁТ ОБРАБОТЧИК, — ДО ОТПРАВКИ. «Можно использовать» и
            // «можно надеть сюда» — разные вопросы: маг с посохом использовать щит может, а
            // надеть при двуручном — нет. Замер: 38 отказов подряд по одному предмету у двух
            // магов, каждые тридцать секунд. Отказ запоминаем по паре предмет+слот.
            // ПАМЯТЬ ОТКАЗОВ КОРОТКАЯ И ПО GUID (Кодекс): отказ «двуручное в руках» снимается
            // сменой оружия, навык выучивается, место освобождается — всё без нового уровня.
            // Десять минут, по паре предмет+слот; временные коды (бой, оглушение, каст) не
            // запоминаются вовсе — это состояние персонажа, а не свойство предмета.
            {
                auto known = c.EquipRefused.find({ item->GetGUID(), dst });
                if (known != c.EquipRefused.end())
                    return false;
            }
            uint16 dest = 0;
            InventoryResult const can = self->CanEquipItem(NULL_SLOT, dest, item, true);
            if (can != EQUIP_ERR_OK)
            {
                // ВРЕМЕННОЕ — ЭТО СОСТОЯНИЕ, А НЕ СВОЙСТВО ПРЕДМЕТА (Кодекс): бой, каст,
                // оглушение, откат оружия, полные сумки — и «двуручное в руках», которое
                // снимается сменой оружия, а не уровнем. Такое не запоминаем вовсе.
                // «ДВУРУЧНОЕ В РУКАХ» ВЫНЕСЕНО ИЗ ВРЕМЕННЫХ — ЗАМЕР ЗАСТАВИЛ. За восемь часов
                // 1226 строк надевания, из них 1225 — этот самый код по ОДНОМУ предмету у двух
                // магов с посохами. Он действительно снимается сменой оружия, но держится
                // ровно столько, сколько носится посох, то есть часами: как «временное» он
                // означал попытку каждые тридцать секунд без единого шанса. Помним десять
                // минут, как прочие отказы, — сменивший оружие подождёт их и наденет.
                bool const transient = can == EQUIP_ERR_NOT_WHILE_DISARMED || can == EQUIP_ERR_CLIENT_LOCKED_OUT
                    || can == EQUIP_ERR_NOT_DURING_ARENA_MATCH || can == EQUIP_ERR_GENERIC_STUNNED
                    || can == EQUIP_ERR_NOT_IN_COMBAT || can == EQUIP_ERR_ITEM_COOLDOWN
                    || can == EQUIP_ERR_INV_FULL;
                if (!transient)
                    c.EquipRefused[{ item->GetGUID(), dst }] = 600000;
                TC_LOG_INFO("server.worldserver",
                    "Constellation НАДЕЛ {}: {} в слот {} не надеть — ядро говорит {}{}",
                    self->GetName(), entry, uint32(dst), uint32(can),
                    transient ? " (временно)" : " (десять минут не пробую)");
                return false;
            }
            // СЛОТ — ИЗ ОТВЕТА ЯДРА, а не из своего вопроса: именно dest затем использует
            // обработчик (Кодекс). Две истины рядом разойдутся на кольцах и аксессуарах.
            dst = uint8(dest & 0xFF);

            // ТЕМ ЖЕ ОПКОДОМ, ЧТО КЛИК ПРАВОЙ КНОПКОЙ. Обработчик требует РОВНО одну запись
            // в Inv.Items — иначе отвергает (ItemHandler.cpp: Inv.Items.size() != 1).
            WorldPacket raw(CMSG_AUTO_EQUIP_ITEM);
            WorldPackets::Item::AutoEquipItem eq(std::move(raw));
            eq.PackSlot = bagSlot;
            eq.Slot = slot;
            eq.Inv.Items.push_back({ bagSlot, slot });
            c.Session->HandleAutoEquipItemOpcode(eq);

            // ПРАВДА ИЗ СОСТОЯНИЯ: лежит ли теперь в слоте именно этот предмет
            Item* now = self->GetItemByPos(INVENTORY_SLOT_BAG_0, dst);
            if (now && now->GetEntry() == entry)
            {
                ++c.Equipped;
                TC_LOG_INFO("server.worldserver",
                    "Constellation НАДЕЛ {}: {} (ур. {}) в слот {} вместо ур. {}; всего надето {}",
                    self->GetName(), entry, now->GetItemLevel(self), uint32(dst), wasLevel, c.Equipped);
                return true;
            }
            TC_LOG_INFO("server.worldserver",
                "Constellation НАДЕЛ {}: {} в слот {} не надет — ядро отказало ({})",
                self->GetName(), entry, uint32(dst), uint32(self->CanUseItem(item)));
            return false;
        };

        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            if (consider(self->GetItemByPos(INVENTORY_SLOT_BAG_0, i)))
                return true;
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = self->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    if (consider(bag->GetItemByPos(j)))
                        return true;
        return false;
    }

    // СУМКУ — В ПУСТОЙ СЛОТ (оператор: «надевать более вместительные»). Тем же опкодом, что и
    // любую вещь; обработчик для сумок разрешает только пустой слот (swap = !IsBag), поэтому
    // замена меньшей сумки на большую — отдельная работа: её надо сперва опустошить
    // (задача 0023, п. 10). Одна сумка за проход, правда — из состояния слота.
    bool EquipBags(Companion& c, Player* self)
    {
        uint8 emptySlot = NULL_SLOT;
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (!self->GetBagByPos(b))
                { emptySlot = b; break; }
        if (emptySlot == NULL_SLOT)
            return false;
        // САМАЯ ВМЕСТИТЕЛЬНАЯ, А НЕ ПЕРВАЯ ПОПАВШАЯСЯ (Кодекс): иначе в слот идёт маленькая,
        // а большая потом ждёт замены, которую ещё надо уметь.
        Item* best = nullptr;
        auto pick = [&](Item* it)
        {
            if (!it || !it->IsBag())
                return;
            if (!best || it->GetTemplate()->GetContainerSlots() > best->GetTemplate()->GetContainerSlots())
                best = it;
        };
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            pick(self->GetItemByPos(INVENTORY_SLOT_BAG_0, i));
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = self->GetBagByPos(b))
                for (uint32 j = 0; j < GetBagSize(bag); ++j)
                    pick(GetItemInBag(bag, j));
        auto tryBag = [&](Item* it) -> bool
        {
            if (!it || !it->IsBag())
                return false;
            uint32 const entry = it->GetEntry();
            uint8 const bagSlot = it->GetBagSlot(), slot = it->GetSlot();
            WorldPacket raw(CMSG_AUTO_EQUIP_ITEM);
            WorldPackets::Item::AutoEquipItem eq(std::move(raw));
            eq.PackSlot = bagSlot;
            eq.Slot = slot;
            eq.Inv.Items.push_back({ bagSlot, slot });
            c.Session->HandleAutoEquipItemOpcode(eq);
            Item* now = self->GetItemByPos(INVENTORY_SLOT_BAG_0, emptySlot);
            if (now && now->GetEntry() == entry)
            {
                TC_LOG_INFO("server.worldserver",
                    "Constellation СУМКА {}: надел сумку {} на {} ячеек в слот {}",
                    self->GetName(), entry, it->GetTemplate()->GetContainerSlots(), uint32(emptySlot));
                return true;
            }
            return false;
        };
        return tryBag(best);
    }

    // ПРЕДМЕТ, КОТОРЫЙ ВЫДАЛ САМ КВЕСТ, И ЕГО ЗАКЛИНАНИЕ ПРИМЕНЕНИЯ.
    //
    // Легион, задача 0012: «неразличимы по всем свойствам, на которые можно фильтровать» —
    // ведро для тушения, камень возвращения и вяленое мясо дают одинаковые нули по цели и
    // дальности. Поэтому источник истины один: квест, в котором эта цель числится, и его
    // собственный SourceItem. Тогда применить камень возвращения к костру становится
    // невозможно ПО ПОСТРОЕНИЮ, а не по удачно подобранному условию.
    // РОВНО ОДНО ЗАКЛИНАНИЕ «ПРИ ИСПОЛЬЗОВАНИИ». Ядро не выбирает первое: оно лишь проверяет,
    // что присланный клиентом номер есть среди эффектов предмета (Player::CastItemUseSpell),
    // а выбор делает клиент. У предмета с двумя такими эффектами модулю выбирать не из
    // чего — такой пропускаем (Кодекс). 0 — «не одно».
    static uint32 UseSpellOf(Item const* tool)
    {
        uint32 useSpell = 0, useCount = 0;
        for (ItemEffectEntry const* eff : tool->GetEffects())
            if (eff && eff->TriggerType == ITEM_SPELLTRIGGER_ON_USE && eff->SpellID > 0
                && uint32(eff->SpellID) != useSpell)
                { useSpell = uint32(eff->SpellID); ++useCount; }
        return useCount == 1 ? useSpell : 0;
    }

    Item* QuestToolFor(Player* self, uint32 targetEntry, uint32* spellOut,
                       Creature const* who = nullptr, uint32* questOut = nullptr,
                       std::set<uint32>* creditsOut = nullptr) const
    {
        if (questOut)
            *questOut = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const qid = self->GetQuestSlotQuestId(slot);
            if (!qid || self->GetQuestStatus(qid) != QUEST_STATUS_INCOMPLETE)
                continue;
            Quest const* q = sObjectMgr->GetQuestTemplate(qid);
            if (!q || !q->GetSrcItemId())
                continue;
            bool mine = false;
            for (QuestObjective const& obj : q->GetObjectives())
                if (obj.Type == QUEST_OBJECTIVE_MONSTER && uint32(obj.ObjectID) == targetEntry
                    && self->GetQuestObjectiveData(qid, obj.ID) < obj.Amount)
                    { mine = true; break; }
            if (!mine)
                continue;
            Item* tool = self->GetItemByEntry(q->GetSrcItemId());
            if (!tool)
                continue;
            if (uint32 const useSpell = UseSpellOf(tool))
                { *spellOut = useSpell; return tool; }
        }
        if (!who)
            return nullptr;

        // ПО КОНТРАКТУ ЗАКЛИНАНИЯ ПРЕДМЕТА. Цель задания бывает невидимым маркером зачёта,
        // который не появляется в мире никогда: «Kill Credit Bunny - Wounded Coldridge…» (37079)
        // у девяти дварфов с «Aid for the Wounded». Зачёт даёт заклинание предмета от квеста
        // (эффект KILL_CREDIT с номером маркера — Spell::EffectKillCredit награждает игрока), а
        // КОГО им лечить, названо в условиях самого заклинания: conditions 13/69855 -> существо
        // 37080, раненый горец, двадцать точек в долине. Ровно эти два вопроса и задаём: даёт
        // ли заклинание зачёт нужной цели, и назван ли этот вид его целью.
        Difficulty const diff = self->GetMap()->GetDifficultyID();
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const qid = self->GetQuestSlotQuestId(slot);
            if (!qid || self->GetQuestStatus(qid) != QUEST_STATUS_INCOMPLETE)
                continue;
            Quest const* q = sObjectMgr->GetQuestTemplate(qid);
            if (!q || !q->GetSrcItemId())
                continue;
            Item* tool = self->GetItemByEntry(q->GetSrcItemId());
            if (!tool)
                continue;
            uint32 const useSpell = UseSpellOf(tool);
            if (!useSpell)
                continue;
            SpellInfo const* si = sSpellMgr->GetSpellInfo(useSpell, diff);
            if (!si || SpellMovesOrControls(si, diff))
                continue;                       // предмет, который переносит или управляет, — не инструмент
            std::set<uint32> unmet;
            for (QuestObjective const& obj : q->GetObjectives())
                if (obj.Type == QUEST_OBJECTIVE_MONSTER && obj.ObjectID > 0
                    && self->GetQuestObjectiveData(qid, obj.ID) < obj.Amount)
                    unmet.insert(uint32(obj.ObjectID));
            if (unmet.empty())
                continue;
            // ПУТЬ ЧЕРЕЗ ПРАВИЛО СУЩЕСТВА («попадание -> зачёт») здесь СОЗНАТЕЛЬНО НЕ ВКЛЮЧЁН
            // (Кодекс): это другой контракт, и известный пример — тотем троллей (25165) —
            // требует ещё и боя; одним применением его не закрыть.
            std::set<uint32> credits;
            if (!SpellContractFits(si, who->GetEntry(), unmet, &credits))
                continue;
            if (creditsOut)
                *creditsOut = credits;
            *spellOut = useSpell;
            if (questOut)
                *questOut = qid;
            return tool;
        }
        return nullptr;
    }

    // ЕСТЬ ЛИ ВООБЩЕ ЧЕМ РАБОТАТЬ — дешёвый предварительный вопрос перед обходом существ.
    bool AnyToolQuest(Player* self) const
    {
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const qid = self->GetQuestSlotQuestId(slot);
            if (!qid || self->GetQuestStatus(qid) != QUEST_STATUS_INCOMPLETE)
                continue;
            Quest const* q = sObjectMgr->GetQuestTemplate(qid);
            if (q && q->GetSrcItemId() && self->GetItemByEntry(q->GetSrcItemId()))
                return true;
        }
        return false;
    }

    // СНИМОК ПО ЗАСЧИТЫВАЕМЫМ ЦЕЛЯМ КВЕСТА — для предмета, выбранного по контракту заклинания:
    // засчитывается маркер, а не тот, на кого применили; и именно те цели, чьи номера
    // заклинание засчитывает, а не все цели квеста подряд (Кодекс).
    void SnapshotQuestObjectives(Player* self, uint32 qid, std::set<uint32> const& credits,
        std::vector<std::pair<std::pair<uint32, uint32>, int32>>& out) const
    {
        out.clear();
        if (Quest const* q = sObjectMgr->GetQuestTemplate(qid))
            for (QuestObjective const& obj : q->GetObjectives())
                if (obj.Type == QUEST_OBJECTIVE_MONSTER && obj.ObjectID > 0 && credits.count(uint32(obj.ObjectID)))
                    out.push_back({ { qid, obj.ID }, self->GetQuestObjectiveData(qid, obj.ID) });
    }

    // СЧЁТЧИКИ ЦЕЛЕЙ ПО ЭТОМУ СУЩЕСТВУ — ДО ПОПЫТКИ. Успех меряется их ростом.
    void SnapshotObjectives(Player* self, uint32 entry,
        std::vector<std::pair<std::pair<uint32, uint32>, int32>>& out) const
    {
        out.clear();
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const qid = self->GetQuestSlotQuestId(slot);
            if (!qid || self->GetQuestStatus(qid) != QUEST_STATUS_INCOMPLETE)
                continue;
            if (Quest const* q = sObjectMgr->GetQuestTemplate(qid))
                for (QuestObjective const& obj : q->GetObjectives())
                    if (obj.Type == QUEST_OBJECTIVE_MONSTER && uint32(obj.ObjectID) == entry)
                        out.push_back({ { qid, obj.ID }, self->GetQuestObjectiveData(qid, obj.ID) });
        }
    }

    // ПЕРЕМЕЩАЕТ ЛИ ИЛИ ПОДЧИНЯЕТ ЗАКЛИНАНИЕ — по его эффектам, на один уровень вызова вглубь.
    static bool SpellMovesOrControls(SpellInfo const* si, Difficulty diff, int depth = 2)
    {
        if (!si)
            return false;
        for (SpellEffectInfo const& eff : si->GetEffects())
        {
            switch (eff.Effect)
            {
                case SPELL_EFFECT_INSTAKILL:
                case SPELL_EFFECT_TELEPORT_UNITS:
                case SPELL_EFFECT_TELEPORT_UNITS_FACE_CASTER:
                case SPELL_EFFECT_SUMMON_PLAYER:
                    return true;
                default:
                    break;
            }
            switch (eff.ApplyAuraName)
            {
                case SPELL_AURA_CONTROL_VEHICLE:
                case SPELL_AURA_MOD_CHARM:
                case SPELL_AURA_MOD_POSSESS:
                case SPELL_AURA_MOD_POSSESS_PET:
                case SPELL_AURA_AOE_CHARM:
                    return true;
                default:
                    break;
            }
            if (depth > 0 && eff.TriggerSpell
                && SpellMovesOrControls(sSpellMgr->GetSpellInfo(eff.TriggerSpell, diff), diff, depth - 1))
                return true;
        }
        return false;
    }

    // БЕЗОПАСЕН ЛИ КЛИК ПО ЭТОМУ ВИДУ — ПО ДАННЫМ, А НЕ ПО ВЕРЕ. Кодекс: HandleSpellClick
    // исполняет ВСЕ подходящие строки npc_spellclick_spells, а это бывает вход в транспорт,
    // подчинение, телепорт. Два условия, оба из таблиц: (1) ни одно заклинание клика, включая
    // вызываемые им, не перемещает и не подчиняет; (2) у существа есть правило «попадание
    // заклинанием -> зачёт убийства» (SmartAI: SPELLHIT, затем по цепочке link —
    // CALL_KILLEDMONSTER), то есть клик и есть задуманный путь зачёта. Раненый пехотинец
    // (50047): клик -> 93072, правило SPELLHIT 93097 -> LINK -> CALL_KILLEDMONSTER 50047.
    // Зачёт должен ложиться на вид, который спутнику НУЖЕН (цель незакрытого задания), а не
    // на какой угодно. Связь «заклинание клика -> заклинание попадания» проверяется, когда она
    // видна в данных (само заклинание или вызываемое им), и только ОТМЕЧАЕТСЯ, когда не видна:
    // у раненого пехотинца клик 93072 «Get Our Boys Back Dummy» произносит 93097 из
    // сценария на C++ (spell_quest.cpp), и никакая таблица этого не покажет. Остаток риска —
    // клик, который ничего не даёт, — закрывает предохранитель по окнам.
    bool ClickGivesCredit(uint32 entry, std::set<uint32> const& wanted, Player* self,
                          uint32* castMs, bool* tied) const
    {
        Difficulty const diff = self->GetMap()->GetDifficultyID();
        std::set<uint32> hitSpells;
        for (auto const& pair : sObjectMgr->GetSpellClickInfoMapBounds(entry))
        {
            SpellInfo const* si = sSpellMgr->GetSpellInfo(pair.second.spellId, diff);
            if (!si || SpellMovesOrControls(si, diff))
                return false;
            hitSpells.insert(si->Id);
            for (SpellEffectInfo const& eff : si->GetEffects())
                if (eff.TriggerSpell)
                    hitSpells.insert(eff.TriggerSpell);
            if (castMs)
                *castMs = std::max(*castMs, si->CalcCastTime());
        }
        if (hitSpells.empty())
            return false;
        bool direct = false;
        bool const found = HitChainCredits(entry, hitSpells, wanted, &direct);
        if (tied)
            *tied = direct;
        if (!found)
            return false;
        if (direct)
            return true;
        // СВЯЗЬ НЕ ВИДНА В ДАННЫХ — ТОЛЬКО ПО ЯВНОМУ СПИСКУ (Кодекс, третий проход). Правило
        // «попадание -> зачёт» у существа есть, но заклинание попадания не совпадает ни с
        // заклинанием клика, ни с вызываемыми им: связь живёт в сценарии на C++. Такие
        // виды допускаются поимённо, с указанием, где связь прочитана. Всё прочее — «не трогаю».
        static std::set<uint32> const opaqueClickAllow = {
            50047,  // Injured Stormwind Infantry, «Fear No Evil»: клик 93072 «Get Our Boys Back
                    // Dummy» -> spell_quest.cpp: OnCast -> 93097 «Renewed Life» на себя ->
                    // SmartAI SPELLHIT 93097 -> CALL_KILLEDMONSTER 50047 создателю
        };
        return opaqueClickAllow.count(entry) != 0;
    }

    // ПРОДВИЖЕНИЕ, ЗАМЕЧЕННОЕ ПОСЛЕ ОКНА. Снимок хранится до следующей сверки — на каждом
    // проходе «стою» и перед каждой новой попыткой. Рост счётчика после окна НЕ доказывает,
    // что это наше применение (ту же цель мог убить кто-то другой), поэтому зачётом не
    // считается; но предохранители сбрасывает: ложное «бесплодно» хуже неучтённого успеха
    // (Кодекс, третий проход).
    bool ReconcileLateCredit(Companion& c, Player* self) const
    {
        if (c.ToolWas.empty())
            return false;
        bool grew = false;
        for (auto const& [key, before] : c.ToolWas)
            if (self->GetQuestObjectiveData(key.first, key.second) > before)
                { grew = true; break; }
        if (!grew)
            return false;
        c.ToolWas.clear();
        c.ToolFruitless = 0;
        c.ToolGiveUps = 0;
        c.ToolActionFruitless = 0;
        c.ToolActionMs = 0;
        TC_LOG_INFO("server.worldserver",
            "Constellation ПРИМЕНЕНИЕ {}: по виду {} продвижение после окна — причина не доказана, предохранители сброшены",
            self->GetName(), c.ToolWasEntry);
        return true;
    }

    // ВОЙТИ В ЗОНУ ОСМОТРА — ЭТО ПАКЕТ, А НЕ ФАКТ. Ядро проверит IsInAreaTrigger само и
    // откажет, если мы снаружи; повторно в ту же зону не шлём минуту.
    void TouchAreaTriggers(Companion& c, Player* self) const
    {
        if (_questTriggers.empty() || !self->IsAlive() || self->GetOutdoorPvP())
            return;                     // в PvP-зоне пакет зоны идёт в её обработчик — не наш случай
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const qid = self->GetQuestSlotQuestId(slot);
            if (!qid || self->GetQuestStatus(qid) != QUEST_STATUS_INCOMPLETE)
                continue;
            auto it = _questTriggers.find(qid);
            if (it == _questTriggers.end())
                continue;
            Quest const* q = sObjectMgr->GetQuestTemplate(qid);
            if (!q)
                continue;
            bool unmet = false;
            for (QuestObjective const& obj : q->GetObjectives())
                if (obj.Type == QUEST_OBJECTIVE_AREATRIGGER && self->GetQuestObjectiveData(obj) < 1)
                    { unmet = true; break; }
            if (!unmet)
                continue;
            for (AreaTriggerEntry const* at : it->second)
            {
                if (at->ContinentID != self->GetMapId() || c.TriggerSentMs.count(at->ID))
                    continue;
                if (!self->IsInAreaTrigger(at))
                    continue;
                c.TriggerSentMs[at->ID] = 60000;
                WorldPacket raw(CMSG_AREA_TRIGGER);
                WorldPackets::AreaTrigger::AreaTrigger pkt(std::move(raw));
                pkt.AreaTriggerID = int32(at->ID);
                pkt.Entered = true;
                pkt.FromClient = true;
                c.Session->HandleAreaTriggerOpcode(pkt);
                bool done = true;
                for (QuestObjective const& obj : q->GetObjectives())
                    if (obj.Type == QUEST_OBJECTIVE_AREATRIGGER && self->GetQuestObjectiveData(obj) < 1)
                        { done = false; break; }
                TC_LOG_INFO("server.worldserver",
                    "Constellation ОСМОТР {}: вошёл в зону {} квеста {} '{}' — {}",
                    self->GetName(), at->ID, qid, q->GetLogTitle(), done ? "зачёт" : "без зачёта");
            }
        }
    }

    // БЛИЖАЙШИЙ ТОРГОВЕЦ ПО КАРТЕ, умеющий нужное и не враждебный по своей фракции.
    // ПРАВИЛО СУЩЕСТВА «ПОПАДАНИЕ -> ЗАЧЁТ», прослеженное по связям и спискам действий. Общее
    // для клика и для предмета: hitSpells — чем в него попадут; tied — совпало ли заклинание
    // правила с одним из них (иначе связь живёт в сценарии на C++ и в данных не видна).
    bool HitChainCredits(uint32 entry, std::set<uint32> const& hitSpells, std::set<uint32> const& wanted, bool* tied) const
    {
        auto const script = sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE);
        std::unordered_map<uint32, SmartScriptHolder const*> byId;
        for (SmartScriptHolder const& e : script)
            byId[e.event_id] = &e;
        // ЗАЧЁТ ЧАСТО ЛЕЖИТ НЕ В САМОМ ПРАВИЛЕ, А В ЕГО СПИСКЕ ДЕЙСТВИЙ. Замер на живом:
        // непокорный тролль (34830) — «попадание 66306 -> ВЫПОЛНИТЬ СПИСОК 3483000», и уже в
        // списке, пунктом четвёртым, «выдать зачёт 34830». Семерым гоблинам отказали именно
        // потому, что цепочка обрывалась на границе списка.
        auto listGives = [&](uint32 listId) -> bool
        {
            if (!listId)
                return false;
            for (SmartScriptHolder const& a : sSmartScriptMgr->GetScript(int32(listId), SMART_SCRIPT_TYPE_TIMED_ACTIONLIST))
                if (a.GetActionType() == SMART_ACTION_CALL_KILLEDMONSTER
                    && wanted.count(a.action.killedMonster.creature))
                    return true;
            return false;
        };
        bool found = false, direct = false;
        for (SmartScriptHolder const& e : script)
        {
            if (e.GetEventType() != SMART_EVENT_SPELLHIT)
                continue;
            bool const spellTied = e.event.spellHit.spell == 0 || hitSpells.count(e.event.spellHit.spell) != 0;
            SmartScriptHolder const* cur = &e;
            for (int hop = 0; cur && hop < 8; ++hop)
            {
                bool gives = cur->GetActionType() == SMART_ACTION_CALL_KILLEDMONSTER
                    && wanted.count(cur->action.killedMonster.creature);
                if (!gives && cur->GetActionType() == SMART_ACTION_CALL_TIMED_ACTIONLIST)
                    gives = listGives(cur->action.timedActionList.id);
                if (!gives && cur->GetActionType() == SMART_ACTION_CALL_RANDOM_TIMED_ACTIONLIST)
                    for (uint32 listId : cur->action.randTimedActionList.actionLists)
                        if (listGives(listId))
                            { gives = true; break; }
                if (gives)
                {
                    found = true;
                    direct = direct || spellTied;
                    break;
                }
                if (!cur->link)
                    break;
                auto it = byId.find(cur->link);
                cur = (it == byId.end()) ? nullptr : it->second;
            }
        }
        if (tied)
            *tied = direct;
        return found;
    }

    // ЗАПИСЬ «ЦЕЛЬ — СУЩЕСТВО НОМЕР N» — в двух видах, старом и нынешнем.
    static bool UnitEntryCondition(Condition const& cond, uint32 entry)
    {
        if (cond.NegativeCondition || cond.ConditionTarget != 0)
            return false;                   // «НЕ такая-то» — не список; о заклинателе — не о цели
        bool const legacy = cond.ConditionType == CONDITION_OBJECT_ENTRY_GUID_LEGACY && cond.ConditionValue1 == 3;
        bool const modern = cond.ConditionType == CONDITION_OBJECT_ENTRY_GUID && cond.ConditionValue1 == TYPEID_UNIT;
        return (legacy || modern) && (!entry || cond.ConditionValue2 == entry);
    }

    // ОДНО СОВПАВШЕЕ УСЛОВИЕ — НЕ СПИСОК РАЗРЕШЁННЫХ (Кодекс). Условия эффекта разложены по
    // группам ElseGroup: внутри группы они соединены И, между группами — ИЛИ. Значит запись
    // «цель — существо N» годится, только если в её ЖЕ группе нет такой же записи о ДРУГОМ
    // существе: тогда группа для нашего существа невыполнима, и ядро цель отвергнет.
    static bool GroupNamesTarget(ConditionContainer const& conds, uint32 entry)
    {
        for (Condition const& cond : conds)
        {
            if (!UnitEntryCondition(cond, entry))
                continue;
            bool clash = false;
            for (Condition const& other : conds)
                if (other.ElseGroup == cond.ElseGroup && UnitEntryCondition(other, 0)
                    && other.ConditionValue2 != entry)
                    { clash = true; break; }
            if (!clash)
                return true;
        }
        return false;
    }

    // КОНТРАКТ ЦЕЛИ И ЗАЧЁТ — ИЗ ОДНОГО МЕСТА (Кодекс, вторая проверка).
    //
    // Прежняя редакция собирала зачёты по всем эффектам и всем вызываемым заклинаниям, а
    // условия на цель — отдельно и так же широко. Тогда зачёт ОДНОГО эффекта сходился бы с
    // условием ЧУЖОГО, и предмет применялся бы не к тому существу. Правило теперь связное:
    // эффект называет наше существо целью, а зачёт нужному маркеру стоит либо в ТОМ ЖЕ
    // заклинании (обычный случай: один эффект бьёт по существу, другой награждает игрока —
    // Spell::EffectKillCredit требует, чтобы целью был игрок), либо в том заклинании, которое
    // ЭТОТ ЖЕ эффект вызывает, — то есть ровно в ветви, которой цель и передаётся.
    void CreditsOf(SpellInfo const* si, std::set<uint32> const& unmet, std::set<uint32>& out) const
    {
        for (SpellEffectInfo const& eff : si->GetEffects())
            if (eff.IsEffect(SPELL_EFFECT_KILL_CREDIT) || eff.IsEffect(SPELL_EFFECT_KILL_CREDIT2))
                if (eff.MiscValue > 0 && unmet.count(uint32(eff.MiscValue)))
                    out.insert(uint32(eff.MiscValue));
    }

    bool SpellContractFits(SpellInfo const* si, uint32 entry, std::set<uint32> const& unmet,
                           std::set<uint32>* creditsOut, uint32 depth = 0) const
    {
        std::set<uint32> own;
        CreditsOf(si, unmet, own);
        for (SpellEffectInfo const& eff : si->GetEffects())
        {
            if (!eff.ImplicitTargetConditions || !GroupNamesTarget(*eff.ImplicitTargetConditions, entry))
                continue;
            std::set<uint32> credits = own;
            if (eff.TriggerSpell)
                if (SpellInfo const* t = sSpellMgr->GetSpellInfo(eff.TriggerSpell, DIFFICULTY_NONE))
                    CreditsOf(t, unmet, credits);
            if (!credits.empty())
            {
                if (creditsOut)
                    *creditsOut = credits;
                return true;
            }
        }
        // вызываемое заклинание может нести и условие, и зачёт — тогда оно само себе контракт
        if (depth < 1)
            for (SpellEffectInfo const& eff : si->GetEffects())
                if (eff.TriggerSpell)
                    if (SpellInfo const* t = sSpellMgr->GetSpellInfo(eff.TriggerSpell, DIFFICULTY_NONE))
                        if (SpellContractFits(t, entry, unmet, creditsOut, depth + 1))
                            return true;
        return false;
    }

    // ---------------------------------------------------------------- помост
    // ОБХОД ТОЧЕК ВОКРУГ NPC С НАСТОЯЩИМ МАРШРУТОМ.
    //
    // Трое нежити стояли в шести ярдах от Смотрителя Кейса по плоскости и в пяти с половиной
    // ПО ВЫСОТЕ: он на помосте склепа, они у подножия. Ядро мерит взаимодействие в
    // пространстве (GetNPCIfCanInteractWith: радиус существа плюс четыре ярда), точка контакта
    // в полутора ярдах от него не строится — и по 22 круга «есть что сдать -> не подойти
    // вплотную» у каждого. Первая мысль — один пакет движения прямо в точку контакта — была
    // отвергнута Кодексом, и правильно: прямая видимость не доказывает проходимость, а
    // такой пакет проведёт сквозь стену или уронит с края. Поэтому не шаг, а ПОИСК: вокруг
    // NPC — на ступенях, на краю помоста, с другой стороны — может найтись точка, до
    // которой ПОЛНЫЙ маршрут есть и откуда ядро уже ответит «можно». Кольца в 3, 5 и 8
    // ярдов по восьми направлениям, высота — от карты у самой точки, на той же поверхности,
    // что и NPC; берём первую с полным путём. Дорого (до 24 построений) — один раз за
    // намерение и только в тупике.
    // СКОЛЬКО НАМ ЕЩЁ ИДТИ — ДО ТОЙ ТОЧКИ, КУДА МЫ ИДЁМ (Кодекс). Пока держится точка обхода,
    // расстояние до самого NPC не убывает — путь ведёт вбок и вокруг, — и проверка «двадцать
    // секунд без продвижения» оборвала бы именно тот заход, ради которого обход и затевался.
    float ProgressDist(Companion const& c, Player* self, WorldObject const* target) const
    {
        if (c.RingHeld && c.ApproachFor == target->GetGUID())
            return self->GetExactDist(c.ApproachX, c.ApproachY, c.ApproachZ);
        return self->GetExactDist(target);
    }

    bool FindReachableApproach(Companion& c, Player* self, WorldObject const* target,
                               Position const* avoid = nullptr)
    {
        if (c.RingTried || !target)
            return false;
        c.RingTried = true;
        static float const rings[3] = { 3.0f, 5.0f, 8.0f };
        uint32 probed = 0, offGround = 0, offSurface = 0, noRoute = 0, lastType = 0;
        for (float r : rings)
            for (uint32 k = 0; k < 8; ++k)
            {
                float const a = float(k) * 6.2831853f / 8.0f;
                float const x = target->GetPositionX() + std::cos(a) * r;
                float const y = target->GetPositionY() + std::sin(a) * r;
                if (!MapManager::IsValidMapCoord(self->GetMapId(), x, y))
                    continue;
                // ОТВЕРГНУТУЮ ТОЧКУ НЕ ПРЕДЛАГАТЬ СНОВА (Кодекс): поиск детерминирован, и без
                // этого второй проход из той же позиции вернул бы ту же точку.
                if (avoid && avoid->GetExactDist2d(x, y) < 2.5f)
                    continue;           // 2.5, не 2.0: соседи между кольцами 3 и 5 стоят ровно на 2.0
                float const z = self->GetMap()->GetHeight(self->GetPhaseShift(), x, y,
                                                          target->GetPositionZ() + 3.0f, true, 20.0f);
                if (z <= INVALID_HEIGHT)
                    { ++offGround; continue; }
                if (std::fabs(z - target->GetPositionZ()) > 6.0f)
                    { ++offSurface; continue; }         // не та поверхность
                ++probed;
                PathGenerator path(self);
                bool const built = path.CalculatePath(x, y, z, false);
                lastType = uint32(path.GetPathType());      // и при отказе тоже — это главный случай
                if (!built || (path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE)))
                    { ++noRoute; continue; }
                c.ApproachX = x;
                c.ApproachY = y;
                c.ApproachZ = z;
                c.ApproachFor = target->GetGUID();
                // ДЕРЖИМ ДО ПРИХОДА ИЛИ ДО ОТКАЗА, А НЕ ДВАДЦАТЬ СЕКУНД (Кодекс): путь в обход
                // помоста бывает много длиннее, чем прямая до точки, и по истечении срока
                // подход снова взял бы недостижимую точку контакта — при том что второй обход
                // уже запрещён.
                c.ApproachMs = 0;
                c.RingHeld = true;
                c.RawTarget = false;
                c.Stalled = false;
                c.NoPathFails = 0;
                c.NoPathMs = 0;
                c.UnstickTries = 0;
                TC_LOG_INFO("server.worldserver",
                    "Constellation ПОДХОД {}: к {} ({}) точка контакта недостижима — нашлась точка в {:.0f} ярдах от него ({:.0f} {:.0f} {:.1f}), по высоте от меня {:+.1f}, иду к ней",
                    self->GetName(), target->GetName(), target->GetEntry(), r, x, y, z, z - self->GetPositionZ());
                return true;
            }
        TC_LOG_INFO("server.worldserver",
            "Constellation ПОДХОД {}: вокруг {} ({}) годной точки нет — проверено {}, без земли {}, не та поверхность {}, без маршрута {} (последний тип {:X})",
            self->GetName(), target->GetName(), target->GetEntry(), probed, offGround, offSurface, noRoute, lastType);
        return false;
    }

    // ОТКАЗ ПОДХОДА — С ЧИСЛАМИ, а не одной фразой: по плоскости и по высоте, сколько раз
    // построитель отказал и каким типом, упёрлись ли, видно ли.
    void LogApproachFailure(Companion const& c, Player* self, WorldObject const* target, char const* whom) const
    {
        if (!target)
            return;
        TC_LOG_INFO("server.worldserver",
            "Constellation ПОДХОД {}: к {} {} ({}) не подойти — по плоскости {:.1f}, по высоте {:+.1f}, отказов маршрута {}, последний тип {:X}, тупик {}, видно {}, {} с",
            self->GetName(), whom, target->GetName(), target->GetEntry(),
            self->GetExactDist2d(target), target->GetPositionZ() - self->GetPositionZ(),
            uint32(c.NoPathFails), c.LastPathType, c.Stalled ? 1 : 0,
            self->IsWithinLOSInMap(target) ? 1 : 0, c.ModeMs / 1000);
    }

    // ---------------------------------------------------------------- прибор «ПРОСТОЙ»
    // ПЕРВЫЕ ВОРОТА ЯДРА, КОТОРЫЕ НЕ ПРОПУСКАЮТ КВЕСТ, — по именам и В ПОРЯДКЕ ЯДРА.
    //
    // Тот же ряд, что у Player::CanTakeQuest в этом форке (Player.cpp:14393), а не
    // придуманный: иначе прибор назвал бы не те ворота (Кодекс). Итог ядра — контроль:
    // расхождение с найденным печатается как расхождение, а не как «ядро даёт».
    char const* FirstFailingGate(Player* self, Quest const* q) const
    {
        char const* gate = nullptr;
        if (DisableMgr::IsDisabledFor(DISABLE_TYPE_QUEST, q->GetQuestId(), self)) gate = "выключен";
        else if (!self->SatisfyQuestStatus(q, false))          gate = "статус";
        else if (!self->SatisfyQuestExclusiveGroup(q, false))  gate = "группа";
        else if (!self->SatisfyQuestClass(q, false))           gate = "класс";
        else if (!self->SatisfyQuestRace(q, false))            gate = "раса";
        else if (!self->SatisfyQuestLevel(q, false))           gate = "уровень";
        else if (!self->SatisfyQuestSkill(q, false))           gate = "навык";
        else if (!self->SatisfyQuestReputation(q, false))      gate = "репутация";
        else if (!self->SatisfyQuestDependentQuests(q, false)) gate = "предыдущие";
        else if (!self->SatisfyQuestTimed(q, false))           gate = "срок";
        else if (!self->SatisfyQuestDay(q, false))             gate = "день";
        else if (!self->SatisfyQuestWeek(q, false))            gate = "неделя";
        else if (!self->SatisfyQuestMonth(q, false))           gate = "месяц";
        else if (!self->SatisfyQuestSeasonal(q, false))        gate = "сезон";
        else if (!self->SatisfyQuestConditions(q, false))      gate = "условия";
        else if (!self->SatisfyQuestExpansion(q, false))       gate = "дополнение";
        bool const core = self->CanTakeQuest(q, false);
        if (gate && !core)
            return gate;
        if (!gate && core)
            return self->SatisfyQuestLog(false) ? "ядро даёт" : "ядро даёт, но журнал полон";
        return gate ? "не классифицировано" : "иное";
    }

    // ПРИВЯЗАТЬ КАМЕНЬ У ТРАКТИРЩИКА, МИМО КОТОРОГО ПРОХОДИМ.
    //
    // Оператор: камень должен вести в ТЕКУЩИЙ хаб, иначе он бесполезен как оптимизатор дороги.
    // Опкод тот же, что шлёт клиент по кнопке «сделать эту таверну домом»; ядро само проверит,
    // что перед нами трактирщик и что мы достаточно близко, а домашнюю точку поставит по нашему
    // месту (SendBindPoint -> заклинание 3286 -> Spell::EffectBind).
    void BindAtInn(Companion& c, Player* self)
    {
        if (c.InnScanMs)
            return;
        c.InnScanMs = 60000 + (c.Guid.GetCounter() % 23) * 1000;   // разброс, как у полётных точек
        std::list<Creature*> around;
        Trinity::AnyUnitInObjectRangeCheck check(self, 30.0f);
        Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, around, check);
        Cell::VisitGridObjects(self, searcher, 30.0f);
        for (Creature* cr : around)
        {
            if (!cr->IsAlive() || !cr->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))
                continue;
            if (c.BoundAt == cr->GetGUID())
                return;                     // здесь уже привязаны — второй раз незачем
            // НОВАЯ ПРИВЯЗКА УНИЧТОЖАЕТ ПРЕЖНЮЮ (Кодекс), поэтому не при каждой встрече: только
            // когда дом действительно далеко — другая карта или дальше полукилометра. Проходя
            // мимо соседней таверны того же хаба, ничего не трогаем.
            bool const homeElsewhere = self->m_homebind.GetMapId() != self->GetMapId()
                || self->GetExactDist2d(self->m_homebind.GetPositionX(), self->m_homebind.GetPositionY()) > 500.0f;
            if (!homeElsewhere)
                { c.BoundAt = cr->GetGUID(); return; }
            if (!self->GetNPCIfCanInteractWith(cr->GetGUID(), UNIT_NPC_FLAG_INNKEEPER, UNIT_NPC_FLAG_2_NONE))
                continue;
            WorldLocation const was = self->m_homebind;
            WorldPacket raw(CMSG_BINDER_ACTIVATE);
            WorldPackets::NPC::Hello bind(std::move(raw));
            bind.Unit = cr->GetGUID();
            c.Session->HandleBinderActivateOpcode(bind);
            // УСПЕХ — ПО ТОМУ, ЧТО ТОЧКА ДЕЙСТВИТЕЛЬНО ДРУГАЯ: карта или координаты (Кодекс).
            // «Стало ближе» — не то же самое и могло бы соврать.
            bool const bound = self->m_homebind.GetMapId() != was.GetMapId()
                || std::fabs(self->m_homebind.GetPositionX() - was.GetPositionX()) > 0.5f
                || std::fabs(self->m_homebind.GetPositionY() - was.GetPositionY()) > 0.5f;
            // НЕУДАЧУ НЕ ЗАПОМИНАЕМ НАВСЕГДА: иначе второй попытки в этой сессии не будет.
            if (bound)
                c.BoundAt = cr->GetGUID();
            TC_LOG_INFO("server.worldserver",
                "Constellation ПРИВЯЗКА {}: у {} ({}) в зоне {} — {}",
                self->GetName(), cr->GetName(), cr->GetEntry(), self->GetZoneId(),
                bound ? "дом теперь здесь" : "ядро привязку не дало");
            return;                         // по одному трактирщику за проход
        }
    }

    // КАМЕНЬ КАК ОПТИМИЗАТОР ДОРОГИ, И ТОЛЬКО ОН.
    //
    // Уходим камнем, если до цели далеко, а от домашней точки до неё близко: тогда «камень плюс
    // короткая дорога» короче длинной. Ядро проверит откат и бой само; судим по состоянию —
    // перенесло нас или нет.
    bool HearthTowards(Companion& c, Player* self, Position const& target)
    {
        if (c.HearthCooldownMs || self->IsInCombat() || self->IsInFlight())
            return false;
        if (self->m_homebind.GetMapId() != self->GetMapId())
            return false;                   // дом на другой карте — это уже не оптимизация
        float const walkAll = self->GetExactDist2d(target.GetPositionX(), target.GetPositionY());
        float const fromHome = std::sqrt(std::pow(self->m_homebind.GetPositionX() - target.GetPositionX(), 2.0f)
                                       + std::pow(self->m_homebind.GetPositionY() - target.GetPositionY(), 2.0f));
        if (walkAll < Cfg().FlyIfFartherThan || fromHome > walkAll * 0.5f
            || walkAll - fromHome < Cfg().FlyIfSaves)
            return false;
        Item* stone = self->GetItemByEntry(6948);
        if (!stone)
            return false;
        SpellInfo const* si = sSpellMgr->GetSpellInfo(8690, DIFFICULTY_NONE);
        if (!si || !self->GetSpellHistory()->IsReady(si, stone->GetEntry())
            || self->IsNonMeleeSpellCast(false))
            return false;
        WorldPacket raw(CMSG_USE_ITEM);
        WorldPackets::Spells::UseItem use(std::move(raw));
        use.PackSlot = stone->GetBagSlot();
        use.Slot = stone->GetSlot();
        use.CastItem = stone->GetGUID();
        use.Cast.CastID = ObjectGuid::Create<HighGuid::Cast>(SPELL_CAST_SOURCE_NORMAL, self->GetMapId(),
            8690, self->GetMap()->GenerateLowGuid<HighGuid::Cast>());
        use.Cast.SpellID = 8690;
        use.Cast.Target.Flags = TARGET_FLAG_NONE;
        // ГАСИМ ДОРОГУ ДО ОТПРАВКИ: движение оборвало бы собственный перенос.
        StopMoving(c, self);
        c.Waypoints.clear();
        c.WaypointIndex = 0;
        c.Moving = false;
        c.Session->HandleUseItemOpcode(use);

        // ПАУЗУ СТАВИМ, ТОЛЬКО ЕСЛИ КАСТ ДЕЙСТВИТЕЛЬНО НАЧАЛСЯ (Кодекс): иначе мы бы честно
        // простояли десять секунд после отказа ядра.
        bool const casting = self->IsNonMeleeSpellCast(false);
        c.HearthCooldownMs = casting ? 600000 : 60000;
        c.HearthCastMs = casting ? si->CalcCastTime() + 2000 : 0;
        TC_LOG_INFO("server.worldserver",
            "Constellation КАМЕНЬ {}: до цели {:.0f} ярдов, от дома до неё {:.0f} — {}",
            self->GetName(), walkAll, fromHome,
            casting ? "читаю камень" : "ядро каст не начало");
        return casting;
    }

    // УЗНАТЬ ПОЛЁТНУЮ ТОЧКУ У МАСТЕРА, МИМО КОТОРОГО ПРОХОДИМ.
    //
    // Игрок, впервые подошедший к полётному мастеру, получает точку — и дальше может к ней
    // летать. Спутники этого не делали ни разу, поэтому их сеть узлов навсегда оставалась
    // стартовой, а любая дальняя цель упиралась в пеший бюджет.
    //
    // ПУТЬ — СВОЙ ОПКОД, А НЕ БЕСЕДА, и это поправка по замеру. Первая редакция искала пункт
    // беседы с OptionNpc == Taxinode; на боевом это дало семь разговоров и НОЛЬ узнанных точек,
    // потому что у большинства мастеров меню беседы нет вовсе. У клиента для этого есть
    // отдельная посылка, и обработчик делает ровно то, что нужно:
    //
    //     CMSG_ENABLE_TAXI_NODE -> HandleEnableTaxiNodeOpcode (TaxiHandler.cpp:35)
    //         -> GetNPCIfCanInteractWith(..., UNIT_NPC_FLAG_FLIGHTMASTER)
    //         -> SendLearnNewTaxiNode(unit)          // и вот он ставит бит в маске
    //
    // Дёшево: обзор раз в тридцать секунд и только когда спутник свободен, и только если ядро
    // уже разрешает с мастером говорить (тот же вопрос, что задаёт обработчик такси).
    void LearnTaxiNode(Companion& c, Player* self)
    {
        if (c.TaxiScanMs)
            return;
        // РАЗБРОС ОБЯЗАТЕЛЕН (Кодекс): без него весь состав отсчитывает одинаковые тридцать
        // секунд от общего старта и обходит сетку одним залпом.
        c.TaxiScanMs = 30000 + (c.Guid.GetCounter() % 17) * 1000;
        std::list<Creature*> around;
        Trinity::AnyUnitInObjectRangeCheck check(self, 30.0f);
        Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, around, check);
        Cell::VisitGridObjects(self, searcher, 30.0f);
        for (Creature* cr : around)
        {
            if (!cr->IsAlive() || !cr->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER))
                continue;
            if (c.TaxiDone.count(cr->GetGUID()) || c.TaxiRetry.count(cr->GetGUID()))
                continue;
            // ЯДРО РЕШАЕТ, БЛИЗКО ЛИ. Тот же вопрос, что задаёт HandleActivateTaxiOpcode.
            if (!self->GetNPCIfCanInteractWith(cr->GetGUID(), UNIT_NPC_FLAG_FLIGHTMASTER, UNIT_NPC_FLAG_2_NONE))
                continue;
            uint32 const node = sObjectMgr->GetNearestTaxiNode(cr->GetPositionX(), cr->GetPositionY(),
                                                               cr->GetPositionZ(), cr->GetMapId(), self->GetTeam());
            if (!node || self->m_taxi.IsTaximaskNodeKnown(node))
                { c.TaxiDone.insert(cr->GetGUID()); continue; }   // узел наш — вопрос закрыт навсегда

            // СВОЙ ОПКОД, А НЕ БЕСЕДА (ядро, TaxiHandler.cpp:35): у большинства мастеров меню
            // беседы нет, и пункт «узнать точку» искать негде — отсюда семь «нет пункта» и ноль
            // узнанных на замере. Клиент для этого шлёт CMSG_ENABLE_TAXI_NODE, а обработчик сам
            // проверяет, что перед нами полётный мастер, и зовёт SendLearnNewTaxiNode.
            WorldPacket raw(CMSG_ENABLE_TAXI_NODE);
            WorldPackets::Taxi::EnableTaxiNode enable(std::move(raw));
            enable.Unit = cr->GetGUID();
            c.Session->HandleEnableTaxiNodeOpcode(enable);

            bool got = self->m_taxi.IsTaximaskNodeKnown(node);
            std::string why;
            if (!got)
            {
                // ЗОНД (временный): та же работа в обход ворот разговора, и рядом — те самые
                // величины, которыми ядро принимает решение. Голого bool мало: SendLearnNewTaxiNode
                // возвращает ИСТИНУ и при curloc == 0, ничего не поставив (TaxiHandler.cpp:134),
                // поэтому исход разбирается по узлу ядра и его биту, а не по возврату.
                Player* const sp = c.Session->GetPlayer();
                uint32 const coreNode = sp ? sObjectMgr->GetNearestTaxiNode(cr->GetPositionX(),
                    cr->GetPositionY(), cr->GetPositionZ(), cr->GetMapId(), sp->GetTeam()) : 0;
                bool const wasCore = sp && coreNode && sp->m_taxi.IsTaximaskNodeKnown(coreNode);
                bool const direct = c.Session->SendLearnNewTaxiNode(cr);
                bool const nowCore = sp && coreNode && sp->m_taxi.IsTaximaskNodeKnown(coreNode);
                got = self->m_taxi.IsTaximaskNodeKnown(node);
                why = Trinity::StringFormat(" [зонд: игрок сессии {}, узел ядра {}, его бит {}->{}, "
                    "возврат {}, наш бит {}, сторона {}, карта {}, до мастера {:.1f}]",
                    sp == self ? "тот же" : "ЧУЖОЙ", coreNode, wasCore ? 1 : 0, nowCore ? 1 : 0,
                    direct ? "да" : "нет", got ? 1 : 0, uint32(self->GetTeam()), cr->GetMapId(),
                    self->GetDistance(cr));
            }
            if (got)
                c.TaxiDone.insert(cr->GetGUID());
            else
                c.TaxiRetry[cr->GetGUID()] = 600000;
            TC_LOG_INFO("server.worldserver",
                "Constellation ПОЛЁТ {}: у {} ({}) точка {} — {}; всего точек {}{}",
                self->GetName(), cr->GetName(), cr->GetEntry(), node,
                got ? "узнана" : "ядро не дало", KnownTaxiNodes(self), why);
            return;                     // по одному мастеру за проход
        }
    }

    // СКОЛЬКО ПОЛЁТНЫХ ТОЧЕК МЫ ЗНАЕМ — по маске ядра, теми же битами, что оно и хранит.
    uint32 KnownTaxiNodes(Player* self) const
    {
        // ПО САМОЙ МАСКЕ, А НЕ ПО НОМЕРАМ УЗЛОВ: IsTaximaskNodeKnown индексирует массив без
        // проверки границ, и перебор номеров вышел бы за него. Считаем взведённые биты.
        uint32 known = 0;
        TaxiMask const& mask = self->m_taxi.GetTaxiMask();
        for (size_t i = 0; i < mask.size(); ++i)          // обход по индексу: begin/end у маски не константные
            for (uint8 bit = 0; bit < 8; ++bit)
                if (mask[i] & (1u << bit))
                    ++known;
        return known;
    }

    // СТОИТ ЛИ ЛЕТЕТЬ, И ОТКУДА КУДА.
    //
    // Считаем полную стоимость, а не одну её часть: дойти до мастера + перелёт + дойти от узла
    // назначения до цели. Летим, только если это заметно короче прямой ходьбы — иначе перелёт
    // сам себе помеха. Узел назначения перебираем ТОЛЬКО среди известных и только с настоящим
    // маршрутом от узла отправления: «ближайший к цели» может оказаться недостижимым (Кодекс).
    bool PlanFlight(Companion& c, Player* self, Position const& target)
    {
        if (c.FlightCooldownMs || self->IsInCombat() || self->IsInFlight())
            return false;
        float const walkAll = self->GetExactDist2d(target.GetPositionX(), target.GetPositionY());
        if (walkAll < Cfg().FlyIfFartherThan)
            return false;
        auto it = _flightMasters.find(self->GetMapId());
        if (it == _flightMasters.end())
            return false;

        // 1) мастер: ближайший, чей узел мы знаем, и до которого идти много меньше, чем до цели
        FlightPoint const* master = nullptr;
        float masterWalk = walkAll * 0.5f;          // дальше половины пути — уже не по дороге
        uint32 fromNode = 0;
        FactionTemplateEntry const* mine = self->GetFactionTemplateEntry();
        for (FlightPoint const& m : it->second)
        {
            float const d = self->GetExactDist2d(m.Where.GetPositionX(), m.Where.GetPositionY());
            if (d > masterWalk)
                continue;
            if (mine)
                if (FactionTemplateEntry const* theirs = sFactionTemplateStore.LookupEntry(m.Faction))
                    if (mine->IsHostileTo(theirs))
                        continue;
            uint32 const node = sObjectMgr->GetNearestTaxiNode(m.Where.GetPositionX(), m.Where.GetPositionY(),
                                                               m.Where.GetPositionZ(), self->GetMapId(), self->GetTeam());
            if (!node || !self->m_taxi.IsTaximaskNodeKnown(node))
                continue;
            masterWalk = d;
            master = &m;
            fromNode = node;
        }
        if (!master)
            return false;

        TaxiNodesEntry const* from = sTaxiNodesStore.LookupEntry(fromNode);
        if (!from)
            return false;

        // 2) узел назначения: среди ИЗВЕСТНЫХ на этой карте — тот, от которого до цели идти
        // меньше всего, и до которого граф действительно строит маршрут.
        // СНАЧАЛА ОТБИРАЕМ, ПОТОМ СПРАШИВАЕМ ГРАФ (Кодекс): построение маршрута — дорогая
        // операция, и перебирать ею весь список узлов в такте мира нельзя. Собираем известные
        // узлы своей карты, сортируем по остатку пути до цели и спрашиваем граф только у
        // первых нескольких.
        std::vector<std::pair<float, TaxiNodesEntry const*>> cands;
        for (TaxiNodesEntry const* to : sTaxiNodesStore)
        {
            if (!to || to->ID == fromNode || to->ContinentID != int32(self->GetMapId()))
                continue;
            if (!self->m_taxi.IsTaximaskNodeKnown(to->ID))
                continue;
            float const tail = std::sqrt(std::pow(to->Pos.X - target.GetPositionX(), 2.0f)
                                       + std::pow(to->Pos.Y - target.GetPositionY(), 2.0f));
            if (tail < walkAll)                     // хуже прямой ходьбы нам не нужно
                cands.push_back({ tail, to });
        }
        std::sort(cands.begin(), cands.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        uint32 bestNode = 0;
        float bestTail = walkAll;
        std::vector<uint32> route;
        uint32 asked = 0;
        for (auto const& [tail, to] : cands)
        {
            if (asked >= Cfg().FlyRouteCandidates)
                break;
            ++asked;
            route.clear();
            if (TaxiPathGraph::GetCompleteNodeRoute(from, to, self, route) < 2)
                continue;                           // маршрута нет — этот узел не годится
            bestTail = tail;
            bestNode = to->ID;
            break;                                  // список уже отсортирован: первый годный и лучший
        }
        if (!bestNode)
            return false;

        // 3) стоит ли овчинка выделки: пешком целиком против «до мастера + от узла»
        float const byAir = masterWalk + bestTail;
        if (byAir > walkAll * 0.6f || walkAll - byAir < Cfg().FlyIfSaves)
            return false;

        c.FlightMaster.Clear();
        c.FlightMasterPos = master->Where;
        c.FlightMasterEntry = master->Entry;
        c.FlightFromNode = fromNode;
        c.FlightNode = bestNode;
        c.FlightSavedYards = walkAll - byAir;
        TC_LOG_INFO("server.worldserver",
            "Constellation ПОЛЁТ {}: до цели {:.0f} ярдов пешком; лечу {} -> {} (до мастера {} — {:.0f}, от узла — {:.0f}), экономия {:.0f}",
            self->GetName(), walkAll, fromNode, bestNode, master->Entry, masterWalk, bestTail, c.FlightSavedYards);
        return true;
    }

    // ПОЧЕМУ СТОИМ — раз в пять минут у того, кому делать нечего. Прежний прибор печатался по
    // разу при первом поиске и устаревал: 33 из 122 стояли молча. Связи «существо -> квесты»
    // здесь только ПЕЧАТАЮТСЯ.
    // ОБХОД СУМОК — ОТДЕЛЬНО ОТ «ДЕЛАТЬ НЕЧЕГО».
    //
    // Зовётся из двух мест: из простоя (как и раньше) и из однократной строки DIAG,
    // которая печатается ЗАНЯТОМУ спутнику. Второе появилось потому, что рыцарь смерти
    // занят всегда — у него висит цель, которую нечем закрыть, — и первый вызов до него
    // не доходил никогда.
    void LogWardrobe(Player* self) const
    {
        // ГАРДЕРОБ — ПОЧЕМУ НЕ НАДЕТО. Тринадцать рыцарей смерти стоят одетыми, но без оружия,
        // а модуль печатает только успех надевания и отказ ядра — то есть про вещь, до которой
        // очередь не дошла, не говорит ничего. Здесь по каждой ненадетой вещи из сумок: что
        // это, куда встаёт, и чей именно ответ её остановил.
        std::string wardrobe;
        uint32 gearSeen = 0, toolsSeen = 0, otherSeen = 0, gearSkipped = 0, toolsSkipped = 0;
        std::map<uint32, uint32> otherByClass;      // класс предмета -> сколько таких
        auto look = [&](Item* it)
        {
            if (!it || it->IsEquipped() || !it->GetTemplate())
                return;
            ItemTemplate const* tpl = it->GetTemplate();
            if (tpl->GetClass() != ITEM_CLASS_WEAPON && tpl->GetClass() != ITEM_CLASS_ARMOR)
            {
                // НЕ ОДЕЖДА — НО МОЖЕТ БЫТЬ ИНСТРУМЕНТОМ. Предмет с единственным заклинанием
                // «при использовании» — это то, чем закрывают цели, которых не закрыть боем.
                // Печатаем его вместе с заклинанием и с существом, которое это заклинание
                // называет целью: сведения о заклинании предмета живут в клиентских данных, и
                // спросить их можно только отсюда.
                uint32 const useSpell = UseSpellOf(it);
                if (!useSpell)
                {
                    ++otherSeen;                    // ни одежда, ни инструмент — но в сумке лежит
                    ++otherByClass[tpl->GetClass()];
                    return;
                }
                if (toolsSeen >= 4)
                    { ++toolsSkipped; return; }     // инструмент, которому не хватило места в строке
                ++toolsSeen;
                std::string names;
                if (SpellInfo const* si = sSpellMgr->GetSpellInfo(useSpell, DIFFICULTY_NONE))
                    for (SpellEffectInfo const& eff : si->GetEffects())
                        if (eff.ImplicitTargetConditions)
                            for (Condition const& cond : *eff.ImplicitTargetConditions)
                                if (UnitEntryCondition(cond, 0))
                                    names += std::to_string(cond.ConditionValue2) + " ";
                wardrobe += Trinity::StringFormat("инструмент {} (кл.{}/{}) закл. {}{}{}; ",
                    it->GetEntry(), uint32(tpl->GetClass()), uint32(tpl->GetSubClass()), useSpell,
                    names.empty() ? "" : " по существу ", names);
                return;
            }
            if (gearSeen >= 6)       // предел одежды — только для одежды: у инструментов свой
                { ++gearSkipped; return; }
            ++gearSeen;
            // СЛОТ БЕРЁМ ИЗ ОТВЕТА ЯДРА (Кодекс): FindEquipSlot отвечает на свой вопрос, а
            // обработчик кладёт вещь туда, куда сказал CanEquipItem, — на кольцах и оружии это
            // разные слоты, и сравнение «лучше надетого» шло бы не с тем предметом.
            char const* why = "годится";
            uint16 dest = 0;
            InventoryResult const useItem = self->CanUseItem(it);
            InventoryResult const canEquip = self->CanEquipItem(NULL_SLOT, dest, it, true);
            uint8 slot = canEquip == EQUIP_ERR_OK ? uint8(dest & 0xFF) : self->FindEquipSlot(it, NULL_SLOT, true);
            if (!UsableKind(self, tpl))          why = "не наш класс/навык";
            else if (useItem != EQUIP_ERR_OK)    why = "ядро: использовать нельзя";
            else if (slot == NULL_SLOT)          why = "слота нет";
            else if (canEquip != EQUIP_ERR_OK)   why = "ядро: надеть нельзя";
            else if (Item* worn = self->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (it->GetItemLevel(self) <= worn->GetItemLevel(self))
                    why = "не лучше надетого";
            wardrobe += Trinity::StringFormat("{} (кл.{}/{}, вид {}, ур.пр. {}, слот {}): {}; ",
                it->GetEntry(), uint32(tpl->GetClass()), uint32(tpl->GetSubClass()),
                uint32(tpl->GetInventoryType()), it->GetItemLevel(self),
                slot == NULL_SLOT ? 255 : uint32(slot), why);
        };
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            look(self->GetItemByPos(INVENTORY_SLOT_BAG_0, i));
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = self->GetBagByPos(b))
                for (uint32 j = 0; j < GetBagSize(bag); ++j)
                    look(GetItemInBag(bag, j));
        // РЫЦАРЬ СМЕРТИ: ЗНАЕТ ЛИ 51769. Последнее звено цепочки 12619 — заклинание по кузнице;
        // предмет 38145 источником не оказался, а character_spell на этом ядре пуста, поэтому
        // спросить можно только у ядра в памяти. Одно слово в уже существующей строке.
        if (self->GetClass() == CLASS_DEATH_KNIGHT)
        {
            // 51769 — зачётное заклинание, его не учит никто; зачёт 12619 даёт ЛЮБАЯ руна из
            // spell_chapter1_runeforging_credit, наложенная у кузни. Печатаем, что из этого знаем.
            static uint32 const runes[] = { 53428, 53343, 53344, 62158, 326805, 326855, 326911, 326977, 327082 };
            std::string known;
            for (uint32 sp : runes)
                if (self->HasSpell(sp))
                    known += std::to_string(sp) + " ";
            wardrobe += Trinity::StringFormat("руны: {}; ", known.empty() ? std::string("ни одной") : known);
        }
        // ХВОСТ: чего строка НЕ показала. Без него «в сумках одиннадцать предметов, надеваний
        // ноль» и «в сумках одиннадцать НЕ-вещей» выглядят одинаково — пустой строкой.
        if (otherSeen || gearSkipped || toolsSkipped)
        {
            std::string classes;
            for (auto const& [cls, n] : otherByClass)
                classes += Trinity::StringFormat("кл.{}×{} ", cls, n);
            wardrobe += Trinity::StringFormat("прочее {} ({}); одежды за пределом {}; инструментов за пределом {}; ",
                otherSeen, classes, gearSkipped, toolsSkipped);
        }
        if (!wardrobe.empty())
            TC_LOG_INFO("server.worldserver", "Constellation ГАРДЕРОБ {} (ур. {}, класс {}): {}",
                self->GetName(), uint32(self->GetLevel()), uint32(self->GetClass()), wardrobe);
    }

    void LogIdle(Companion const& c, Player* self) const
    {
        std::string waiting;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const qid = self->GetQuestSlotQuestId(slot);
            if (!qid || self->GetQuestStatus(qid) != QUEST_STATUS_COMPLETE)
                continue;
            char const* why = "ждёт";
            if (c.Impossible.count(qid))
                why = "закрыть нечем";
            else if (c.TurnInBackoff.count(qid))
                why = "отсрочка";
            else
            {
                bool spawn = false, ender = false;
                auto mapIt = _spawns.find(self->GetMapId());
                for (auto const& [_, e] : sObjectMgr->GetCreatureQuestInvolvedRelationReverseBounds(qid))
                {
                    ender = true;
                    if (mapIt != _spawns.end() && mapIt->second.count(e))
                        spawn = true;
                }
                why = !ender ? "принимающего-существа нет" : (spawn ? "принимающий на карте" : "у принимающего нет точки на карте");
            }
            waiting += std::to_string(qid) + " (" + why + ") ";
        }
        std::list<Creature*> around;
        Trinity::AnyUnitInObjectRangeCheck check(self, 60.0f);
        Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, around, check);
        Cell::VisitGridObjects(self, searcher, 60.0f);
        uint32 printed = 0;
        for (Creature* cr : around)
        {
            if (printed >= 6)
                break;
            auto const rel = sObjectMgr->GetCreatureQuestRelations(cr->GetEntry());
            if (rel.begin() == rel.end())
                continue;
            std::string quests;
            uint32 k = 0;
            for (uint32 qid : rel)
            {
                if (++k > 4)
                    { quests += "…"; break; }
                Quest const* q = sObjectMgr->GetQuestTemplate(qid);
                if (!q)
                    continue;
                static char const* const colourName[5] = { "серое", "зелёное", "жёлтое", "красное", "цвет?" };
                quests += std::to_string(qid) + " " + colourName[std::min<uint8>(QuestColour(self, q), 4)]
                        + "/" + FirstFailingGate(self, q) + (self->CanSeeStartQuest(q) ? "/видит" : "")
                        + (c.QuestRefused.count(qid) ? " (отказной)" : "") + "; ";
            }
            ++printed;
            // СВОИ ФИЛЬТРЫ — ОТДЕЛЬНО ОТ ВОРОТ ЯДРА (Кодекс): чёрный список и отказные — модуля,
            // и без них строка обвинила бы ядро.
            // 3D-расстояние и фаза по спавну (Кодекс/оператор: «боты знают, на какой они стадии»):
            // грид ищет по 3D, а печаталось только 2D; фазу до сих пор не печатал никто.
            CreatureData const* crData = cr->GetCreatureData();
            bool const inPhase = crData
                ? PhasingHandler::InDbPhaseShift(self, crData->phaseUseFlags, uint16(crData->phaseId), crData->phaseGroup)
                : self->GetPhaseShift().CanSee(cr->GetPhaseShift());
            TC_LOG_INFO("server.worldserver",
                "Constellation ПРОСТОЙ {} (ур. {}, зона {}): {} ({}) в {:.0f} ярдах ({:.0f} по 3D), видно {}, фаза {}, в чёрном списке {}, статус {:X}: {}",
                self->GetName(), uint32(self->GetLevel()), self->GetZoneId(), cr->GetName(), cr->GetEntry(),
                self->GetExactDist2d(cr), self->GetExactDist(cr), self->IsWithinLOSInMap(cr) ? 1 : 0, inPhase ? 1 : 0,
                c.GiverUnreachable.count(cr->GetGUID()) ? 1 : 0,
                uint64(self->GetQuestDialogStatus(cr)), quests);
            // МЕНЮ ЗДЕСЬ НЕ СПРАШИВАЕМ, И ЭТО РЕШЕНИЕ, А НЕ УПУЩЕНИЕ (Кодекс, вторая проверка).
            //
            // Первая редакция слала отсюда Hello и беседу, чтобы показать меню, которое ядро
            // собрало бы игроку. Но эти обработчики не только печатают: они запускают сценарии
            // приветствия и меняют состояние меню у персонажа. Прибор обязан быть немым, иначе
            // он лечит то, что измеряет. Настоящее меню и так видно в строке РАЗГОВОР, которую
            // печатает сам путь взятия квеста, когда спутник до квестодателя доходит.
        }
        LogWardrobe(self);


        TC_LOG_INFO("server.worldserver",
        // ПЛОЩАДЬ, А НЕ ТОЛЬКО ЗОНА: ауры по местности ядро вешает по ПЛОЩАДИ
            // (Player::UpdateAreaDependentAuras), и призванный принимающий держится именно ими.
            "Constellation ПРОСТОЙ {} (ур. {}, зона {} «{}», площадь {}, полётных точек {}): квестодателей в 60 ярдах с квестами {}; готовые ждут: {}",
            self->GetName(), uint32(self->GetLevel()), self->GetZoneId(), ZoneName(self->GetZoneId()),
            self->GetAreaId(),
            KnownTaxiNodes(self), printed, waiting.empty() ? "-" : waiting);
    }

    // ---------------------------------------------------------------- имя зоны из ядра
    // Имени зоны нет в базе мира: оно живёт в клиентском AreaTable, который ядро уже загрузило.
    // Спрашиваем ровно так же, как спрашивает само ядро. Русское имя, если оно есть в данных,
    // иначе английское — обе локали грузятся из data/dbc, если каталоги на месте.
    static std::string ZoneName(uint32 zoneId)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
        if (!area)
            return "?";
        char const* name = area->AreaName[LOCALE_ruRU];
        if (!name || !*name)
            name = area->AreaName[LOCALE_enUS];
        return name && *name ? name : "?";
    }

    // ВСЕ ЗОНЫ РАЗОМ — ПО КОМАНДЕ. Имён зон в базе мира нет, а таблице покрытия они нужны
    // для каждой строки, не только для тех, где спутник стоял в момент строки простоя.
    // Тот же шаблон строки, что и в ПРОСТОЙ («зона N «имя»»), чтобы сборщик имён ничего
    // нового не учил. ВСЕ записи AreaTable, а не только верхний уровень: QuestSortID
    // указывает и на площади внутри зон — стартовые локации современных рас (6455, 6450,
    // 4755…) заведены именно так, и фильтр по родителю оставил их без имени.
    bool DumpZones(ChatHandler* handler) const
    {
        uint32 n = 0;
        for (AreaTableEntry const* area : sAreaTableStore)
        {
            if (!area)
                continue;
            std::string const name = ZoneName(area->ID);
            if (name == "?")
                continue;
            TC_LOG_INFO("server.worldserver", "Constellation ЗОНЫ: зона {} «{}»", area->ID, name);
            ++n;
        }
        handler->PSendSysMessage("Constellation: зон выписано в журнал: %u", n);
        return true;
    }

    // ---------------------------------------------------------------- квестодатель по карте
    // КАКОЙ КВЕСТ ЯДРО ДАЛО БЫ У ЭТОГО ВИДА — по воротам ядра и светофору. Связи «существо ->
    // квесты» выбирают, КУДА ИДТИ, а не что брать: у самого квестодателя спутник, как и
    // прежде, шлёт Hello, читает меню, собранное ядром, и берёт по цвету.
    uint32 TakeableQuestAt(Player* self, Companion const& c, uint32 entry) const
    {
        for (uint32 qid : sObjectMgr->GetCreatureQuestRelations(entry))
        {
            if (c.QuestRefused.count(qid))
                continue;
            Quest const* q = sObjectMgr->GetQuestTemplate(qid);
            if (!q || self->GetQuestStatus(qid) != QUEST_STATUS_NONE)
                continue;
            if (!self->CanTakeQuest(q, false) || SeasonalKind(q) || ProfessionWeLack(self, q))
                continue;                       // сезонное и чужие профессии — не наша работа
            if (QuestColour(self, q) >= 3)
                continue;                       // красное и неизвестное — не по нам
            return qid;
        }
        return 0;
    }

    bool FindGiverByMap(Companion& c, Player* self, uint32* entry, ObjectGuid::LowType* spawn,
                        Position* pos, uint32* questOut) const
    {
        auto it = _givers.find(self->GetMapId());
        if (it == _givers.end())
            return false;
        FactionTemplateEntry const* mine = self->GetFactionTemplateEntry();
        Giver const* best = nullptr;
        float bestD = Cfg().GiverSeekRange;
        uint32 bestQuest = 0;
        uint32 tooFar = 0;                          // сколько точек отвергнуто ТОЛЬКО за дальностью
        float nearestFar = 0.0f;                    // и на каком расстоянии ближайшая из них
        uint32 nearestFarEntry = 0;
        std::unordered_map<uint32, uint32> byEntry;     // вид -> квест (0 = ничего): ядро спрашиваем раз на вид
        for (Giver const& g : it->second)
        {
            float const d = self->GetExactDist2d(g.Where.GetPositionX(), g.Where.GetPositionY());
            if (d > Cfg().GiverSeekRange)
            {
                ++tooFar;                           // КАЖДУЮ, а не только новую ближайшую (Кодекс)
                if (!nearestFar || d < nearestFar)
                    { nearestFar = d; nearestFarEntry = g.Entry; }
            }
            if (d > bestD + 0.01f || d < Cfg().QuestGiverRange)
                continue;                       // дальше лучшего — не нужен; в обзоре — уже спрошен и молчит
            if (c.SeekBackoff.count(g.SpawnId))
                continue;
            uint32 qid = 0;
            auto known = byEntry.find(g.Entry);
            if (known != byEntry.end())
                qid = known->second;
            else
            {
                bool hostile = false;
                if (mine)
                    if (FactionTemplateEntry const* theirs = sFactionTemplateStore.LookupEntry(g.Faction))
                        hostile = mine->IsHostileTo(theirs);
                if (!hostile)
                    qid = TakeableQuestAt(self, c, g.Entry);
                byEntry[g.Entry] = qid;
            }
            if (!qid)
                continue;
            // УСТОЙЧИВЫЙ ВЫБОР (Кодекс): при равном расстоянии — меньший номер точки, а не
            // порядок контейнера.
            if (best && std::fabs(d - bestD) <= 0.01f && g.SpawnId > best->SpawnId)
                continue;
            bestD = d;
            best = &g;
            bestQuest = qid;
        }
        if (!best)
        {
            // ЗА ПОТОЛКОМ. Печатаем ОДИН раз на спутника: без этого «полётов ноль» не отличить
            // от «лететь не к кому». Квест у дальнего НЕ спрашиваем — это дорого, а строка нужна
            // ровно для решения, стоит ли поднимать потолок.
            if (tooFar && !c.FarDiagDone)
            {
                c.FarDiagDone = true;
                TC_LOG_INFO("server.worldserver",
                    "Constellation ПОТОЛОК {}: в {:.0f} ярдах никого, за потолком {} точек, ближайшая — вид {} в {:.0f} ярдах",
                    self->GetName(), Cfg().GiverSeekRange, tooFar, nearestFarEntry, nearestFar);
            }
            return false;
        }
        *entry = best->Entry;
        *spawn = best->SpawnId;
        *pos = best->Where;
        *questOut = bestQuest;
        // ХОДЯЧИЙ: точка спавна — не место NPC. Идём к ближайшему узлу его маршрута: он проходит
        // каждый узел раз за круг, а по приходу отбор ищет его в 60 ярдах.
        if (best->PathId)
            if (WaypointPath const* path = sWaypointMgr->GetPath(best->PathId))
            {
                float bestNode = 1.0e9f;
                for (WaypointNode const& node : path->Nodes)
                {
                    float const dn = self->GetExactDist2d(node.X, node.Y);
                    if (dn < bestNode)
                    {
                        bestNode = dn;
                        pos->Relocate(node.X, node.Y, node.Z);
                    }
                }
                if (bestNode < 1.0e9f)
                    TC_LOG_INFO("server.worldserver", "Constellation ПОХОД {}: {} ({}) ходит по маршруту {} — иду к ближайшему узлу в {:.0f} ярдах",
                        self->GetName(), best->Entry, best->SpawnId, best->PathId, bestNode);
            }
        return true;
    }

    bool FindMenderByMap(Companion const& c, Player* self, bool needSell, bool needRepair, uint32* entry, Position* pos) const
    {
        auto it = _menders.find(self->GetMapId());
        if (it == _menders.end())
            return false;
        FactionTemplateEntry const* mine = self->GetFactionTemplateEntry();
        Mender const* best = nullptr;
        float bestD = 600.0f;               // дальше — не поход, а переезд
        for (Mender const& m : it->second)
        {
            if ((needSell && !m.Sells) || (needRepair && !m.Fixes))
                continue;
            if (needSell && c.VendorNoSell.count(m.Entry))
                continue;                           // недавно отказал во всём — не продавец
            if (mine)
                if (FactionTemplateEntry const* theirs = sFactionTemplateStore.LookupEntry(m.Faction))
                    if (mine->IsHostileTo(theirs))
                        continue;
            float const d = self->GetExactDist2d(m.Where.GetPositionX(), m.Where.GetPositionY());
            if (d < bestD)
                { bestD = d; best = &m; }
        }
        if (!best)
            return false;
        *entry = best->Entry;
        *pos = best->Where;
        return true;
    }

    // СВЕТОФОР ЗАДАНИЯ — ТОТ ЖЕ, ЧТО ВИДИТ ИГРОК В ЖУРНАЛЕ.
    //
    // Оператор, 2026-09-02: «в Легионе делали выбор как у игрока подсветками светофора —
    // красный не трогаем, не доросли; сначала серые и зелёные, после жёлтые».
    //
    // Числа берём у ядра, а не выдумываем. Уровень задания ДЛЯ ЭТОГО персонажа считает
    // Player::GetQuestLevel — с учётом content tuning и того, что задание чужой фракции
    // выдаёт максимум своей полосы. Порог серого — ровно тот, которым ядро метит Trivial
    // в GetQuestDialogStatus: свой уровень выше уровня задания больше чем на
    // Quests.LowLevelHideDiff (по умолчанию 4).
    //
    // 0 — серое, 1 — зелёное, 2 — жёлтое, 3 — оранжевое/красное, 4 — уровень неизвестен.
    // Берём по возрастанию: сперва безопасное; красное и неизвестное не трогаем вовсе.
    uint8 QuestColour(Player* self, Quest const* quest) const
    {
        int32 const my = int32(self->GetLevel());
        int32 const q = self->GetQuestLevel(quest);
        if (q <= 0)
            return 4;                   // НЕИЗВЕСТНО — это не «легко» (Кодекс): ноль означает,
                                        // что данных настройки контента нет, а не что задание
                                        // по нам. При правиле «красное не трогаем» неизвестное
                                        // откладывается и попадает в журнал, а не берётся молча.
        int32 const hide = int32(sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF));
        if (my > q + hide)
            return 0;                   // серое
        if (q < my)
            return 1;                   // зелёное
        if (q == my)
            return 2;                   // жёлтое
        return (q - my <= Cfg().QuestMaxAbove) ? 2 : 3;      // чуть выше — ещё жёлтое; дальше красное
    }

    // ГОДИТСЯ ЛИ ОБЪЕКТ ДЛЯ ОТКРЫТИЯ РУКАМИ.
    //
    // Зачем вообще: прямой опкод использования не спрашивает профессию — обработчик лишь
    // проверяет досягаемость и зовёт Use. Значит спутник вскрыл бы траву или руду без
    // травничества, чего живой игрок не может, и инвариант «играем действиями клиента»
    // был бы нарушён.
    //
    // ЭТО ПРАВИЛО Я ПИСАЛ ДВАЖДЫ И ДВАЖДЫ НЕВЕРНО. Первая версия отвергала любой замок и
    // выключила способность целиком. Вторая требовала, чтобы КАЖДЫЙ заполненный вариант
    // был «просто открыть» с нулевым навыком — и отсеяла 7068 точек из 15553, ровно те,
    // что были нужны: у каждого спутника с целью-предметом видов объектов оказалось ноль.
    //
    // Настоящая семантика (Кодекс, по Spell::CanOpenLock): восемь слотов замка — это
    // АЛЬТЕРНАТИВЫ, а не требования. Ядро возвращает успех на ПЕРВОМ подошедшем варианте.
    // Сундук, который открывается и рукой, и травничеством, открывается рукой — а моё
    // правило отвергало его из-за второго варианта.
    //
    // И величину навыка ядро спрашивает НЕ ВСЕГДА: только если тип замка вообще
    // отображается в профессию через SkillByLockType. Для «просто открыть» ненулевое поле
    // навыка ядром игнорируется — а я на него смотрел.
    //
    // Поэтому список из четырёх известных «открыть» тоже не нужен: разделение на навык и
    // не-навык уже сделано в самой игре, и спрашиваем мы его, а не свою память.
    static bool OpenableByHand(GameObjectTemplate const* tpl)
    {
        uint32 const lockId = tpl->GetLockId();
        if (!lockId)
            return true;                    // замка нет — открывается всем
        LockEntry const* lock = sLockStore.LookupEntry(lockId);
        if (!lock)
            return false;

        bool populated = false;
        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
        {
            if (lock->Type[i] == LOCK_KEY_NONE)
                continue;                   // слот пуст
            populated = true;
            if (lock->Type[i] == LOCK_KEY_SKILL
                && lock->Index[i] != LOCKTYPE_LOCKPICKING
                && SkillByLockType(LockType(lock->Index[i])) == SKILL_NONE)
                return true;                // этого варианта достаточно — руками можно
        }
        return !populated;                  // все слоты пусты — тоже замок без требований
    }

    // КУДА ИДТИ ЗА ПРЕДМЕТОМ ЗАДАНИЯ — ОТВЕЧАЕТ УКАЗАТЕЛЬ, А НЕ ОБХОД МИРА.
    //
    // Здесь стоял обход сетки вокруг спутника, и это была ошибка формы: я скопировал вид
    // боевого поиска, тогда как надо было копировать вид поиска принимающего — тот уже ходит
    // по указателю, построенному один раз. Оператор назвал это прямо: «зачем искать что
    // собирать?? у ресурсов руды и травы свои точки спавна».
    //
    // Возвращает НЕ живой объект, а кандидата: идентификатор спавна и место. Живой объект
    // берётся по приходу, по этому же идентификатору, без единого обхода.
    bool FindGatherCandidate(Companion& c, Player* self) const
    {
        std::set<uint32> wanted, wantedItems;
        WantedEntries(self, wanted, nullptr, nullptr, nullptr, nullptr, &wantedItems);

        // какие виды объектов дают хоть один из нужных предметов
        std::set<uint32> entries;
        for (uint32 item : wantedItems)
            if (auto it = _itemFromGo.find(item); it != _itemFromGo.end())
                entries.insert(it->second.begin(), it->second.end());

        auto mapIt = _goSpawns.find(self->GetMapId());
        uint32 onMap = 0, phased = 0;
        if (mapIt != _goSpawns.end())
            for (uint32 entry : entries)
                if (auto e = mapIt->second.find(entry); e != mapIt->second.end())
                    onMap += uint32(e->second.size());

        // ПОЧЕМУ НЕ НАШЛОСЬ — ПО РАЗУ НА КАЖДОГО. Замер показал 15 спутников с целями-
        // предметами, добываемыми с объектов, и НОЛЬ попыток сбора; различить «нечего
        // искать», «вида нет на карте» и «отсеяла фаза» по нынешним строкам нельзя.
        if (!c.GatherDiagDone && !wantedItems.empty())
        {
            c.GatherDiagDone = true;
            TC_LOG_INFO("server.worldserver",
                "Constellation ОТБОР {}: нужных предметов {}, видов объектов {}, точек на карте {}",
                self->GetName(), uint32(wantedItems.size()), uint32(entries.size()), onMap);
        }

        if (wantedItems.empty() || entries.empty() || mapIt == _goSpawns.end())
            return false;

        float best = 0.0f;
        bool found = false;
        for (uint32 entry : entries)
        {
            auto entryIt = mapIt->second.find(entry);
            if (entryIt == mapIt->second.end())
                continue;
            for (GatherSpawn const& sp : entryIt->second)
            {
            if (auto back = c.GatherBackoff.find(sp.SpawnId); back != c.GatherBackoff.end() && back->second)
                continue;                       // недавно не вышло — этот пока мимо

            // ЧУЖАЯ ФАЗА ОТСЕИВАЕТСЯ ЗДЕСЬ, А НЕ ПО ПРИХОДУ.
            //
            // Ядро само отвечает, увидит ли этот спутник спавн с такими полями фазы —
            // PhasingHandler::InDbPhaseShift. Это проверка по множеству, без запроса к миру.
            // Иначе спутник шёл бы к объекту, которого для него не существует, тратил
            // тридцать секунд и заносил точку в отсрочку — тихий отказ, выглядящий как
            // «дошёл и стоит».
            if (!PhasingHandler::InDbPhaseShift(self, sp.PhaseUseFlags, sp.PhaseId, sp.PhaseGroup))
                { ++phased; continue; }
            // ДАЛЬШЕ, ЧЕМ УСПЕЕМ ДОЙТИ, — НЕ БЕРЁМ (Кодекс).
            //
            // Отбор брал ближайшую точку по ВСЕЙ карте без предела, а само намерение даёт
            // на дорогу 45 секунд и сдаётся за 20 без продвижения. Точка за полкилометра
            // гарантированно кончится отказом и отсрочкой — то есть холостым походом.
            // При скорости бега около семи ярдов в секунду 45 секунд это чуть больше
            // трёхсот ярдов; берём двести с запасом на неровности пути.
            float const d = self->GetExactDist2d(sp.Where.GetPositionX(), sp.Where.GetPositionY());
            if (d > 200.0f)
                continue;
            if (!found || d < best)
            {
                best = d;
                found = true;
                c.GatherSpawnId = sp.SpawnId;
                c.GatherEntry = sp.Entry;
                c.GatherPos = sp.Where;
            }
            }
        }
        if (!found && phased)
            TC_LOG_INFO("server.worldserver",
                "Constellation ОТБОР {}: все {} точек отсеяны фазой", self->GetName(), phased);
        return found;
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
        std::set<uint32> wanted, wantedItems, proxyOk;
        uint32 slotsUsed = 0, incomplete = 0, monsterObjs = 0, unmet = 0;
        WantedEntries(self, wanted, &slotsUsed, &incomplete, &monsterObjs, &unmet, &wantedItems, &proxyOk);
        // ДИАГНОСТИКА ПО РАЗУ НА КАЖДОГО, А НЕ ПО РАЗУ НА ВЕСЬ МОДУЛЬ.
        //
        // Флаг был один на всех, и за целый прогон печаталась РОВНО ОДНА строка — про
        // Гаррика. По ней нельзя сказать ничего о составе: у него нашлось ноль целей-убить,
        // а у скольких ещё — неизвестно. Пять гипотез подряд разбились об это, поэтому
        // флаг переезжает в спутника: 122 строки один раз, и картина видна целиком.
        if (!c.FightDiagDone && slotsUsed)
        {
            c.FightDiagDone = true;
            LogWardrobe(self);
            TC_LOG_INFO("server.worldserver",
                "Constellation DIAG {}: слотов занято {}, незакрытых {}, целей-убить {}, ненабранных {}, видов {}, радиус зова {:.0f}",
                self->GetName(), slotsUsed, incomplete, monsterObjs, unmet, uint32(wanted.size()),
                sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS));
        }
        if (wanted.empty() && wantedItems.empty())
            return nullptr;

        // ОБХОД ШИРЕ ВЫБОРА НА РАДИУС ЗОВА (разбор): сосед в десяти ярдах от цели может стоять
        // за нашей границей поиска, и без него счёт заступников был бы занижен. Кандидатов
        // по-прежнему берём только внутри FightRange — шире ищем, но не дальше бьём.
        float const helpR = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        // ЗАПАС ОБХОДА — ПО БОЛЬШЕМУ ИЗ ДВУХ РАДИУСОВ (разбор): агро бывает шире зова, и у ядра
        // его потолок 45 ярдов (Creature::GetAttackDistance).
        float const margin = std::max(helpR, 45.0f);
        std::list<Creature*> around;
        Trinity::AnyUnitInObjectRangeCheck check(self, Cfg().FightRange + margin);
        Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, around, check);
        Cell::VisitGridObjects(self, searcher, Cfg().FightRange + margin);

        uint32 seen = 0, matched = 0, rejected = 0, rejBusy = 0, rejInvalid = 0, rejLos = 0, rejPhase = 0;
        uint32 assists = 0, bestAssists = 0xFFFFFFFF;
        // СПИСОК УГРОЗ — ОДИН РАЗ НА ПРОХОД, А НЕ НА КАЖДОГО КАНДИДАТА (разбор: перебор был
        // квадратичным). Здесь только те, кто вообще может вступить: живой, враждебный, ещё не
        // занятый чужим боем.
        std::vector<Creature*> threats;
        threats.reserve(around.size());
        for (Creature* other : around)
            if (other->IsAlive() && !other->IsInCombat() && other->IsHostileTo(self))
                threats.push_back(other);
        // сбрасываем перед КАЖДЫМ проходом: иначе прошлый кандидат живёт в состоянии до
        // тех пор, пока его не употребят, и спутник идёт к тому, кого рядом уже нет (Кодекс)
        c.TalkCandidate.Clear();
        float talkDist = -1.0f;
        uint32 lastEntry = 0, lastFaction = 0;
        bool const anyTool = AnyToolQuest(self);   // есть ли предмет от квеста, которым вообще можно работать
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
            // РАЗЛИЧАЕМ, ОТКУДА ЦЕЛЬ. Разговор годится только для целей «убить N существ»,
            // ошибочно размеченных как бой. Существо, с которого нужен ПРЕДМЕТ, — не тот
            // случай, и вести с ним беседу нельзя: это расширило бы исполнение сценариев
            // далеко за пределы разобранного (Кодекс).
            // ПРЯМАЯ ЦЕЛЬ И ПРОКСИ — РАЗНЫЕ ВЕЩИ (Кодекс): прокси годится только в БОЙ, и лишь
            // когда квест не запрещает зачёт через другое существо; в разговор — только прямая.
            bool const byMonster = wanted.count(creature->GetEntry()) != 0;
            bool byProxy = false;
            if (!byMonster && !proxyOk.empty())
                if (CreatureTemplate const* ct = creature->GetCreatureTemplate())
                    for (uint32 i = 0; i < MAX_KILL_CREDIT && !byProxy; ++i)
                        if (ct->KillCredit[i] && proxyOk.count(ct->KillCredit[i]))
                            byProxy = true;             // засчитает нужную цель (KillCredit)
            bool suitable = byMonster || byProxy;
            if (!suitable && !wantedItems.empty())
                if (std::vector<uint32> const* qi = sObjectMgr->GetCreatureQuestItemList(
                        creature->GetEntry(), self->GetMap()->GetDifficultyID()))
                    for (uint32 item : *qi)
                        if (wantedItems.count(item))
                            { suitable = true; break; }
            // ЦЕЛЬ ПО КОНТРАКТУ ЗАКЛИНАНИЯ ПРЕДМЕТА. Существо не в списке нужных — нужен маркер
            // зачёта, которого в мире нет, — но заклинание предмета от квеста называет это
            // существо целью и даёт зачёт маркеру (QuestToolFor, второй проход). Только не
            // боевая цель, в нашей фазе, восприимчивая к игрокам и не отставленная.
            if (!suitable && anyTool && !c.ToolActionMs
                && self->GetPhaseShift().CanSee(creature->GetPhaseShift())
                && !self->IsValidAttackTarget(creature) && !creature->IsImmuneToPC()
                && !c.TalkBackoff.count(creature->GetEntry())
                && !c.TalkRetry.count(creature->GetGUID())
                && !c.TalkUnreachable.count(creature->GetGUID()))
            {
                uint32 sp = 0, qh = 0;
                if (QuestToolFor(self, creature->GetEntry(), &sp, creature, &qh) && qh)
                {
                    float const d = self->GetExactDist(creature);
                    if (talkDist < 0.0f || d < talkDist)
                        { talkDist = d; c.TalkCandidate = creature->GetGUID(); }
                }
                continue;
            }
            if (!suitable)
                continue;
            ++matched;
            // ФАЗА. Поиск по сетке возвращает существ независимо от фазы, а игрок
            // видит только свою: в стартовых зонах их несколько, и без этой проверки
            // спутник целится в тех, кого на его месте не увидел бы вовсе.
            if (!self->GetPhaseShift().CanSee(creature->GetPhaseShift()))
                { ++rejPhase; ++rejected; continue; }
            // НЕ ПО ЗУБАМ — ЭТО ВЕЛИЧИНА ЯДРА, А НЕ ДОГАДКА (замер: 0 побед, 21 гибель).
            //
            // CreatureTemplate::Classification различает Normal, Elite, RareElite, Rare, Trivial,
            // MinusMob. Одиночка без группы берёт только Normal, Trivial и MinusMob: Бренна
            // пятого уровня ходила умирать к редкому Grik'nir the Cold, у которого 250 жизней
            // против её 150, и ядро при этом честно считало его «для нас ур 5» — уровень тут не
            // мера. Мера — вид существа и запас жизней.
            if (Cfg().SkipElites)
            {
                CreatureClassifications const cls = creature->GetCreatureClassification();
                if (cls != CreatureClassifications::Normal && cls != CreatureClassifications::Trivial
                    && cls != CreatureClassifications::MinusMob)
                {
                    if (!c.EliteNoted)
                    {
                        c.EliteNoted = true;
                        TC_LOG_INFO("server.worldserver",
                            "Constellation БОЙ {}: {} ({}) — вид {} (элитный/редкий), в одиночку не беру",
                            self->GetName(), creature->GetName(), creature->GetEntry(), uint32(cls));
                    }
                    ++rejected; continue;
                }
                // ЗАПАС ЖИЗНЕЙ МЕРОЙ НЕ БЕРЁМ (оператор): у тканевых он тонок по устройству, и
                // такой порог заставил бы мага отказываться почти от всего.
                //
                // МЕРА — СКОЛЬКО ЗАСТУПНИКОВ У ЦЕЛИ, И СПРАШИВАЕМ ЭТО У ЯДРА ЕГО ЖЕ ВОПРОСОМ:
                // Creature::CanAssistTo(цель, мы) — тот самый, которым оно отбирает подмогу в
                // CallAssistance. Радиус его же: CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS.
            }
            // СКОЛЬКО ВРАГОВ ПОЛУЧИМ, УДАРИВ ЭТОГО — СЧИТАЕМ ВСЕГДА И НЕЗАВИСИМО ОТ ФИЛЬТРА
            // ЭЛИТНЫХ (разбор: раньше это стояло внутри него, и выключение одного молча
            // выключало другое, а порядок выбора шёл по устаревшему числу).
            //
            // Вопрос один и тот же для обоих источников: «сколько врагов окажется на мне, если я
            // буду драться ВОТ ТУТ». Поэтому и расстояние, и видимость — до МЕСТА БОЯ, то есть
            // до самой цели:
            //   * ПОЗОВУТ — радиус зова ядра и его же условие CanAssistTo, с видимостью до
            //     зовущего (этого требует AnyAssistCreatureInRangeCheck);
            //   * ЗАМЕТЯТ САМИ — собственный агро-радиус соседа GetAttackDistance(мы), уже с
            //     учётом разницы уровней и аур обнаружения.
            assists = 0;
            float packX = 0.0f, packY = 0.0f, packZ = 0.0f;
            for (Creature* other : threats)
            {
                if (other == creature)
                    continue;
                float const dd = other->GetExactDist(creature);
                bool const called = helpR > 0.0f && dd <= helpR
                    && creature->IsWithinLOSInMap(other) && other->CanAssistTo(creature, self);
                bool const notices = dd <= other->GetAttackDistance(self)
                    && other->IsWithinLOSInMap(creature);
                if (called || notices)
                {
                    ++assists;
                    packX += other->GetPositionX();
                    packY += other->GetPositionY();
                    packZ += other->GetPositionZ();
                    if (Cfg().MaxAssist && assists > Cfg().MaxAssist)
                        break;              // порог превышен — считать дальше незачем
                }
            }
            // ГОЛОДАНИЕ ОТМЕНЯЕТ ПОРОГ (разбор): цели, которые водятся только стаями, иначе стали
            // бы невыполнимы навсегда. Долго нет боя — берём наименее людную, но элитных всё
            // равно не берём: это отдельное правило.
            if (Cfg().MaxAssist && assists > Cfg().MaxAssist && c.NoTargetMs < Cfg().StarveMs)
            {
                if (!c.ToughNoted)
                {
                    c.ToughNoted = true;
                    TC_LOG_INFO("server.worldserver",
                        "Constellation БОЙ {}: за {} ({}) вступятся {} — беру того, кто с краю",
                        self->GetName(), creature->GetName(), creature->GetEntry(), assists);
                }
                ++rejected; continue;
            }
            // чужую добычу не отбираем: тот, кто уже с кем-то дерётся, не наш
            if (creature->IsInCombat() && creature->GetVictim() != self)
                { ++rejBusy; ++rejected; continue; }
            if (!self->IsValidAttackTarget(creature))
            {
                // С НИМ НАДО ГОВОРИТЬ, А НЕ ДРАТЬСЯ — И ЭТО ВИДНО ПО ЯДРУ, А НЕ ПО ОПИСАНИЮ.
                //
                // Цель задания, которую ядро отказывается считать атакуемой и у которой
                // есть беседа, — это «поговорить с», как бы ни был записан тип цели.
                // Признак взят у самой игры: IsValidAttackTarget плюс флаг беседы.
                // Зачёт такие NPC выдают по ВЫБОРУ ПУНКТА в окне разговора: у Лилиан Восс
                // и двух других стоит правило «выбран пункт меню -> применить заклинание
                // на говорящего», и это единственный путь закрыть цель.
                // БЛИЖАЙШИЙ, А НЕ ПОСЛЕДНИЙ ВСТРЕЧЕННЫЙ: порядок обхода сетки — не политика
                // выбора цели (Кодекс). И только от целей «убить», не от добытчиков предметов.
                // НЕ ТОЛЬКО ГОВОРЯЩИЕ. Цель задания, которую ядро отказывается считать
                // атакуемой, бывает двух видов, и оба закрываются не боем:
                //   * с беседой — выбором пункта разговора (нежить, «The Wakening»);
                //   * без беседы — ПРИМЕНЕНИЕМ ПРЕДМЕТА, который выдал сам квест. Так
                //     устроен «Extinguishing Hope» у людей: восемь «пожаров» — существа,
                //     которые тушат ведром. На Легионе это доведено до конца (задача 0012),
                //     и там же записано, что опознавать предмет по свойствам заклинания
                //     БЕСПОЛЕЗНО — два фильтра подряд умерли, потому что ведро неотличимо
                //     от камня возвращения. Спрашивать надо КВЕСТ: GetSrcItemId().
                // НЕВОСПРИИМЧИВЫЙ К ИГРОКАМ — НЕ СОБЕСЕДНИК, А ЕЩЁ НЕ ГОТОВАЯ ЦЕЛЬ.
                //
                // Замер: 153 подхода к пленному разведчику Гнилопастных (38142) и столько же
                // «закрыть нечем». Сценарий (zone_durotar.cpp) объясняет: это цель УБИЙСТВА,
                // которую держат невосприимчивой к игрокам, пока тюремщик не выведет пленника;
                // потом флаг снимают, и его бьют. «Не атакуется» тут значит «ещё рано», а не
                // «поговори со мной» — и разговор ему предлагать бессмысленно.
                if (byMonster && creature->IsImmuneToPC())
                {
                    if (c.TalkUnreachable.insert(creature->GetGUID()).second && !c.ImmuneNoted)
                    {
                        c.ImmuneNoted = true;
                        TC_LOG_INFO("server.worldserver",
                            "Constellation ПРИМЕНЕНИЕ {}: {} ({}) пока невосприимчив к игрокам — не собеседник, жду",
                            self->GetName(), creature->GetName(), creature->GetEntry());
                    }
                }
                else if (byMonster && !c.ToolActionMs
                    && !c.TalkBackoff.count(creature->GetEntry())
                    && !c.TalkRetry.count(creature->GetGUID())
                    && !c.TalkUnreachable.count(creature->GetGUID()))
                {
                    float const d = self->GetExactDist(creature);
                    if (talkDist < 0.0f || d < talkDist)
                        { talkDist = d; c.TalkCandidate = creature->GetGUID(); }
                }
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
            // ЦЕЛЬ ВЫБИРАЕТСЯ ПО ОДИНОЧЕСТВУ, А ПОТОМ УЖЕ ПО БЛИЗОСТИ: так лагерь разбирается
            // с края по одному, а не начинается с середины (оператор: «цеплять по 1-2»).
            float d = self->GetExactDist2d(creature);
            if (assists < bestAssists || (assists == bestAssists && d < bestDist))
            {
                bestAssists = assists;
                bestDist = d;
                best = creature;
                // ЦЕНТР ПАЧКИ — ОТ НЕГО И БУДЕМ ОТХОДИТЬ (разбор): лагерь не всегда за спиной
                // цели, а вот центр посчитанных соседей указывает на него прямо.
                c.PackCenterKnown = assists > 0;
                if (c.PackCenterKnown)
                    c.PackCenter.Relocate(packX / float(assists), packY / float(assists), packZ / float(assists));
            }
        }
        if (best)
            c.EngageAssists = bestAssists;      // с чем шли в бой — по этому решим, отводить ли
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
    // ПОЧЕМУ НЕ БЕРУТСЯ КВЕСТЫ — ПРИБОР, А НЕ ДОГАДКА.
    //
    // За все часы работы состав взял ОДИН квест, при этом у 70 спутников из 122 квестодатель
    // стоит в пределах тридцати ярдов. Значит отбор кого-то отвергает, а какой именно
    // проверкой — по журналу не видно вовсе: у этого пути нет ни одной строки, кроме успеха.
    //
    // Тот же приём, что вскрыл боевую воронку: по разу на КАЖДОГО спутника печатаем, сколько
    // существ рядом и сколько отсеяла каждая проверка. Один раз на спутника — 122 строки за
    // прогон, дальше молчок.
    Creature* NearestQuestGiver(Companion& c, Player* self, float rangeOverride = 0.0f) const
    {
        // радиус шире обычного — только по приходу к точке спавна (см. SeekingGiver): NPC
        // стоит не на точке, а рядом, и 30 ярдов от места остановки его не достают
        float const range = rangeOverride > 0.0f ? rangeOverride : Cfg().QuestGiverRange;
        std::list<Creature*> around;
        Trinity::AnyUnitInObjectRangeCheck check(self, range);
        Trinity::CreatureListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(self, around, check);
        Cell::VisitGridObjects(self, searcher, range);

        Creature* best = nullptr;
        float bestDist = range + 1001.0f;   // невидимые идут с надбавкой в тысячу ярдов — после всех видимых
        uint32 seen = 0, dead = 0, nothingToOffer = 0, blacklisted = 0, noLos = 0;
        for (Creature* creature : around)
        {
            ++seen;
            if (!creature->IsAlive())
                { ++dead; continue; }
            // «НЕ NONE» — ЭТО НЕ «ЕСТЬ ЧТО ПРЕДЛОЖИТЬ». ЭТО БЫЛА ГЛАВНАЯ ОШИБКА.
            //
            // GetQuestDialogStatus возвращает не ответ «да/нет», а МАСКУ всего, что этот
            // NPC значит для игрока прямо сейчас (Player.cpp: две ветки — по принимаемым
            // квестам и по выдаваемым). В неё попадают, среди прочего:
            //
            //   * Reward — у спутника есть НЕЗАВЕРШЁННЫЙ квест, который этот NPC ПРИНИМАЕТ.
            //     В клиенте это серый вопросительный знак, а не восклицательный;
            //   * Future — квест у NPC есть, но уровнем спутник до него не дорос;
            //   * *Turnin, *RewardComplete* — «принеси готовое», тоже не выдача.
            //
            // Проверка «!= None» принимала всё это за «есть что взять». Отсюда измеренное
            // расхождение: 56 спутников из 122 квестодателя ВЫБИРАЛИ, шли к нему, открывали
            // разговор — и брать там было нечего, потому что выбран был ПРИНИМАЮЩИЙ их же
            // текущего квеста. За весь день при этом взят ОДИН квест.
            //
            // Берём только биты, означающие «выдаёт квест ПРЯМО СЕЙЧАС». Future
            // сознательно исключён: спутник дорастёт и вернётся сам.
            QuestGiverStatus const st = self->GetQuestDialogStatus(creature);
            QuestGiverStatus const offers =
                  QuestGiverStatus::Quest              | QuestGiverStatus::Trivial
                | QuestGiverStatus::DailyQuest         | QuestGiverStatus::TrivialDailyQuest
                | QuestGiverStatus::RepeatableQuest    | QuestGiverStatus::TrivialRepeatableQuest
                | QuestGiverStatus::MetaQuest          | QuestGiverStatus::TrivialMetaQuest
                | QuestGiverStatus::JourneyQuest       | QuestGiverStatus::TrivialJourneyQuest
                | QuestGiverStatus::LegendaryQuest     | QuestGiverStatus::TrivialLegendaryQuest
                | QuestGiverStatus::ImportantQuest     | QuestGiverStatus::TrivialImportantQuest
                | QuestGiverStatus::CovenantCallingQuest;
            if ((st & offers) == QuestGiverStatus::None)
                { ++nothingToOffer; continue; }
            // ЗНАК — НЕ МЕНЮ (Player.cpp:16086-16112): бит Quest даёт CanSeeStartQuest, а меню — CanTakeQuest.
            // Мегс на Кезане носит знак при пустом меню. Спрашиваем право на выдачу по меню (CanTakeQuest)
            // ПЛЮС фильтры модуля: отказные, цвет, сезонность, чужая профессия (Кодекс, задача 57).
            if (!TakeableQuestAt(self, c, creature->GetEntry()))
                { ++nothingToOffer; continue; }
            if (c.GiverUnreachable.count(creature->GetGUID()))
                { ++blacklisted; continue; }   // уже пробовали дойти и не вышло
            // НЕ ВИДНО — НЕ ЗНАЧИТ НЕ ДОЙТИ. Проверка видимости стояла ПОСЛЕ проверки «предлагает
            // ли» и отсеивала единственного предлагающего: у гнома Ноббина Невин Твистренч в
            // восьми ярдах за стенкой мастерской — «не видно 1», «выбран никто», и так у семи
            // гномов. Дорога строится по сетке, а не по лучу; у похода к нему свой срок и
            // чёрный список. Видимых предпочитаем; невидимого берём, когда видимых нет.
            bool const visible = self->IsWithinLOSInMap(creature);
            if (!visible)
                ++noLos;
            float d = self->GetExactDist2d(creature) + (visible ? 0.0f : 1000.0f);
            if (d < bestDist)
            {
                bestDist = d;
                best = creature;
            }
        }
        if (!c.GiverDiagDone)
        {
            c.GiverDiagDone = true;
            uint32 used = 0;
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                if (self->GetQuestSlotQuestId(slot))
                    ++used;
            TC_LOG_INFO("server.worldserver",
                "Constellation КВЕСТОДАТЕЛЬ {}: рядом существ {}, мертвы {}, нечего предложить {}, "
                "в чёрном списке {}, не видно {}, ВЫБРАН {}; в журнале квестов {}, радиус {:.0f}",
                self->GetName(), seen, dead, nothingToOffer, blacklisted, noLos,
                best ? best->GetName() : "никто", used, range);
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

    // ШАГ СПИНОЙ: идём назад, но смотрим на цель — иначе ядро отвергнет каждый замах
    // (Unit::UpdateMeleeAttackingState требует IsWithinMeleeRange И HasInArc). Один пакет, как и
    // обычный шаг: позиция сзади, ориентация на цель.
    void StepBackFacing(Companion& c, Player* self, Position const& to, Unit* face, float dt)
    {
        float const speed = self->GetSpeed(MOVE_WALK) * 0.9f;   // пятимся медленнее, и это к лучшему:
        float const go = std::min(speed * dt, self->GetExactDist2d(to.GetPositionX(), to.GetPositionY()));
        if (go <= 0.05f)
            return;
        float const ang = self->GetAbsoluteAngle(to.GetPositionX(), to.GetPositionY());
        float const nx = self->GetPositionX() + std::cos(ang) * go;
        float const ny = self->GetPositionY() + std::sin(ang) * go;
        float nz = self->GetMap()->GetHeight(self->GetPhaseShift(), nx, ny, self->GetPositionZ() + 2.0f);
        if (nz <= INVALID_HEIGHT)
            nz = self->GetPositionZ();
        Position next(nx, ny, nz, self->GetAbsoluteAngle(face));    // ЛИЦОМ К ЦЕЛИ
        SendMove(c, self, next, MOVEMENTFLAG_BACKWARD);
        c.Moving = true;
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
                    Constellation::Plan::Planner::Instance()->OnLogin(c.Session->GetPlayer());
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
        Constellation::Plan::Planner::Instance()->OnLogout(c.Guid);
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
    uint32 _flights = 0;                // сколько раз состав улетал
    uint32 _otherTier = 0;              // сколько раз путь дался к цели на другом ярусе
    uint32 _noPath = 0;                 // сколько раз сетка не дала маршрута
    uint32 _noPathLogged = 0;           // из них записано в журнал (потолок 20)
    std::unordered_map<uint32, std::unordered_map<uint32, std::vector<Position>>> _spawns;
    std::unordered_map<uint32, std::vector<AreaTriggerEntry const*>> _questTriggers;   // квест -> зоны
    std::unordered_map<uint32, std::vector<uint32>> _creditedBy;
    std::unordered_set<uint32> _spawnedSomewhere;   // виды, у которых есть хоть одна точка появления
    // ГДЕ СТОЯТ ПОЛЁТНЫЕ МАСТЕРА — своя запись, потому что узел ядро выводит из их позиции.
    struct FlightPoint { uint32 Entry; uint32 Faction; Position Where; };
    std::unordered_map<uint32, std::vector<FlightPoint>> _flightMasters;   // карта -> где стоят
    // цель -> существа, чьи KillCredit на неё указывают
    struct Mender { uint32 Entry; uint32 Faction; Position Where; bool Sells; bool Fixes; };
    struct Giver { uint32 Entry; uint32 Faction; Position Where; ObjectGuid::LowType SpawnId; uint32 PathId; };
    std::unordered_map<uint32, std::vector<Giver>> _givers;      // карта -> квестодатели по таблице
    std::unordered_map<uint32, std::vector<Mender>> _menders;                          // карта -> торговцы
    // Поля фазы храним ВМЕСТЕ с точкой: фаза — это «версия места», и спавн, объявленный
    // в чужой фазе, для этого спутника не существует. Отсеять его надо ДО выхода, а не
    // обнаружить по приходу (оператор: «боты знают, на какой они стадии, значит могу
    // сопоставить, что они ДОЛЖНЫ видеть» — ядро отвечает на это готовым помощником).
    struct GatherSpawn
    {
        uint32 SpawnId; uint32 Entry; Position Where;
        uint8 PhaseUseFlags; uint16 PhaseId; uint32 PhaseGroup;
    };
    // КАРТА -> ВИД -> ТОЧКИ, А НЕ КАРТА -> ВСЕ ТОЧКИ.
    //
    // Плоский список был моей же ошибкой в новом костюме: обход сетки я убрал, но вместо
    // него проходил ВЕСЬ указатель карты у каждого спутника раз в секунду — на стартовых
    // картах это тысячи записей на 122 спутника. Замер: ядро 98 %, 35 переходов за восемь
    // минут, ноль сбора. Указатель существ так не устроен изначально: там ключом стоит вид,
    // и перебираются только нужные.
    std::unordered_map<uint32, std::unordered_map<uint32, std::vector<GatherSpawn>>> _goSpawns;
    std::unordered_map<uint32, std::set<uint32>> _itemFromGo;            // предмет -> виды объектов
    std::unordered_map<uint8, std::pair<uint32, uint32>> _raceAccounts;   // раса -> {bnet, игровая}
    uint32 _warmupMs = 0;
    uint32 _throttleMs = 0;
    bool _bootstrapped = false;
    bool _planTried = false;        // план строится один раз за запуск, даже если отклонён
};
}

} // namespace Constellation

// КТО ЧТО НОСИТ — ПЕЧАТАЕМ ДАННЫЕ ДО ТОГО, КАК ПО НИМ ПРОДАВАТЬ (Кодекс: DB2 в репозитории
// нет, продажа необратима). Маска брони каждого класса и наличие каждого оружейного навыка у
// классов. Зовётся из OnStartup БЕЗУСЛОВНО — и при выключенном модуле, чтобы читаться в
// журнале пробного мира до подмены живого.
void LogWhoWearsWhat()
{
    // КТО ЧТО НОСИТ — ПЕЧАТАЕМ ДАННЫЕ, ПРЕЖДЕ ЧЕМ ПО НИМ ПРОДАВАТЬ (Кодекс: DB2 в
    // репозитории нет, продажа необратима). Маска брони каждого класса и наличие каждого
    // оружейного навыка у классов; читается в журнале пробного мира до подмены живого.
    for (uint32 cls = 1; cls <= 13; ++cls)
    {
        ChrClassesEntry const* e = sChrClassesStore.LookupEntry(cls);
        if (!e)
            continue;
        std::string bits;
        for (uint32 b = 0; b <= 6; ++b)
            bits += (e->ArmorTypeMask & (1u << b)) ? '1' : '0';
        TC_LOG_INFO("server.loading",
            "Constellation НОСИТ класс {}: маска брони 0x{:x}, биты 0..6 = {} (0 прочее, 1 ткань, 2 кожа, 3 кольчуга, 4 латы, 5 косметика, 6 щит)",
            cls, e->ArmorTypeMask, bits);
    }
    static std::pair<uint32, char const*> const weaponSkills[] = {
        { 43, "мечи" }, { 55, "двуручные мечи" }, { 44, "топоры" }, { 172, "двуручные топоры" },
        { 54, "дробящее" }, { 160, "двуручное дробящее" }, { 229, "древковое" }, { 136, "посохи" },
        { 173, "кинжалы" }, { 473, "кистевое" }, { 45, "луки" }, { 46, "ружья" }, { 226, "арбалеты" },
        { 228, "жезлы" }, { 2152, "глефы" }, { 356, "рыбалка" }, { 433, "щит" } };
    for (auto const& [skill, name] : weaponSkills)
    {
        std::string who;
        for (SkillRaceClassInfoEntry const* r : sDB2Manager.GetSkillRaceClassInfo(skill))
            who += Trinity::StringFormat("{:x} ", uint32(r->ClassMask));
        TC_LOG_INFO("server.loading", "Constellation НОСИТ навык {} ({}): маски классов [{}]",
            skill, name, who.empty() ? std::string("строк нет") : who);
    }
}

class constellation_worldscript : public WorldScript
{
public:
    constellation_worldscript() : WorldScript("constellation_worldscript") { }

    void OnStartup() override
    {
        TC_LOG_INFO("server.loading", "Constellation {} [{}] - {}", CONSTELLATION_VERSION,
            CONSTELLATION_BUILD_STAMP,
            Constellation::Cfg().Enable ? "enabled" : "present but disabled (Constellation.Enable = 0)");
        LogWhoWearsWhat();
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
        Constellation::Plan::Planner::Instance()->Tick(diff);
    }

    void OnShutdown() override
    {
        Constellation::Plan::Planner::Instance()->OnShutdown();
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
            { "zones",   HandleZones,   rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "spell",   HandleSpell,   rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "itemsrc", HandleItemSrc, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "trig",    HandleTrig,    rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
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

    // .constellation spell <id> — эффекты заклинания глазами ядра (Spell.db2 в памяти сервера)
    static bool HandleSpell(ChatHandler* handler, uint32 spellId)
    {
        SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!si)
        {
            handler->PSendSysMessage("Constellation: заклинания %u нет", spellId);
            return true;
        }
        for (SpellEffectInfo const& e : si->GetEffects())
        {
            std::string line = Trinity::StringFormat(
                "Constellation ЗАКЛИНАНИЕ {} «{}» эффект {}: тип {}, аура {}, триггер {}, предмет {}, значение {}, misc {} {}",
                spellId, si->SpellName->Str[LOCALE_enUS], uint32(e.EffectIndex), uint32(e.Effect), uint32(e.ApplyAuraName),
                e.TriggerSpell, e.ItemType, e.CalcValue(), e.MiscValue, e.MiscValueB);
            TC_LOG_INFO("server.worldserver", "{}", line);
            handler->PSendSysMessage("%s", line.c_str());
        }
        handler->PSendSysMessage("Constellation: фокус %u, требует предмета класса %d подкласса %d", si->RequiresSpellFocus,
            int32(si->EquippedItemClass), int32(si->EquippedItemSubClassMask));
        return true;
    }

    // .constellation itemsrc <item> — какие заклинания создают этот предмет (CREATE_ITEM / CREATE_LOOT)
    static bool HandleItemSrc(ChatHandler* handler, uint32 itemId)
    {
        // обход хранилища ядра, а не перебор номеров до потолка (Кодекс): полно и без дубликатов вариантов
        uint32 found = 0;
        sSpellMgr->ForEachSpellInfo([&](SpellInfo const* si)
        {
            if (si->Difficulty != DIFFICULTY_NONE || found >= 20)
                return;
            for (SpellEffectInfo const& e : si->GetEffects())
                if (e.ItemType == itemId)
                {
                    ++found;
                    std::string line = Trinity::StringFormat("Constellation ИСТОЧНИК предмета {}: заклинание {} «{}» эффект {} тип {}",
                        itemId, si->Id, si->SpellName->Str[LOCALE_enUS], uint32(e.EffectIndex), uint32(e.Effect));
                    TC_LOG_INFO("server.worldserver", "{}", line);
                    handler->PSendSysMessage("%s", line.c_str());
                }
        });
        handler->PSendSysMessage("Constellation: заклинаний, создающих %u: %u", itemId, found);
        return true;
    }

    // .constellation trig <spell> — кто ведёт к этому заклинанию: триггер эффекта, обучение, значение скрипта
    static bool HandleTrig(ChatHandler* handler, uint32 target)
    {
        // обход хранилища ядра (Кодекс); обучение идёт через TriggerSpell, совпадение по значению
        // скрипта/пустышки — эвристика без кастера и так и помечено
        uint32 found = 0;
        sSpellMgr->ForEachSpellInfo([&](SpellInfo const* si)
        {
            if (si->Difficulty != DIFFICULTY_NONE || found >= 30)
                return;
            for (SpellEffectInfo const& e : si->GetEffects())
            {
                bool const byTrigger = e.TriggerSpell == target;
                bool const byValue = (e.Effect == SPELL_EFFECT_SCRIPT_EFFECT || e.Effect == SPELL_EFFECT_DUMMY)
                    && uint32(e.CalcValue()) == target;
                if (!byTrigger && !byValue)
                    continue;
                ++found;
                std::string line = Trinity::StringFormat("Constellation ВЕДЁТ к {}: заклинание {} «{}» эффект {} тип {} ({})",
                    target, si->Id, si->SpellName->Str[LOCALE_enUS], uint32(e.EffectIndex), uint32(e.Effect), byTrigger ? "триггер" : "значение, эвристика");
                TC_LOG_INFO("server.worldserver", "{}", line);
                handler->PSendSysMessage("%s", line.c_str());
            }
        });
        handler->PSendSysMessage("Constellation: ведущих к %u: %u", target, found);
        return true;
    }

    static bool HandleZones(ChatHandler* handler)
    {
        return Constellation::Manager::Instance()->DumpZones(handler);
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
