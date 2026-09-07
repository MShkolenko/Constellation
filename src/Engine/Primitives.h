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
    struct Bid
    {
        ActionId Action    = ActionId::None;
        float    Relevance = REL_IDLE;
        uint32   CreatedMs = 0;
        bool     SkipPrerequisites = false;

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
        bool   Computed = false;

        void Invalidate() { Computed = false; }
    };

    template <class T>
    class Value
    {
    public:
        explicit Value(uint32 intervalMs = 0) : _intervalMs(intervalMs) { }
        virtual ~Value() = default;

        Value(Value const&) = delete;
        Value& operator=(Value const&) = delete;

        // The slot is the companion's; this object owns nothing that varies between them.
        // nowMs is passed in rather than read from a clock so the whole tick shares one
        // timestamp and two values cannot disagree about "now".
        T const& Get(Ctx& ctx, Slot<T>& slot, uint32 nowMs) const
        {
            if (!slot.Computed || !_intervalMs || nowMs - slot.LastMs >= _intervalMs)
            {
                Calculate(ctx, slot.Buffer);     // fills, never allocates a new container
                slot.LastMs   = nowMs;
                slot.Computed = true;
            }
            return slot.Buffer;
        }

        uint32 IntervalMs() const { return _intervalMs; }

    protected:
        // Fills `out`, which the caller owns and which keeps its capacity between recomputes.
        // A collection-valued implementation clears and refills; it must not assign a fresh
        // container, and it must cap its own growth the way BidSink caps bids.
        virtual void Calculate(Ctx& ctx, T& out) const = 0;

    private:
        uint32 _intervalMs = 0;
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

        void Add(ActionId action, float relevance, bool skipPrerequisites = false)
        {
            if (_into.size() >= SCRATCH_CAP)
            {
                ++_dropped;
                return;
            }
            _into.push_back(Bid{ action, relevance, 0, skipPrerequisites });
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

        virtual bool Useful(Ctx&)   { return true; }
        virtual bool Possible(Ctx&) { return true; }
        virtual bool Execute(Ctx&) = 0;

        // §10′ — idempotent, and callable on an action that never started.
        virtual void Cancel(Ctx&, CancelReason) { }

        virtual void Prerequisites(BidSink&) const { }
        virtual void Alternatives(BidSink&)  const { }
        virtual void Continuers(BidSink&)    const { }

        // §4.3′ — the class of work decides how distance enters, and an action that is NOT
        // distance-sensitive must say so rather than inherit a falloff.
        enum class Cost : uint8
        {
            None,       // emergency: distance irrelevant
            Linear,     // a small cost — a hand-in already earned is still worth walking to
            Falloff,    // 1/(1 + d/(400 + level*10)) — choosing between candidates
            Steep,      // follow-the-owner
        };
        virtual Cost DistanceCost() const { return Cost::Falloff; }

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
    class Strategy
    {
    public:
        explicit Strategy(char const* name) : _name(name) { }
        virtual ~Strategy() = default;

        char const* Name() const { return _name; }

        virtual void Triggers(std::vector<TriggerId>&) const { }
        virtual void DefaultBids(BidSink&)             const { }
        virtual void Multipliers(std::vector<Multiplier const*>&) const { }

    private:
        char const* _name;
    };
}

#endif // CONSTELLATION_ENGINE_PRIMITIVES_H
