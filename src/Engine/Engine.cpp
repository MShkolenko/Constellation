/*
 * Constellation — the tick.
 *
 * Contract: engine-spec-v1.md §4, amended by v2 (§4.3′ falloff by class, §4.4′ spreading with a
 * salt that changes, §11 no per-tick allocation) and v3 (§2″ Reset is not a handoff).
 *
 * The first draft of this file declared six policies and implemented none of them; the review
 * listed all six. Each is now marked where it lives.
 *
 * Copyright (C) 2026 Constellation contributors. Licensed under the GNU AGPL v3 — see COPYING.
 */
#include "Engine.h"

#include "ClientAct.h"
#include "Context.h"
#include "Log.h"

#include <algorithm>

namespace Constellation::Ai
{
    namespace
    {
        // §4.1 — a bid older than this is answering a world that has moved on.
        constexpr uint32 BID_TTL_MS = 3000;

        // §4.1 — how many bids one tick may work through. The loop stops at the first action
        // that executes, so this bounds the *failures* we are willing to walk past, not the work.
        constexpr uint32 ITERATIONS_PER_TICK = 8;

        // §4.4′ — stickiness, and a re-plan no more often than this. Explicit hysteresis instead
        // of the reference's fixed percentages, so the policy is legible and reproducible.
        constexpr float  STICKINESS = 1.35f;
        constexpr uint32 REPLAN_COOLDOWN_MS = 3000;

        // §4.4′ — bids closer than this are a tie, and a tie is broken by the salt, not by
        // vector order. Vector order would make 114 companions choose identically.
        constexpr float TIE_EPSILON = 0.5f;

        uint32 Elapsed(uint32 now, uint32 then) { return now - then; }   // uint32 wrap is intended

        // §4.4′ — stable within one assignment, different across assignments. splitmix-style
        // mixing so consecutive epochs do not produce consecutive residues.
        uint64 Salt(uint64 guidCounter, uint32 epoch)
        {
            uint64 z = guidCounter ^ (uint64(epoch) << 32) ^ 0x9E3779B97F4A7C15ull;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }
    }

    Engine& Engine::Instance()
    {
        static Engine instance;
        return instance;
    }

    void Engine::Register(std::unique_ptr<Action> action)
    {
        if (_ready || !action)
            return;
        size_t const idx = size_t(action->Id());
        if (_actions.size() <= idx)
            _actions.resize(idx + 1);
        _actions[idx] = std::move(action);
    }

    void Engine::Register(std::unique_ptr<Trigger> trigger)
    {
        if (!_ready && trigger)
            _triggers.push_back(std::move(trigger));
    }

    void Engine::Register(std::unique_ptr<Multiplier> multiplier)
    {
        if (!_ready && multiplier)
            _multipliers.push_back(std::move(multiplier));
    }

    void Engine::Register(std::unique_ptr<Strategy> strategy)
    {
        if (!_ready && strategy)
            _strategies.push_back(std::move(strategy));
    }

    Action* Engine::Find(ActionId id) const
    {
        size_t const idx = size_t(id);
        return idx < _actions.size() ? _actions[idx].get() : nullptr;
    }

    void Engine::Seal()
    {
        _actions.resize(size_t(ActionId::Count));
        _ready = true;
        TC_LOG_INFO("server.worldserver",
            "Constellation ДВИЖОК: действий {}, триггеров {}, множителей {}, стратегий {}",
            _actions.size(), _triggers.size(), _multipliers.size(), _strategies.size());
    }

    bool Engine::Push(EngineState& st, std::vector<Bid> const& bids, float forced, uint32 nowMs)
    {
        bool pushed = false;
        for (Bid const& b : bids)
        {
            float const rel = forced > 0.0f ? forced : b.Relevance;
            if (rel <= 0.0f)
                continue;
            // §11 — THE CAP IS ENFORCED, not hoped for. A push beyond it is dropped and counted;
            // the counter is the defect report.
            if (st.Queue.size() >= QUEUE_CAP)
            {
                ++st.BidsDropped;
                continue;
            }
            st.Queue.push_back(Bid{ b.Action, rel, nowMs, b.SkipPrerequisites });
            ++st.BidsPushed;
            pushed = true;
        }
        return pushed;
    }

    float Engine::MultipliedRelevance(Action& action, Ctx& ctx, float relevance,
                                      char const** vetoedBy) const
    {
        for (auto const& m : _multipliers)
        {
            float const k = m->Of(action, ctx);
            relevance *= k;
            if (relevance <= 0.0f)
            {
                if (vetoedBy)
                    *vetoedBy = m->Name();
                return 0.0f;
            }
        }
        return relevance;
    }

    // §4.4′ — the choice. Stickiness is a SELECTION-TIME score, never a mutation of the queued
    // bid: the first draft multiplied the bid in place, so an unselected running bid grew by 1.35
    // every tick and would have outranked an emergency within seconds. Ties within TIE_EPSILON
    // are broken by the salt so equal companions do not make equal choices.
    size_t Engine::Choose(EngineState& st, Ctx& ctx, uint32 nowMs) const
    {
        bool const sticky = st.Running != ActionId::None && nowMs < st.ReplanAfterMs;

        float  bestScore = -1.0f;
        for (Bid const& b : st.Queue)
        {
            float const s = b.Relevance * ((sticky && b.Action == st.Running) ? STICKINESS : 1.0f);
            if (s > bestScore)
                bestScore = s;
        }

        // Count the ties first, then pick by salt — two passes, no allocation.
        size_t ties = 0;
        for (Bid const& b : st.Queue)
        {
            float const s = b.Relevance * ((sticky && b.Action == st.Running) ? STICKINESS : 1.0f);
            if (bestScore - s <= TIE_EPSILON)
                ++ties;
        }
        size_t pick = ties > 1
            ? size_t(Salt(ctx.World.Guid().GetCounter(), st.AssignmentEpoch) % ties)
            : 0;

        for (size_t i = 0; i < st.Queue.size(); ++i)
        {
            Bid const& b = st.Queue[i];
            float const s = b.Relevance * ((sticky && b.Action == st.Running) ? STICKINESS : 1.0f);
            if (bestScore - s <= TIE_EPSILON)
            {
                if (pick == 0)
                    return i;
                --pick;
            }
        }
        return 0;
    }

    void Engine::LogChoice(EngineState& st, Ctx& ctx, Action const& chosen, float relevance) const
    {
        // §5 — every decision says why. An engine whose choice cannot be read back is worse than
        // the `if` chain it replaces, because at least a chain can be read top to bottom.
        TC_LOG_INFO("server.worldserver",
            "Constellation РЕШЕНИЕ {}: выбрал «{}» {:.2f} (эпоха {}, в очереди {}, сброшено {})",
            ctx.World.Name(), chosen.Name(), relevance, st.AssignmentEpoch,
            st.Queue.size(), st.BidsDropped);
    }

    void Engine::Reset(EngineState& st, Ctx& ctx, CancelReason why)
    {
        // §2″ — give up the work, KEEP the mode. Not a handoff.
        // §10′ — and the one place reservations will be released and values invalidated once
        // those exist. Nothing else may do it.
        if (st.Running != ActionId::None)
        {
            if (Action* a = Find(st.Running))
                a->Cancel(ctx, why);
            ++st.ActionsCancelled;
        }
        st.Queue.clear();
        st.Scratch.clear();
        st.Running       = ActionId::None;
        st.RunningRel    = REL_IDLE;
        st.ReplanAfterMs = 0;
        ++st.AssignmentEpoch;       // §4.4′ — new work, new salt; spreading must not be permanent
    }

    bool Engine::Tick(EngineState& st, Ctx& ctx, uint32 modeEpoch)
    {
        if (!_ready)
            return false;

        uint32 const now = ctx.NowMs;

        // §2″ — somebody else put us in this mode: everything we remember predates it.
        if (modeEpoch != st.ModeEpochSeen)
        {
            if (st.Running != ActionId::None || !st.Queue.empty())
            {
                Reset(st, ctx, CancelReason::EnteredFromOutside);
                ++st.ReentriesReset;
            }
            st.ModeEpochSeen = modeEpoch;
        }

        // §11 — the movement budget is per tick, and nothing may move before it is opened.
        ctx.Act.ResetTick();

        // §4.1 — a bid made three seconds ago is answering a world that has moved on.
        size_t const before = st.Queue.size();
        st.Queue.erase(std::remove_if(st.Queue.begin(), st.Queue.end(),
            [now](Bid const& b) { return Elapsed(now, b.CreatedMs) > BID_TTL_MS; }),
            st.Queue.end());
        st.BidsExpired += uint32(before - st.Queue.size());

        // §4.1 — triggers, each on its own interval. Checking every trigger every tick is what
        // the module's hand-rolled throttles exist to avoid; here the interval is declared.
        for (auto const& t : _triggers)
        {
            if (!t->NeedsCheck(now))
                continue;
            t->Checked(now);
            if (!t->Check(ctx))
                continue;
            st.Scratch.clear();
            BidSink sink(st.Scratch, st.BidsDropped);
            t->Handlers(sink);
            Push(st, st.Scratch, 0.0f, now);
        }

        // §4.1 — and the strategies' defaults, EVERY tick, as the reference's
        // PushDefaultActions does. The first draft forgot them entirely, which would have left
        // a companion with nothing to do the moment no trigger fired.
        for (auto const& s : _strategies)
        {
            st.Scratch.clear();
            BidSink sink(st.Scratch, st.BidsDropped);
            s->DefaultBids(sink);
            Push(st, st.Scratch, 0.0f, now);
        }

        if (st.Queue.empty())
            return false;

        for (uint32 i = 0; i < ITERATIONS_PER_TICK && !st.Queue.empty(); ++i)
        {
            size_t const idx = Choose(st, ctx, now);
            Bid const bid = st.Queue[idx];
            st.Queue.erase(st.Queue.begin() + idx);

            Action* action = Find(bid.Action);
            if (!action)
                continue;

            // §3.3 — two different questions with two different recoveries. USELESS drops the
            // bid; IMPOSSIBLE pushes the alternatives.
            if (!action->Useful(ctx))
                continue;

            char const* vetoedBy = nullptr;
            float const rel = MultipliedRelevance(*action, ctx, bid.Relevance, &vetoedBy);
            if (rel <= 0.0f)
            {
                TC_LOG_DEBUG("server.worldserver",
                    "Constellation РЕШЕНИЕ {}: «{}» снято правилом «{}»",
                    ctx.World.Name(), action->Name(), vetoedBy ? vetoedBy : "?");
                continue;
            }

            if (!action->Possible(ctx))
            {
                st.Scratch.clear();
                BidSink sink(st.Scratch, st.BidsDropped);
                action->Alternatives(sink);
                Push(st, st.Scratch, rel + REL_ALTERNATIVE, now);
                continue;
            }

            if (!bid.SkipPrerequisites)
            {
                st.Scratch.clear();
                BidSink sink(st.Scratch, st.BidsDropped);
                action->Prerequisites(sink);
                if (Push(st, st.Scratch, rel + REL_PREREQ_BUMP, now))
                {
                    // The prerequisite goes above us and we come back just under it, so the
                    // original is not lost and does not outrank what it is waiting for.
                    st.Scratch.clear();
                    BidSink requeue(st.Scratch, st.BidsDropped);
                    requeue.Add(bid.Action, rel + REL_REQUEUE, true);
                    Push(st, st.Scratch, 0.0f, now);
                    continue;
                }
            }

            if (action->Execute(ctx))
            {
                // §4.4′ — NEW work gets a new salt; continuing the same work keeps it, so the
                // choice is stable within an assignment and decorrelated across them.
                if (bid.Action != st.Running)
                    ++st.AssignmentEpoch;

                LogChoice(st, ctx, *action, rel);
                st.Scratch.clear();
                BidSink sink(st.Scratch, st.BidsDropped);
                action->Continuers(sink);
                Push(st, st.Scratch, rel, now);
                st.Running       = bid.Action;
                st.RunningRel    = rel;
                st.ReplanAfterMs = now + REPLAN_COOLDOWN_MS;
                return true;
            }

            st.Scratch.clear();
            BidSink sink(st.Scratch, st.BidsDropped);
            action->Alternatives(sink);
            Push(st, st.Scratch, rel + REL_ALTERNATIVE, now);
        }

        return false;
    }
}
