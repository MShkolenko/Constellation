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
#include "World.h"

#include <algorithm>
#include <cmath>

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

    void Engine::Register(std::unique_ptr<Trigger> trigger, uint32 ownerMask)
    {
        if (_ready || !trigger)
            return;
        if (!ownerMask)
        {
            ++_rejected;      // владельца нет — сработать не сможет никогда
            return;
        }
        _triggers.push_back(Owned<Trigger>{ std::move(trigger), ownerMask });
    }

    void Engine::Register(std::unique_ptr<Multiplier> multiplier, uint32 ownerMask)
    {
        if (_ready || !multiplier)
            return;
        if (!ownerMask)
        {
            ++_rejected;
            return;
        }
        _multipliers.push_back(Owned<Multiplier>{ std::move(multiplier), ownerMask });
    }

    void Engine::Register(std::unique_ptr<Strategy> strategy)
    {
        if (!_ready && strategy)
            _strategies.push_back(std::move(strategy));
    }

    // §12 — ОДИН ПОСТАВЩИК НА ЗНАЧЕНИЕ, И ВТОРОЙ НЕ ТИХО ПОБЕЖДАЕТ.
    //
    // Замена молча — тот же класс дефекта, что и `lms load` поверх загруженной модели:
    // всё работает, отвечает не тот, и ни одно число об этом не говорит. Считаем и
    // отказываем в готовности там, где есть журнал.
    void Engine::RegisterValueBase(ValueId id, std::unique_ptr<ValueBase> value)
    {
        if (_ready || !value)
            return;
        if (id >= ValueId::Count)
        {
            ++_valuesRejected;
            return;
        }
        size_t const idx = size_t(id);
        if (_values.size() <= idx)
            _values.resize(idx + 1);
        if (_values[idx])
        {
            ++_valuesRejected;      // второй поставщик на тот же идентификатор
            return;
        }
        _values[idx] = std::move(value);
    }

    Action* Engine::Find(ActionId id) const
    {
        size_t const idx = size_t(id);
        return idx < _actions.size() ? _actions[idx].get() : nullptr;
    }

    bool Engine::Seal()
    {
        _actions.resize(size_t(ActionId::Count));

        // SEALING IS A VALIDATION, NOT A RESIZE. It used to only grow the vector and set a flag,
        // so a bid naming an unregistered action met a `continue` in the tick — silent, every
        // tick, forever. The failure would have looked exactly like "the ladder chose something
        // else", which is the hardest kind of defect to see in a log full of choices.
        uint32 missing = 0;
        for (size_t i = 1; i < size_t(ActionId::Count); ++i)     // 0 is None, deliberately absent
        {
            if (_actions[i])
                continue;
            ++missing;
            TC_LOG_ERROR("server.worldserver",
                "Constellation ДВИЖОК: действие «{}» ({}) объявлено и не зарегистрировано",
                NameOf(ActionId(i)), i);
        }

        // §12 — ТО ЖЕ ДЛЯ ЗНАЧЕНИЙ. Без этой проверки незарегистрированное значение
        // даёт разыменование нуля на живом рилме при первом же чтении — не «выбралось что-то
        // другое», а падение мирового потока (Кодекс, состязательный проход по разбору).
        _values.resize(size_t(ValueId::Count));
        for (size_t i = 0; i < size_t(ValueId::Count); ++i)
        {
            if (_values[i])
                continue;
            ++missing;
            TC_LOG_ERROR("server.worldserver",
                "Constellation ДВИЖОК: значение «{}» ({}) объявлено и не зарегистрировано",
                NameOf(ValueId(i)), i);
        }

        // THE FLIGHT DOOR IS A TELEPORT WHEN THE CORE SAYS SO, AND A WARNING IS NOT A GATE.
        //
        // Player.cpp:23076 — with CONFIG_INSTANT_TAXI set, ActivateTaxiPathTo calls TeleportTo and
        // returns false. So CMSG_ACTIVATE_TAXI, a perfectly legal client opcode, moves a companion
        // without walking (the operator's «ходим ногами» broken while invariant 0's letter is
        // kept), AND IsInFlight never becomes true, so a successful teleport and a companion with
        // no money are the same observation. The realm has it at 0 today; a config edit is one
        // line away, so this is read here and acted on, not logged.
        _instantTaxi = sWorld->getBoolConfig(CONFIG_INSTANT_TAXI);
        if (_instantTaxi)
            TC_LOG_ERROR("server.worldserver",
                "Constellation ДВИЖОК: InstantFlightPaths включён — ядро телепортирует по"
                " CMSG_ACTIVATE_TAXI (Player.cpp:23076). Полёты движком ЗАПРЕЩЕНЫ на этом мире.");

        if (_rejected)
            TC_LOG_ERROR("server.worldserver",
                "Constellation ДВИЖОК: отвергнуто регистраций без владельца: {}."
                " Такой триггер или множитель не сработал бы ни у кого", _rejected);

        if (_valuesRejected)
            TC_LOG_ERROR("server.worldserver",
                "Constellation ДВИЖОК: отвергнуто регистраций значений: {}."
                " Дубль или неизвестный идентификатор — отвечал бы не тот", _valuesRejected);

        _ready = (missing == 0 && _rejected == 0 && _valuesRejected == 0);
        TC_LOG_INFO("server.worldserver",
            "Constellation ДВИЖОК: действий {}, триггеров {}, множителей {}, стратегий {},"
            " мгновенные полёты {} — {}",
            _actions.size(), _triggers.size(), _multipliers.size(), _strategies.size(),
            _instantTaxi ? "ДА" : "нет",
            _ready ? "готов"
                   : "НЕ ГОТОВ: не зарегистрировано действий " + std::to_string(missing)
                     + ", отвергнуто регистраций " + std::to_string(_rejected));
        return _ready;
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
            Bid q = b;
            q.Relevance = rel;
            q.CreatedMs = nowMs;
            // Полный сброс кэша счёта, а не только флага: так инвариант «Scored говорит,
            // считан ли Score» читается без оговорок про остаточные поля.
            q.Scored    = false;
            q.Score     = REL_IDLE;
            q.ScoredMs  = 0;
            st.Queue.push_back(q);
            ++st.BidsPushed;
            pushed = true;
        }
        return pushed;
    }

    float Engine::MultipliedRelevance(Action& action, Ctx& ctx, uint32 strategyMask,
                                      float relevance, char const** vetoedBy) const
    {
        for (size_t i = 0; i < _multipliers.size(); ++i)
        {
            // §9 — только множители тех стратегий, что включены у ЭТОГО спутника. Проверка
            // битовая, потому что собирать вектор указателей на каждую оценённую ставку
            // означало бы выделение на такте — ровно то, что §11 запрещает.
            if (!(_multipliers[i].Owners & strategyMask))
                continue;
            auto const& m = _multipliers[i].Obj;
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
    size_t Engine::Choose(EngineState& st, Ctx& ctx, uint32 nowMs, float& outScore) const
    {
        bool const sticky = st.Running != ActionId::None && nowMs < st.ReplanAfterMs;

        // §4.3′ — the SCORE decides, not the raw relevance. Each action turns its base bid into a
        // score using its own subject and its own class of work; the engine only compares.
        //
        // SCORED EXACTLY ONCE, AND THE WINNER'S SCORE TRAVELS OUT. The previous version scored
        // here and then scored again before executing: with a Score() that is not pure — and the
        // interface cannot force purity, since a value-backed score needs a mutable Ctx — the
        // action could execute at a relevance nobody selected. Now `outScore` carries the exact
        // number the choice was made on.
        //
        // ONE BUFFER FOR THE COMPARISON; THE BASE SCORE LIVES ON THE BID.
        //
        // `cmp` carries the stickiness bonus and decides the comparison; the bid's own `Score`
        // does not and is what leaves the function. Carrying stickiness into the executed
        // relevance would inflate the continuers pushed after it, and that inflation would
        // compound tick after tick — the same compounding already caught once when stickiness was
        // written into the queued bid.
        //
        // Scoring is guarded by the tick stamp, because Choose() runs up to eight times a tick
        // and used to re-score the whole remaining queue on every pass.
        // Pass one: score what is not yet scored this tick, and DROP a bid whose score is not
        // finite, compacting the queue in place.
        //
        // A NON-FINITE SCORE IS A DEFECT, NOT A WINNER, AND ZEROING IT WAS NOT ENOUGH. NaN loses
        // every comparison, so such a bid slips past the tie test into the fallback; infinity
        // wins everything forever. Setting it to zero stopped it executing — the multiplier gate
        // rejects `<= 0` — but it still occupied a place in the selection and consumed one of the
        // eight iterations this tick is allowed. So it leaves the queue here.
        size_t write = 0;
        for (size_t read = 0; read < st.Queue.size(); ++read)
        {
            Bid& b = st.Queue[read];
            if (!b.Scored || b.ScoredMs != nowMs)
            {
                Action* a = Find(b.Action);
                float const s = a ? a->Score(ctx, b, b.Relevance) : b.Relevance;
                if (!std::isfinite(s))
                {
                    ++st.BidsDropped;
                    continue;                       // not compacted forward: it is gone
                }
                b.Score    = s;
                b.ScoredMs = nowMs;
                b.Scored   = true;
            }
            if (write != read)
                st.Queue[write] = st.Queue[read];   // Bid is trivially copyable
            ++write;
        }
        st.Queue.resize(write);

        float cmp[QUEUE_CAP] = {};
        size_t const n = st.Queue.size() < QUEUE_CAP ? st.Queue.size() : QUEUE_CAP;
        float bestCmp = -1.0f;
        for (size_t i = 0; i < n; ++i)
        {
            Bid const& b = st.Queue[i];
            cmp[i] = b.Score * ((sticky && b.Action == st.Running) ? STICKINESS : 1.0f);
            if (cmp[i] > bestCmp)
                bestCmp = cmp[i];
        }

        size_t ties = 0;
        for (size_t i = 0; i < n; ++i)
            if (bestCmp - cmp[i] <= TIE_EPSILON)
                ++ties;

        size_t pick = ties > 1
            ? size_t(Salt(ctx.World.Guid().GetCounter(), st.AssignmentEpoch) % ties)
            : 0;

        for (size_t i = 0; i < n; ++i)
        {
            if (bestCmp - cmp[i] <= TIE_EPSILON)
            {
                if (pick == 0)
                {
                    outScore = st.Queue[i].Score;
                    return i;
                }
                --pick;
            }
        }
        // Unreachable while every score is finite — which the guard above now guarantees, since
        // the only way past the tie test was a NaN that loses every comparison.
        outScore = n ? st.Queue[0].Score : REL_IDLE;
        return 0;
    }

    void Engine::LogChoice(EngineState& st, Ctx& ctx, Action const& chosen, float relevance,
                           Run run) const
    {
        // §5 — every decision says why. An engine whose choice cannot be read back is worse than
        // the `if` chain it replaces, because at least a chain can be read top to bottom.
        //
        // ONLY ON A CHANGE, and this is not cosmetic. Written on every successful Execute it is
        // an fmt format into a fresh std::string at 122 companions × 4 Hz — hundreds of formatted
        // lines a second, in a module that has already produced 31 343 238 lines in ten minutes.
        // A decision that has not changed is not news; the counters live in the periodic line.
        if (chosen.Id() == st.Running)
            return;
        TC_LOG_INFO("server.worldserver",
            "Constellation {} {}: «{}» {:.2f} вместо «{}» (эпоха {}, в очереди {}, сброшено {})",
            run == Run::Shadow ? "ТЕНЬ" : "РЕШЕНИЕ",
            ctx.World.Name(), chosen.Name(), relevance, NameOf(st.Running),
            st.AssignmentEpoch, st.Queue.size(), st.BidsDropped);
    }

    void Engine::Reset(EngineState& st, Ctx& ctx, CancelReason why)
    {
        // §2″ — give up the work, KEEP the mode. Not a handoff.
        // §10′ — and the one place reservations will be released and values invalidated once
        // those exist. Nothing else may do it.
        if (st.Running != ActionId::None)
        {
            if (Action* a = Find(st.Running))
                a->Cancel(ctx, st.RunningAbout, why);
            ++st.ActionsCancelled;
        }
        st.Queue.clear();
        st.Scratch.clear();
        st.Running       = ActionId::None;
        st.RunningAbout  = Subject();
        st.RunningRel    = REL_IDLE;
        st.ReplanAfterMs = 0;
        // §12 — РАБОТА БРОШЕНА, ЗНАЧИТ ОТВЕТЫ О НЕЙ УСТАРЕЛИ. Заголовок уже называл
        // это место единственным для будущей инвалидации; без этой строки он обещал, а
        // не делал (Кодекс, пункт 5). Стоит три присвоения `bool` — буферы не трогаются
        // и свою ёмкость сохраняют.
        st.Values.InvalidateAll();
        ++st.AssignmentEpoch;       // §4.4′ — new work, new salt; spreading must not be permanent
    }

    void Engine::Discard(EngineState& st)
    {
        // No Ctx, so no Action::Cancel — there is no world left to cancel in. What CAN be done
        // without one is releasing anything the companion holds outside its own struct.
        //
        // RESERVATION RELEASE BELONGS HERE AND IS NOT WRITTEN, DELIBERATELY.
        //
        // The review asked twice for it to be implemented now. Refused, in writing, because
        // there is nothing to release: no registry exists, and a release function over an absent
        // registry is an empty abstraction that would have to be rewritten the moment the real
        // one lands (plan step 28, which the operator put inside this migration).
        //
        // What IS implemented is the thing that makes forgetting impossible: every context-free
        // ending in the module now routes through this one function — config off, a companion
        // with no session, and dismissal including shutdown — instead of assigning a fresh state
        // over the old one. When the registry arrives it has exactly one place to plug into, and
        // that place is already called from all three.
        //
        // The danger being guarded against, so it is not rediscovered: reservations will live
        // OUTSIDE the companion struct, so dropping the struct loses the handle and leaks the
        // point for as long as the world runs — and the health metric reads green, because a
        // leak and a healthy registry look identical when nothing sweeps.
        uint32 const dropped = st.BidsDropped;      // kept: it is a defect report, not live state
        st = EngineState();
        st.BidsDropped = dropped;
    }

    bool Engine::Tick(EngineState& st, Ctx& ctx, uint32 modeEpoch, Run run)
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
        // В ТЕНИ ТАКТ НЕ ОТКРЫВАЕТСЯ ВОВСЕ: Execute не зовётся, значит ходить нечему, а
        // открытый бюджет позволил бы двинуться чему-то, что тень запускать не должна.
        if (run == Run::Decide)
            ctx.Act.ResetTick();

        // §4.1 — a bid made three seconds ago is answering a world that has moved on.
        size_t const before = st.Queue.size();
        st.Queue.erase(std::remove_if(st.Queue.begin(), st.Queue.end(),
            [now](Bid const& b) { return Elapsed(now, b.CreatedMs) > BID_TTL_MS; }),
            st.Queue.end());
        st.BidsExpired += uint32(before - st.Queue.size());

        // §4.1 — triggers, each on its own interval. Checking every trigger every tick is what
        // the module's hand-rolled throttles exist to avoid; here the interval is declared.
        for (size_t ti = 0; ti < _triggers.size(); ++ti)
        {
            // §9 — триггер работает, только если его стратегия включена у этого спутника.
            if (!(_triggers[ti].Owners & st.StrategyMask))
                continue;
            auto const& t = _triggers[ti].Obj;
            uint32& lastMs = st.TriggerLastMs[size_t(t->Id())];
            if (!t->NeedsCheck(now, lastMs))
                continue;
            lastMs = now;                    // this companion's clock, not the trigger's
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
            if (!(MaskOf(s->Id()) & st.StrategyMask))
                continue;
            st.Scratch.clear();
            BidSink sink(st.Scratch, st.BidsDropped);
            s->DefaultBids(ctx, sink);
            Push(st, st.Scratch, 0.0f, now);
        }

        if (st.Queue.empty())
            return false;

        for (uint32 i = 0; i < ITERATIONS_PER_TICK && !st.Queue.empty(); ++i)
        {
            float chosenScore = REL_IDLE;
            size_t const idx = Choose(st, ctx, now, chosenScore);
            if (st.Queue.empty())
                break;                  // Choose may have dropped every bid as non-finite
            Bid const bid = st.Queue[idx];
            st.Queue.erase(st.Queue.begin() + idx);

            Action* action = Find(bid.Action);
            if (!action)
                continue;

            // §3.3 — two different questions with two different recoveries. USELESS drops the
            // bid; IMPOSSIBLE pushes the alternatives.
            if (!action->Useful(ctx, bid))
                continue;

            // THE SCORE SELECTION COMPUTED, not a second call to Score(). Calling it again could
            // return a different number, and then what executes is not what won.
            char const* vetoedBy = nullptr;
            float const rel = MultipliedRelevance(*action, ctx, st.StrategyMask, chosenScore, &vetoedBy);
            if (rel <= 0.0f)
            {
                TC_LOG_DEBUG("server.worldserver",
                    "Constellation РЕШЕНИЕ {}: «{}» снято правилом «{}»",
                    ctx.World.Name(), action->Name(), vetoedBy ? vetoedBy : "?");
                continue;
            }

            if (!action->Possible(ctx, bid))
            {
                st.Scratch.clear();
                BidSink sink(st.Scratch, st.BidsDropped);
                action->Alternatives(ctx, bid, sink);
                Push(st, st.Scratch, rel + REL_ALTERNATIVE, now);
                continue;
            }

            if (!bid.SkipPrerequisites)
            {
                st.Scratch.clear();
                BidSink sink(st.Scratch, st.BidsDropped);
                action->Prerequisites(ctx, bid, sink);
                if (Push(st, st.Scratch, rel + REL_PREREQ_BUMP, now))
                {
                    // The prerequisite goes above us and we come back just under it, so the
                    // original is not lost and does not outrank what it is waiting for.
                    //
                    // AND IT KEEPS ITS ORIGINAL BIRTHDAY. Stamping `now` here made the TTL
                    // unreachable: a bid whose prerequisite is never satisfied would re-queue
                    // itself fresh every tick and live forever, which is precisely the immortal
                    // retry the expiry exists to kill.
                    st.Scratch.clear();
                    BidSink requeue(st.Scratch, st.BidsDropped);
                    requeue.Add(bid.Action, rel + REL_REQUEUE, bid.About, true);
                    Push(st, st.Scratch, 0.0f, bid.CreatedMs);
                    continue;
                }
            }

            // §10 — В ТЕНИ ИСПОЛНЕНИЯ НЕТ. Считаем его удавшимся: иначе ветка альтернатив
            // разошлась бы с настоящей на первом же отказе, и сравнивать было бы нечего.
            bool const ran = (run == Run::Shadow) ? true : action->Execute(ctx, bid);
            if (ran)
            {
                // §4.4′ — NEW work gets a new salt; continuing the same work keeps it, so the
                // choice is stable within an assignment and decorrelated across them.
                if (bid.Action != st.Running)
                    ++st.AssignmentEpoch;

                LogChoice(st, ctx, *action, rel, run);
                st.Scratch.clear();
                BidSink sink(st.Scratch, st.BidsDropped);
                action->Continuers(ctx, bid, sink);
                Push(st, st.Scratch, rel, now);
                st.Running       = bid.Action;
                st.RunningAbout  = bid.About;
                st.RunningRel    = rel;
                st.ReplanAfterMs = now + REPLAN_COOLDOWN_MS;
                return true;
            }

            // EXECUTE FAILED, SO WE ARE NOT RUNNING IT ANY MORE.
            //
            // `Running` was left pointing at the failed action, which fed the stickiness bonus:
            // the thing that just failed kept a 1.35× advantage for the whole replan cooldown and
            // would be chosen again ahead of a working alternative. Clearing it also lets the
            // next successful action be reported as a change (§5) instead of being swallowed.
            if (st.Running == bid.Action)
            {
                st.Running      = ActionId::None;
                st.RunningAbout = Subject();
                st.RunningRel   = REL_IDLE;
            }

            st.Scratch.clear();
            BidSink sink(st.Scratch, st.BidsDropped);
            action->Alternatives(ctx, bid, sink);
            Push(st, st.Scratch, rel + REL_ALTERNATIVE, now);
        }

        return false;
    }
}
