/*
 * Constellation — the engine's primitives. First cut of the port specified in
 * homelab/.agent/design/constellation-engine/engine-spec-v1.md as amended by v2 and v3,
 * three Codex passes, the last one BUILD THE FIRST CUT AS WRITTEN.
 *
 * Every declaration below names the spec section it implements. Do not "improve" one here
 * without a new pass there — that is the rule Plan.h already carries, and it is what keeps a
 * spec from becoming decoration.
 *
 * Nothing in this header touches the world. Writing is `ClientAct` and only `ClientAct` (§6′).
 *
 * Copyright (C) 2026 Constellation contributors. Licensed under the GNU AGPL v3 — see COPYING.
 */
#ifndef CONSTELLATION_ENGINE_PRIMITIVES_H
#define CONSTELLATION_ENGINE_PRIMITIVES_H

#include "Context.h"   // §12 — Value<T>::Get читает ctx.World.MapId(); цикла нет
#include "Define.h"
#include <string>
#include <vector>

namespace Constellation::Ai
{
    struct Ctx;                                     // Context.h — const views + ClientAct + NowMs

    // ---------------------------------------------------------------------------------------
    // §3.5 — typed ids, not strings.
    //
    // The reference binds everything by std::string through AiObjectContext. Flexible, and the
    // review is right that it is refactor-unsafe: a renamed action fails at runtime, not at
    // build time. Here a misspelled id is a compile error, and the name still exists for the
    // operator-facing РЕШЕНИЕ line that §5 makes mandatory.
    // ---------------------------------------------------------------------------------------

    // The thirteen rungs of the Idle branch, in the order the current `if` chain tries them.
    // THAT ORDER IS NOT CARRIED OVER: it is replaced by utility minus travel cost (§4.3′), and
    // this list is an inventory, not a priority.
#define CONSTELLATION_ACTIONS(X)                                            \
    X(None,               "нет действия")                                   \
    X(FleeCombat,         "отхожу, драться нечем")                          \
    X(EndEmptyCombat,     "в бою без нападающих")                           \
    X(TurnInQuest,        "сдать квест")                                    \
    X(Rest,               "перевести дух")                                  \
    X(VisitVendor,        "к торговцу")                                     \
    X(KillObjective,      "бить цель задания")                              \
    X(OpenCageForTarget,  "открыть клетку цели")                            \
    X(TalkToTarget,       "поговорить, а не драться")                       \
    X(GatherObjective,    "нужное лежит на земле")                          \
    X(TravelToObjective,  "идти за целью задания")                          \
    X(TakeQuestNearby,    "взять квест рядом")                              \
    X(SeekGiverByMap,     "идти к квестодателю по карте")                    \
    X(FollowOwner,        "идти за хозяином")

    // Housekeeping that the ladder skeleton hid (action-matrix.md, "what reading the skim rows
    // changed"). These are NOT candidates competing for selection — they fire on their own
    // trigger every scan, whatever the companion chooses. Three of the four send packets.
#define CONSTELLATION_TRIGGERS(X)                                           \
    X(StandingInAreaTrigger, "стою в зоне осмотра")                         \
    X(LateCreditAppeared,    "счётчик вырос сам")                           \
    X(PassedFlightMaster,    "мимо полётного мастера")                      \
    X(PassedInnkeeper,       "мимо трактирщика")                            \
    X(InCombatUnableToFight, "в бою, а драться нечем")                      \
    X(HasCompletedQuest,     "есть что сдать")                              \
    X(NeedsRest,             "надо перевести дух")                          \
    X(VendorWorthATrip,      "к торговцу стоит идти")

    // §9 — WHAT A COMPANION IS CURRENTLY DOING WITH ITS LIFE, as a set of named strategies.
    //
    // A strategy owns triggers and multipliers; enabling or disabling one changes what a
    // companion may bid on and what may veto it, without touching any action. This is also how
    // the canary is expressed (plan §9): a cohort is a strategy set, not a hardcoded list.
#define CONSTELLATION_STRATEGIES(X)                                         \
    X(Survival,     "выживание")                                            \
    X(Quests,       "квесты")                                               \
    X(Combat,       "бой")                                                  \
    X(Trade,        "торговля")                                             \
    X(Housekeeping, "попутное")                                             \
    X(Follow,       "следование")

    // §12 — ЗНАЧЕНИЯ: всё, что дорого считать и глупо считать каждый такт.
    //
    // ЯРУС ОДИН, И ЭТО РЕШЕНИЕ, А НЕ УПУЩЕНИЕ. План v2 (поправка 10) требовал
    // второй ярус «на карту» со своим складом у `Manager`. Он не строится: факты карты
    // УЖЕ посчитаны один раз при загрузке (`Constellation.cpp:7786-7797`, `_givers`), а то, что
    // план назвал `GiversByMap`, фактом карты не является — `FindGiverByMap` (`:10372`) сортирует
    // по расстоянию ОТ ИГРОКА, фильтрует по ЕГО фракции и ЕГО отсрочкам.
    // Решение и доказательство: `homelab/.agent/memory/decisions.md`, 2026-09-08.
#define CONSTELLATION_VALUES(X)                                              \
    /*  имя              текст                     тип буфера  */         \
    X(CompletedTurnIns, "готовые к сдаче",       TurnInList)                    \
    X(GiversInSight,    "квестодатели в обзоре", GiverSightList)                \
    X(GiversByIndex,    "квестодатели по карте", GiverIndexList)

    enum class ValueId : uint8
    {
#define CONSTELLATION_VALUE_ENUM(name, text, type) name,
        CONSTELLATION_VALUES(CONSTELLATION_VALUE_ENUM)
#undef CONSTELLATION_VALUE_ENUM
        Count
    };

    enum class StrategyId : uint8
    {
#define CONSTELLATION_STRATEGY_ENUM(name, text) name,
        CONSTELLATION_STRATEGIES(CONSTELLATION_STRATEGY_ENUM)
#undef CONSTELLATION_STRATEGY_ENUM
        Count
    };

    static_assert(size_t(StrategyId::Count) <= 32, "маска стратегий — одно слово");

    // Сдвиг на неизвестное число — неопределённое поведение, а StrategyId приходит из cast'а.
    // Вне диапазона возвращаем ноль: такая стратегия просто не включится ни у кого, что видно
    // в поведении, в отличие от сдвига на 200.
    inline constexpr uint32 MaskOf(StrategyId id)
    {
        return id < StrategyId::Count ? (1u << uint32(id)) : 0u;
    }

    enum class ActionId : uint8
    {
#define CONSTELLATION_ACTION_ENUM(name, text) name,
        CONSTELLATION_ACTIONS(CONSTELLATION_ACTION_ENUM)
#undef CONSTELLATION_ACTION_ENUM
        Count
    };

    enum class TriggerId : uint8
    {
#define CONSTELLATION_TRIGGER_ENUM(name, text) name,
        CONSTELLATION_TRIGGERS(CONSTELLATION_TRIGGER_ENUM)
#undef CONSTELLATION_TRIGGER_ENUM
        Count
    };

    char const* NameOf(ActionId id);
    char const* NameOf(TriggerId id);
    char const* NameOf(StrategyId id);
    char const* NameOf(ValueId id);

    // ---------------------------------------------------------------------------------------
    // §4.2 — the relevance scale, in our own names, so a number in a log is readable.
    // ---------------------------------------------------------------------------------------
    inline constexpr float REL_IDLE       =  0.0f;
    inline constexpr float REL_BACKGROUND =  5.0f;
    inline constexpr float REL_NORMAL     = 10.0f;
    inline constexpr float REL_HIGH       = 20.0f;
    inline constexpr float REL_MOVE       = 30.0f;
    inline constexpr float REL_INTERRUPT  = 40.0f;
    inline constexpr float REL_EMERGENCY  = 90.0f;

    // §4.1 — the offsets are the reference's, unchanged. They make a retry win the NEXT round
    // rather than starve, without letting it outrank a genuinely more important bid.
    inline constexpr float REL_PREREQ_BUMP = 0.002f;   // the prerequisite goes above us
    inline constexpr float REL_REQUEUE     = 0.001f;   // and we come back just under it
    inline constexpr float REL_ALTERNATIVE = 0.003f;   // a failure's alternatives beat both

    // ---------------------------------------------------------------------------------------
    // §4.1 — a bid. Actions are never called, they are bid.
    //
    // Trivially copyable and free of owning members ON PURPOSE (§11): the live host runs at
    // 9.4 GiB of 14.6 and has been OOM-killed seven times, so the queue is a reused vector of
    // these and nothing here may allocate.
    // ---------------------------------------------------------------------------------------
    // §8 — the four kinds of thing a bid can be about. Kept as a tagged value rather than four
    // parallel fields so that "which of these is set" is never a question a reader has to answer
    // by inspecting emptiness.
    class Subject
    {
    public:
        enum class Kind : uint8
        {
            None,       // the bid is about the companion itself: rest, flee, follow the owner
            Unit,       // a creature — a giver, an ender, a quest target
            Object,     // a gameobject — a cage, a gather node, a door
            Spawn,      // a spawn id: a point that may have no object loaded right now
            Quest,      // a quest number: a hand-in whose ender is chosen later
        };

        Subject() = default;

        // THE ONLY WAYS TO BUILD ONE, AND THE PAYLOAD IS PRIVATE.
        //
        // The first version left the three fields public with factories beside them. Factories
        // alone enforce nothing: `s.What = Kind::Spawn` next to a GUID compiles and produces a
        // subject that lies about itself, and the reader that trusts the tag gets a zero while
        // the reader that trusts the payload gets a stale guid. Neither would look like a bug.
        static Subject OfUnit(ObjectGuid g)   { Subject s; s._what = Kind::Unit;   s._guid = g; return s; }
        static Subject OfObject(ObjectGuid g) { Subject s; s._what = Kind::Object; s._guid = g; return s; }
        static Subject OfSpawn(uint32 id)     { Subject s; s._what = Kind::Spawn;  s._id  = id; return s; }
        static Subject OfQuest(uint32 id)     { Subject s; s._what = Kind::Quest;  s._id  = id; return s; }

        Kind What() const { return _what; }
        bool IsNone() const { return _what == Kind::None; }

        // A payload read against the wrong kind returns nothing rather than something stale.
        ObjectGuid Guid() const
        {
            return (_what == Kind::Unit || _what == Kind::Object) ? _guid : ObjectGuid::Empty;
        }
        uint32 Id() const
        {
            return (_what == Kind::Spawn || _what == Kind::Quest) ? _id : 0;
        }

        bool operator==(Subject const& o) const
        {
            return _what == o._what && _guid == o._guid && _id == o._id;
        }

    private:
        Kind       _what = Kind::None;
        ObjectGuid _guid;
        uint32     _id   = 0;
    };

    // -------------------------------------------------------------------------------------------
    // §14 — ОТСРОЧКИ: ОДНА ТАБЛИЦА ВМЕСТО ВОСЬМИ НАБОРОВ.
    //
    // Лестница завела восемь отдельных контейнеров, по одному на случай: `SellRefused`,
    // `EquipRefused`, `TurnInBackoff`, `SeekBackoff`, `GiverUnreachable`, `TalkBackoff`,
    // `TalkUnreachable`, `QuestRefused`. Каждый со своим ключом, своим сроком и своей политикой
    // очистки — а два из них не чистятся вовсе. Здесь это одна вещь.
    //
    // КЛЮЧ — ТРОЙКА, И ЭТО НЕ ИЗБЫТОЧНОСТЬ. Один и тот же NPC бывает одновременно непригоден для
    // взятия квеста, годен для сдачи другого, недоступен для разговора и пригоден как торговец.
    // Ключ по одному `Subject` склеил бы четыре разных запрета в один; ключи лестницы это и
    // подтверждают — `EquipRefused` ключуется парой «гуид + слот», `TalkBackoff` по виду,
    // `TalkUnreachable` по гуиду. `Detail` и есть место для такой пары.
    // -------------------------------------------------------------------------------------------
#define CONSTELLATION_BACKOFFS(X)                                                     \
    /*  имя             текст                       */                                \
    X(None,             "нет")                                                        \
    X(NothingOffered,   "предложить нечего")                                          \
    X(Unreachable,      "не дойти")                                                   \
    X(CoreRefused,      "ядро отказало")

    enum class BackoffKind : uint8
    {
#define CONSTELLATION_BACKOFF_ENUM(name, text) name,
        CONSTELLATION_BACKOFFS(CONSTELLATION_BACKOFF_ENUM)
#undef CONSTELLATION_BACKOFF_ENUM
        Count
    };

    // Определение inline, а не в .cpp: таблица короткая, а имя нужно и журналу движка, и
    // диагностике действия — лишний узел компоновки здесь ничего не купил бы.
    inline char const* NameOf(BackoffKind k)
    {
        switch (k)
        {
#define CONSTELLATION_BACKOFF_NAME(name, text) case BackoffKind::name: return text;
            CONSTELLATION_BACKOFFS(CONSTELLATION_BACKOFF_NAME)
#undef CONSTELLATION_BACKOFF_NAME
            default: return "?";
        }
    }

    // Ёмкость НЕ УГАДАНА и угадана быть не должна: она берётся по наблюдаемому пику живых ключей
    // на спутника, а счётчики `BackoffFull` и `BackoffEvictedLive` в `EngineState` для того и
    // заведены. Шестнадцать — начальное значение при составе из восьми; при возврате к 122 его
    // надо ПЕРЕПРОВЕРИТЬ по этим счётчикам, а не по ощущению.
    inline constexpr size_t BACKOFF_CAP = 16;

    struct BackoffKey
    {
        BackoffKind Kind   = BackoffKind::None;
        Subject     About;
        uint8       Detail = 0;     // слот экипировки, номер пункта меню — то, чем ключи различаются

        bool operator==(BackoffKey const& o) const
        {
            return Kind == o.Kind && Detail == o.Detail && About == o.About;
        }
    };

    struct Bid
    {
        ActionId Action    = ActionId::None;
        float    Relevance = REL_IDLE;
        uint32   CreatedMs = 0;
        bool     SkipPrerequisites = false;

        // §8 — WHAT THE BID IS ABOUT. Without it there is no path from "the scan found this
        // giver" to "the action goes to that giver": an Action is one shared object, so it
        // cannot hold a per-companion subject.
        //
        // I refused this once, arguing it would quadruple a struct kept at sixteen bytes. The
        // arithmetic does not survive: with padding the bid lands near 64 bytes, and at a cap of
        // thirty-two over 122 companions that is about 250 KiB against 9.3 GiB. The rule that
        // matters is "nothing allocates per tick", which this respects; I had applied a proxy
        // for that rule where the proxy does not hold.
        //
        // A GUID alone is not enough either: the matrix needs a gather point addressed by its
        // spawn id and a quest addressed by its number, neither of which is an object in the
        // world at the moment the bid is made.
        Subject  About;

        // §4.3′ — THE SCORE IS COMPUTED ONCE PER BID PER TICK, and it is cached here.
        //
        // The first fix made selection carry the winning score to execution, which stopped
        // "what ran" from differing from "what won". It did not stop the rescanning: Choose()
        // runs inside the iteration loop up to eight times a tick and re-scored every remaining
        // bid each pass, so an action's Score() could be invoked eight times — eight world scans
        // for one decision, on the thread that is the bottleneck.
        //
        // The stamp is what makes it exact rather than approximate: a bid pushed mid-loop (an
        // alternative, a continuer) has no score yet and gets one on the next pass; a bid already
        // scored this tick is never scored again.
        float    Score     = REL_IDLE;
        uint32   ScoredMs  = 0;            // 0 = never scored; the tick's NowMs otherwise
        bool     Scored    = false;        // NowMs can legitimately be 0 on a fresh world

        bool operator<(Bid const& o) const { return Relevance < o.Relevance; }
    };

    // §10′ — why an action stopped. Never merely "it ended": the reason drives the disposition
    // of reservations, and an unexplained ending is how ghost actions appear.
    enum class CancelReason : uint8
    {
        Finished,           // the ordinary success
        Failed,             // tried and did not work
        SubjectGone,        // the target, giver, object or spawn disappeared
        Died,
        LoggedOut,
        Dismissed,
        MapChanged,
        EngineDisabled,     // Cfg().Engine turned off mid-flight
        EnteredFromOutside, // §2″ epoch check: somebody else put us in this mode
    };

    // ---------------------------------------------------------------------------------------
    // Разброс интервала значения по гуиду. Число не выбрано здесь — оно взято у ветки
    // `Idle`, где пережило измерение на живом составе (`1000 + guid % 250`).
    inline constexpr uint32 VALUE_JITTER_MS = 250;

    // §3.1 — Value<T>: computed once, cached with an interval.
    //
    // The module proved it needs one: `c.VendScanMs = 5000` exists because a 100-yard grid sweep
    // per tick, at four ticks a second and 122 companions, took the world thread to 99 % of a
    // core and the FSM nearly stopped. That throttle was hand-rolled at ONE site.
    //
    // THE FIRST DRAFT OF THIS CLASS WAS WRONG IN TWO WAYS, AND BOTH WOULD HAVE BEEN MULTIPLIED
    // BY TWELVE HAD ANY VALUE BEEN WRITTEN AGAINST IT.
    //
    //   1. The cache and the timestamp lived on the Value object. Values are registered once and
    //      shared by every companion, so the FIRST companion to tick would compute, and the other
    //      121 would read its answer and be told the interval had not expired. A value meaning
    //      "the nearest quest giver TO ME" would have returned somebody else's.
    //
    //   2. `Calculate` returned T by value. Every value the port needs is a collection, so each
    //      recompute would heap-allocate a fresh vector and free the previous one — on the order
    //      of a thousand alloc/free pairs a second at 122 companions, on the host that has been
    //      OOM-killed seven times. The old comment said "a reference, not a copy", which was true
    //      of the RETURN and said nothing about the churn on the recompute path.
    //
    // So: the Value object is SHARED and STATELESS. The cache, the timestamp and the buffer are
    // per companion, supplied by the caller as a `Slot<T>` living in EngineState, and Calculate
    // FILLS that buffer instead of returning one.
    // ---------------------------------------------------------------------------------------
    template <class T>
    struct Slot
    {
        T      Buffer{};
        uint32 LastMs   = 0;
        // НА КАКОЙ КАРТЕ ЭТО СЧИТАЛОСЬ. Сегодня спутник карту внутри такта не меняет:
        // телепортные ветки выходят выше (`Constellation.cpp:2109`), а смена карты проходит
        // через `EngineReset` ДО построения `Ctx` (`:2661`). Но это свойство сегодняшнего
        // кода, а не гарантия интерфейса: в двери есть `ActivateTaxi` (`ClientAct.h:104`), а
        // `Tick` после `Execute` зовёт `Continuers`, которые вполне могут читать значения
        // (Кодекс, состязательный проход по разбору, пункт 2). Один `uint32` на слот дешевле
        // правила, которое некому проверить.
        uint32 MapId    = 0;
        bool   Computed = false;

        void Invalidate() { Computed = false; }
    };

    // НЕШАБЛОННОЕ ОСНОВАНИЕ, И ОНО НУЖНО НЕ ДЛЯ КРАСОТЫ. `Value<T>` разных T — не
    // родственники, так что без общего основания их негде сложить и нечем проверить, что
    // у каждого объявленного `ValueId` есть ровно один поставщик. Именно этой проверки
    // не хватало `Seal()` по разбору Кодекса (пункт 5): сегодня он смотрит только действия.
    class ValueBase
    {
    public:
        ValueBase(ValueId id, uint32 intervalMs) : _id(id), _intervalMs(intervalMs) { }
        virtual ~ValueBase() = default;

        ValueBase(ValueBase const&) = delete;
        ValueBase& operator=(ValueBase const&) = delete;

        ValueId     Id() const         { return _id; }
        char const* Name() const       { return NameOf(_id); }
        uint32      IntervalMs() const { return _intervalMs; }

    private:
        ValueId _id;
        uint32  _intervalMs;
    };

    template <class T>
    class Value : public ValueBase
    {
    public:
        Value(ValueId id, uint32 intervalMs) : ValueBase(id, intervalMs) { }

        // The slot is the companion's; this object owns nothing that varies between them.
        // nowMs is passed in rather than read from a clock so the whole tick shares one
        // timestamp and two values cannot disagree about "now".
        T const& Get(Ctx& ctx, Slot<T>& slot, uint32 nowMs) const
        {
            // СМЕНА КАРТЫ ГАСИТ КЭШ БЕЗУСЛОВНО, даже если интервал не истёк. Всё, что
            // считают значения шага 12, привязано к карте: точки появления, расстояния,
            // обзор сетки. Ответ с чужой карты — не устаревший, а бессмысленный.
            uint32 const mapId = ctx.World.MapId();
            // ДЖИТТЕР ПО ГУИДУ, А НЕ ОБЩИЙ ИНТЕРВАЛ. Сто четырнадцать спутников с
            // одинаковым интервалом тикают в одном мировом обновлении и истекают тоже в
            // одном: ровный по времени средний расход и пила в пике. Ветка `Idle` уже
            // разводит свои сканы так же — `1000 + guid % 250` (Constellation.cpp:2591).
            uint32 const interval = IntervalMs()
                ? IntervalMs() + (ctx.World.Guid().GetCounter() % VALUE_JITTER_MS)
                : 0u;
            if (!slot.Computed || slot.MapId != mapId
                || !interval || nowMs - slot.LastMs >= interval)
            {
                Calculate(ctx, slot.Buffer);     // fills, never allocates a new container
                slot.LastMs   = nowMs;
                slot.MapId    = mapId;
                slot.Computed = true;
            }
            return slot.Buffer;
        }

    protected:
        // Fills `out`, which the caller owns and which keeps its capacity between recomputes.
        // A collection-valued implementation clears and refills; it must not assign a fresh
        // container, and it must cap its own growth the way BidSink caps bids — `CappedList`
        // in Values.h does exactly that, and counts what it drops.
        virtual void Calculate(Ctx& ctx, T& out) const = 0;
    };

    // ---------------------------------------------------------------------------------------
    // §11 — hard caps, and the ONLY way a provider may append a bid.
    //
    // The first draft handed providers a raw std::vector<Bid>& and enforced the cap afterwards
    // in Push. The review pointed out the obvious: a provider can push past the cap and
    // reallocate before Push ever sees it. So providers get a BidSink, which owns the check.
    // A bid beyond the cap is dropped and counted; the counter is the defect report.
    // ---------------------------------------------------------------------------------------
    inline constexpr size_t QUEUE_CAP   = 32;
    inline constexpr size_t SCRATCH_CAP = 16;

    class BidSink
    {
    public:
        BidSink(std::vector<Bid>& into, uint32& dropped) : _into(into), _dropped(dropped) { }

        // The subject is part of the bid, not an afterthought: a trigger that found a giver says
        // WHICH giver here, and that is the only channel by which the action learns it.
        void Add(ActionId action, float relevance, Subject about = Subject(),
                 bool skipPrerequisites = false)
        {
            if (_into.size() >= SCRATCH_CAP)
            {
                ++_dropped;
                return;
            }
            Bid b;
            b.Action            = action;
            b.Relevance         = relevance;
            b.SkipPrerequisites = skipPrerequisites;
            b.About             = about;
            _into.push_back(b);
        }

        size_t Size() const { return _into.size(); }

    private:
        std::vector<Bid>& _into;
        uint32&           _dropped;
    };

    // ---------------------------------------------------------------------------------------
    // §3.2 — Trigger. Watches for a state; when it holds, its handlers are bid.
    // ---------------------------------------------------------------------------------------
    class Trigger
    {
    public:
        Trigger(TriggerId id, uint32 intervalMs) : _id(id), _intervalMs(intervalMs) { }
        virtual ~Trigger() = default;

        TriggerId Id() const { return _id; }

        // A trigger is not evaluated every tick — that is what made the hand-rolled throttles
        // necessary in the first place.
        //
        // THE TIMESTAMP IS THE COMPANION'S, NOT THE TRIGGER'S. It used to be a member here, and
        // triggers are registered once and shared by all 122: the first companion to tick would
        // mark the trigger checked and the other 121 would be told to skip it. A trigger that
        // fires once a minute would have fired once a minute FOR THE WHOLE ROSTER.
        bool NeedsCheck(uint32 nowMs, uint32 lastMs) const
        {
            return !_intervalMs || !lastMs || nowMs - lastMs >= _intervalMs;
        }

        uint32 IntervalMs() const { return _intervalMs; }

        virtual bool Check(Ctx& ctx) = 0;

        // Appended through a capped sink rather than returned by value: §11, no per-tick
        // allocation, and the cap is enforced at the append, not after it.
        virtual void Handlers(BidSink& out) const = 0;

    private:
        TriggerId _id;
        uint32    _intervalMs = 0;
    };

    // ---------------------------------------------------------------------------------------
    // §3.3 — Action.
    //
    // Useful() and Possible() stay two questions. The engine treats them differently and that
    // is the whole point: NOT USEFUL drops the bid silently, NOT POSSIBLE pushes the
    // alternatives. "I am already at full health" and "the giver is out of range" need
    // different recoveries, and today Constellation.cpp gives them the same one.
    // ---------------------------------------------------------------------------------------
    class Action
    {
    public:
        explicit Action(ActionId id) : _id(id) { }
        virtual ~Action() = default;

        ActionId Id() const { return _id; }
        char const* Name() const { return NameOf(_id); }

        // EVERY PER-BID METHOD TAKES THE BID, and that is the whole point of §8.
        //
        // The first version put the subject on the bid and handed it only to `Score`. So the
        // engine could PRICE a giver and then execute an action that had no idea which giver it
        // was — the channel was built halfway and was therefore useless. An Action is one shared
        // object; the bid is the only thing that varies per companion, so anything that needs to
        // know "which one" must be given it.
        //
        // The derived bids matter as much: a prerequisite, an alternative or a continuer of
        // "talk to THIS giver" is almost always about the same giver, and without the bid they
        // could not say so.
        virtual bool Useful(Ctx&, Bid const&)   { return true; }
        virtual bool Possible(Ctx&, Bid const&) { return true; }

        // §14 — ПОД КАКИМ ВИДОМ ЗАПРЕТА ХОДИТ ЭТО ДЕЙСТВИЕ. Движок фильтрует ставку по паре
        // {этот вид, предмет ставки} ДО того, как спросит `Useful`, — то есть по ТОЧНОМУ ключу,
        // а не по предмету вообще. Иначе запрет «у этого NPC нечего взять» заодно запретил бы
        // сдать ему другой квест.
        //
        // `None` по умолчанию значит «это действие отсрочек не признаёт», и таких большинство:
        // заводить вид ради одного действия незачем, пока это действие не показало, что умеет
        // выбираться впустую.
        virtual BackoffKind DeferKind() const { return BackoffKind::None; }

        // §14 — И ДЕТАЛЬ КЛЮЧА, ИЗ ТОЙ ЖЕ СТАВКИ. Без неё ключ объявлен тройкой, а строился
        // всегда с нулём — и запись с ненулевой деталью не подавляла бы ставку НИКОГДА, молча
        // (Кодекс, пункт 5). Спрашивается у того же действия, что объявляет вид: тогда оно
        // отвечает на «чем ключуемся» одним кодом и при постановке запрета, и при проверке, и
        // разойтись им негде.
        //
        // Ноль по умолчанию значит «различать нечего»: у сдачи квеста ключ и есть сам квест.
        // Деталь понадобится там, где один предмет несёт несколько независимых запретов —
        // у лестницы это `EquipRefused`, ключ которого пара «гуид + слот».
        virtual uint8 DeferDetail(Bid const&) const { return 0; }
        virtual bool Execute(Ctx&, Bid const&) = 0;

        // §10′ — idempotent, and callable on an action that never started.
        //
        // IT GETS THE SUBJECT, because releasing a reservation means releasing THAT point. The
        // first version passed only the reason: after execution the engine kept the ActionId and
        // dropped everything else, so a spawn-specific hold could not be given back and would
        // have leaked for the life of the world.
        virtual void Cancel(Ctx&, Subject const&, CancelReason) { }

        // CONTRACT for the three providers: they may inspect the world, read values and emit
        // bids. They may NOT act on the world, and they may not acquire or release ownership of
        // anything — no reservation is taken or given back here. `Ctx&` is mutable only because
        // reading a value may recompute it; that is the whole of the licence.
        virtual void Prerequisites(Ctx&, Bid const&, BidSink&) const { }
        virtual void Alternatives(Ctx&, Bid const&, BidSink&)  const { }
        virtual void Continuers(Ctx&, Bid const&, BidSink&)    const { }

        // §4.3′ — THE CLASS OF WORK DECIDES HOW DISTANCE ENTERS, AND THE ACTION IS THE ONLY ONE
        // WHO KNOWS ITS SUBJECT.
        //
        // This was a `DistanceCost()` enum that the engine never read — a declared policy with no
        // implementation, the same shape as the six the review already caught. Worse, it could not
        // have worked: the engine has no idea what an action's subject is, and putting a position
        // on every Bid to tell it would have quadrupled a struct kept at 16 bytes on purpose.
        //
        // So the action scores itself. `Score` is called during SELECTION, not after it, because
        // a falloff applied after the winner is picked changes nothing. Default: unchanged, which
        // is the right answer for emergencies — distance is irrelevant when you are dying.
        //
        // CONTRACT: pure with respect to the tick. It may read values (which is why `ctx` is not
        // const — a value may recompute), but it must not act on the world, and two calls within
        // one tick must return the same number. The engine calls it EXACTLY ONCE per bid per tick
        // and carries the result to execution, so a breach can no longer split "what was chosen"
        // from "what ran" — but it would still make one tick's log disagree with the next.
        //
        // The bid carries the subject (§8), which is how a shared Action prices a per-companion
        // target: `bid.Subject` is the giver, the mob or the object this particular bid is about.
        virtual float Score(Ctx& /*ctx*/, Bid const& bid, float baseRelevance) const
        {
            (void)bid;
            return baseRelevance;
        }

        // The shared shape, so distance-sensitive actions decay identically rather than each
        // inventing a curve. Reach is the reference's own new-quest search radius: a giver at
        // twenty yards beats an objective at two hundred without either being ordered by hand.
        static float Falloff(float yards, uint8 level)
        {
            float const reach = 400.0f + float(level) * 10.0f;
            return 1.0f / (1.0f + (yards > 0.0f ? yards : 0.0f) / reach);
        }

    private:
        ActionId _id;
    };

    // ---------------------------------------------------------------------------------------
    // §3.4 — Multiplier. One virtual; returning 0 vetoes.
    //
    // Cross-cutting rules live here instead of being repeated inside every action: do not act
    // while dead, do not fight what killed us twice, do not leave the level bracket.
    // ---------------------------------------------------------------------------------------
    class Multiplier
    {
    public:
        explicit Multiplier(char const* name) : _name(name) { }
        virtual ~Multiplier() = default;

        char const* Name() const { return _name; }
        virtual float Of(Action const& action, Ctx& ctx) const = 0;

    private:
        char const* _name;
    };

    // ---------------------------------------------------------------------------------------
    // §3.4 — Strategy: contributes triggers, default bids and multipliers.
    // ---------------------------------------------------------------------------------------
    // §9 — A STRATEGY DOES NOT HAND OUT LISTS ANY MORE.
    //
    // It used to answer `Triggers(std::vector<TriggerId>&)` and
    // `Multipliers(std::vector<Multiplier const*>&)` — the last two provider hooks still taking a
    // raw, uncapped vector, which is exactly the shape `BidSink` was invented to replace. Worse,
    // the obvious way to honour a per-companion strategy set would have been to collect those
    // vectors per evaluated bid per tick: a per-tick allocation introduced by the very step that
    // exists to make the ladder tradeable, in the file whose §11 forbids it.
    //
    // Instead ownership is declared at REGISTRATION: a trigger or a multiplier is handed to the
    // engine together with the strategy that owns it. The engine builds one mask per trigger and
    // per multiplier at `Seal()`, and a companion's set is a single `uint32`. Filtering is then a
    // bitwise `and` over registries that already exist — no vector, no allocation, one word of
    // per-companion state instead of an owning container.
    class Strategy
    {
    public:
        Strategy(StrategyId id) : _id(id) { }
        virtual ~Strategy() = default;

        StrategyId  Id() const { return _id; }
        char const* Name() const { return NameOf(_id); }

        // The only thing a strategy still emits, and it goes through the capped sink.
        virtual void DefaultBids(Ctx&, BidSink&) const { }

    private:
        StrategyId _id;
    };
}

#endif // CONSTELLATION_ENGINE_PRIMITIVES_H
