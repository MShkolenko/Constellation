/*
 * Constellation — the engine: a bid queue and one tick.
 *
 * Contract: homelab/.agent/design/constellation-engine/engine-spec-v1.md §4 as amended by v2 and
 * v3 §2″. Three Codex passes on the spec, four on the door, and one on this tick that found six
 * gaps — every one of them a policy the spec required and the first draft only declared.
 *
 * WHAT THIS IS. Actions are never called; they are bid. A trigger that fires pushes its handlers
 * with a relevance, strategies push their defaults every tick, multipliers can scale or veto,
 * and the highest surviving bid executes. Failure pushes alternatives slightly above the failed
 * bid so a retry wins the next round without starving anything more important.
 *
 * WHO OWNS WHICH MODE IS NOT DECIDED HERE. The module knows its own Behavior enum; the engine
 * does not and must not guess at it. `Constellation.cpp` decides when to call Tick, in one place,
 * behind Cfg().Engine, and passes its mode epoch so the engine can tell when somebody else put
 * the companion into an engine-owned mode (§2″).
 *
 * Copyright (C) 2026 Constellation contributors. Licensed under the GNU AGPL v3 — see COPYING.
 */
#ifndef CONSTELLATION_ENGINE_ENGINE_H
#define CONSTELLATION_ENGINE_ENGINE_H

#include "Primitives.h"
#include <memory>
#include <vector>

namespace Constellation::Ai
{
    struct Ctx;

    // §11 — the caps live in Primitives.h beside BidSink, which is the only thing allowed to
    // append. The review found Reserve() declared and never called, and then found that a
    // provider handed a raw vector could push past the cap before Push ever checked it; both
    // are closed by the sink. A bid beyond the cap is DROPPED and counted, because on a host at
    // 9.4 GiB of 14.6 a silent reallocation is worse than a lost bid.
    //
    // Everything a companion carries between ticks, deliberately small.
    struct EngineState
    {
        EngineState()
        {
            Queue.reserve(QUEUE_CAP);
            Scratch.reserve(SCRATCH_CAP);
        }

        std::vector<Bid> Queue;              // reserved once in the constructor, never grown
        std::vector<Bid> Scratch;            // where handlers/alternatives are appended, same rule

        // §3.2 — WHEN THIS COMPANION last checked each trigger. It used to live on the Trigger
        // object, which is shared by all 122: the first to tick marked it checked and the rest
        // were told to skip. A per-minute trigger fired once a minute for the whole roster.
        // Flat, fixed-size, no allocation — one word per trigger per companion.
        uint32 TriggerLastMs[size_t(TriggerId::Count)] = {};

        ActionId Running         = ActionId::None;   // what we chose last tick
        // §10′ — И О ЧЁМ ОНО БЫЛО. Без этого Cancel не может вернуть резервацию именно той
        // точки: после исполнения движок хранил один ActionId и терял всё остальное.
        Subject  RunningAbout;
        float    RunningRel      = REL_IDLE;
        uint32   ReplanAfterMs   = 0;        // §4.4′ — hysteresis, not dice
        uint32   AssignmentEpoch = 0;        // §4.4′ — bumped when NEW work is chosen, and on Reset
        uint32   ModeEpochSeen   = 0;        // §2″ — the module's epoch when we last ticked

        // §9 — WHICH STRATEGIES THIS COMPANION RUNS. One word, not an owning container: the
        // canary of the plan is a strategy set, and a set that costs a vector per companion
        // would have reintroduced the per-tick allocation §11 forbids.
        uint32   StrategyMask    = 0;

        // §11 — reported, because "456 ticks a second is affordable" is an argument, not a
        // measurement. A number that is never printed is a number nobody checks.
        uint32 BidsPushed       = 0;
        uint32 BidsDropped      = 0;         // hit a cap — a defect to find, not a cost to absorb
        uint32 BidsExpired      = 0;
        uint32 ActionsCancelled = 0;
        uint32 ReentriesReset   = 0;         // §2″ — how often somebody else moved us
    };

    class Engine
    {
    public:
        static Engine& Instance();

        // Registration. Called once at load; the registries are shared by every companion
        // because an Action holds no per-companion state — that lives in EngineState and Ctx.
        void Register(std::unique_ptr<Action> action);
        // §9 — OWNERSHIP IS DECLARED HERE, not asked for later. A trigger and a multiplier are
        // registered together with the strategy that owns them, so the engine can build one mask
        // each at Seal() and filter by a bitwise and instead of collecting vectors per tick.
        // Владельцев может быть НЕСКОЛЬКО: маска, а не один идентификатор. Хранилище это
        // допускало с самого начала, а приём — нет, и общий триггер пришлось бы дублировать.
        // Пишется как MaskOf(StrategyId::Quests) или MaskOf(a) | MaskOf(b).
        void Register(std::unique_ptr<Trigger> trigger, uint32 ownerMask);
        void Register(std::unique_ptr<Multiplier> multiplier, uint32 ownerMask);
        void Register(std::unique_ptr<Strategy> strategy);

        Action*  Find(ActionId id) const;
        bool     Ready() const { return _ready; }

        // Closes registration AND validates it: every declared ActionId must have an object, or
        // the engine refuses to become ready. Returns false in that case — the caller must not
        // enable the seam. Also reads CONFIG_INSTANT_TAXI once, because that config turns the
        // flight door into a teleport (Player.cpp:23076).
        bool     Seal();

        // True when the core teleports instead of flying. Flight actions must refuse.
        bool     InstantTaxi() const { return _instantTaxi; }

        // §2 — WHICH FSM BRANCHES THE ENGINE HAS TAKEN OVER, as a raw mode value, because the
        // engine does not know the module's Behavior enum and must not guess at it.
        //
        // Today it owns NOTHING, and that is the point: the seam lands inert. When a branch is
        // migrated, its entry is added here IN THE SAME COMMIT THAT DELETES ITS OLD BODY — and
        // it becomes true for EVERY companion, never for a canary subset. The module's switch
        // has no `default:` arm, so a companion in a mode with a deleted body and an engine that
        // does not own it would do nothing at all, every tick, forever.
        static bool Owns(uint8 /*mode*/) { return false; }

        // §10 — THE SHADOW: the engine chooses and does NOT execute, while the ladder still
        // decides.
        //
        // Spec §9′ named an off-realm replay harness as the gate; the operator approved the live
        // shadow in its place (decisions.md, 2026-09-07). It is cheaper than a canary and
        // stronger: it runs on the WHOLE roster at once and can break nothing, because Execute
        // is never called and the movement tick is never opened.
        //
        // HONEST LIMIT: the shadow shows what would be CHOSEN, not what would SUCCEED. Execute's
        // return is unknowable without running it, so the shadow treats execution as successful —
        // otherwise the alternative and prerequisite branches would diverge from the real ones at
        // the first failure and there would be nothing left to compare.
        enum class Run : uint8 { Decide, Shadow };

        // §4.1 — one tick. `modeEpoch` is the module's counter of mode changes for this
        // companion; if it moved since we last ticked, somebody else put us here and every bit
        // of our state is stale (§2″). Returns true if an action executed.
        bool Tick(EngineState& st, Ctx& ctx, uint32 modeEpoch, Run run = Run::Decide);

        // §2″ — give up the work, keep the mode, decide again next tick. THIS IS NOT A HANDOFF,
        // and the naming matters: calling it one is what produced a contract that contradicted
        // itself in spec v2.
        //
        // §10′ — this is also the ONE lifecycle point. When reservations and the value registry
        // exist they are released and invalidated HERE and nowhere else; the review is right that
        // adding either without extending Reset would break v2/v3.
        void Reset(EngineState& st, Ctx& ctx, CancelReason why);

        // §10′ — THE SAME ENDING, WITHOUT A WORLD TO END IT IN.
        //
        // `Reset` needs a `Ctx` because it calls `Action::Cancel`. Two paths have no player and
        // therefore no Ctx: a companion whose session is already gone, and the config being
        // switched off. Those used to assign a fresh `EngineState` over the old one — which
        // works today only because nothing outside the struct is owned yet.
        //
        // This is the named place where that stops being true. When reservations arrive (plan
        // step 28) they are held OUTSIDE the companion, in a registry, and dropping the struct
        // would leak them silently. Releasing them belongs HERE, not in `Reset`, because these
        // are exactly the paths `Reset` cannot serve.
        void Discard(EngineState& st);

    private:
        Engine() = default;

        bool  Push(EngineState& st, std::vector<Bid> const& bids, float forced, uint32 nowMs);
        float MultipliedRelevance(Action& action, Ctx& ctx, uint32 strategyMask, float relevance,
                                  char const** vetoedBy) const;
        // Returns the winning bid's index AND the score it won on. The score leaves the function
        // because calling Score() a second time before executing could return a different number,
        // and then what executes is not what was chosen. `outScore` excludes the stickiness
        // bonus deliberately — see the comment in the body.
        size_t Choose(EngineState& st, Ctx& ctx, uint32 nowMs, float& outScore) const;
        void  LogChoice(EngineState& st, Ctx& ctx, Action const& chosen, float relevance,
                        Run run) const;

        std::vector<std::unique_ptr<Action>>     _actions;      // indexed by ActionId
        // ОДИН ВЕКТОР, А НЕ ДВА ПАРАЛЛЕЛЬНЫХ. Раньше объект и маска владельца лежали в разных
        // векторах и добавлялись двумя push_back подряд: они МОГЛИ разойтись, и утверждение в
        // Seal() только заметило бы это, а не предотвратило. Пара не расходится по устройству.
        template <class T> struct Owned
        {
            std::unique_ptr<T> Obj;
            uint32             Owners = 0;
        };

        std::vector<Owned<Trigger>>    _triggers;
        std::vector<Owned<Multiplier>> _multipliers;
        std::vector<std::unique_ptr<Strategy>>   _strategies;
        bool _ready       = false;
        bool _instantTaxi = false;           // read once at Seal; see Seal()'s comment

        // §9 — ОТКАЗ В РЕГИСТРАЦИИ НЕ ИСЧЕЗАЕТ БЕССЛЕДНО. Регистрация идёт до того, как
        // логирование заведомо настроено, поэтому кричать на месте нельзя — но и молчать
        // нельзя: отвергнутый триггер выглядит как ненаписанный, а Seal() при этом сказал бы
        // «готов». Считаем здесь, докладываем и отказываем в готовности там, где есть журнал.
        uint32 _rejected = 0;
    };
}

#endif // CONSTELLATION_ENGINE_ENGINE_H
