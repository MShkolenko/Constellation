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
        // §11′ — НЕНАПИСАННОЕ ДЕЙСТВИЕ БОЛЬШЕ НЕ ЗАПРЕЩАЕТ ГОТОВНОСТЬ.
        //
        // Здесь стояла проверка «у каждого объявленного ActionId есть объект, иначе не
        // готовы», и она делала план невыполнимым: объявлено тринадцать, пишутся они по
        // одному, а тень — весь смысл пошаговости — не могла тикнуть ни разу, пока не
        // перенесена вся ветка целиком. То есть ворота требовали конца работы для того,
        // чтобы разрешить её начало.
        //
        // Защищала она настоящий дефект — ставка на незарегистрированное действие
        // встречала молчаливый `continue`, каждый такт и вечно, — но защищала не там.
        // Недопустимо МОЛЧАНИЕ, а не сама ставка; молчание убрано в самом такте
        // (см. `BidsUnbacked` ниже), и гарантия цела, а ворота больше не требуют будущего.
        //
        // Считаем и докладываем — но одной строкой, а не тринадцатью: сегодня нет ни
        // одного, и тринадцать ошибок при каждом подъёме мира — это шум, а не сведение.
        uint32 missingValues = 0;
        uint32 unwritten = 0;
        for (size_t i = 1; i < size_t(ActionId::Count); ++i)     // 0 is None, deliberately absent
            if (!_actions[i])
                ++unwritten;
        if (unwritten)
        {
            // И КАКИХ ИМЕННО. Агрегат «0 из 13» не говорит, что осталось сделать, а весь
            // смысл этой строки — чтобы подъём мира сам рассказывал, на каком шаге миграция.
            // Один раз за подъём это предложение, а не шум (Кодекс, п. 5).
            std::string names;
            for (size_t i = 1; i < size_t(ActionId::Count); ++i)
                if (!_actions[i])
                {
                    if (!names.empty())
                        names += ", ";
                    names += NameOf(ActionId(i));
                }
            TC_LOG_INFO("server.worldserver",
                "Constellation ДВИЖОК: действий объявлено {}, написано {} — миграция идёт."
                " Нет ещё: {}. Ставка на любое из них будет посчитана и названа в самом такте",
                uint32(ActionId::Count) - 1, uint32(ActionId::Count) - 1 - unwritten, names);
        }

        // §12 — ТО ЖЕ ДЛЯ ЗНАЧЕНИЙ. Без этой проверки незарегистрированное значение
        // даёт разыменование нуля на живом рилме при первом же чтении — не «выбралось что-то
        // другое», а падение мирового потока (Кодекс, состязательный проход по разбору).
        _values.resize(size_t(ValueId::Count));
        for (size_t i = 0; i < size_t(ValueId::Count); ++i)
        {
            if (_values[i])
                continue;
            ++missingValues;
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

        _ready = (missingValues == 0 && _rejected == 0 && _valuesRejected == 0);
        TC_LOG_INFO("server.worldserver",
            "Constellation ДВИЖОК: действий {}, триггеров {}, множителей {}, стратегий {},"
            " мгновенные полёты {} — {}",
            _actions.size(), _triggers.size(), _multipliers.size(), _strategies.size(),
            _instantTaxi ? "ДА" : "нет",
            _ready ? "готов"
                   : "НЕ ГОТОВ: не зарегистрировано ЗНАЧЕНИЙ " + std::to_string(missingValues)
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
        // ТРИ ИМЕНИ, А НЕ ДВА, И ЭТО ИСПРАВЛЕНИЕ ПРИБОРА, А НЕ УКРАШЕНИЕ.
        //
        // «было» — прежнее действие САМОГО движка: строка пишется только на смене, и без него
        // непонятно, что сменилось. «у лестницы» — что в этот момент делает другой механизм, и
        // ровно этого здесь не хватало: я читал первое как второе и разбирал расхождения,
        // которых строка не показывала.
        TC_LOG_INFO("server.worldserver",
            "Constellation {} {}: «{}» {:.2f} (было «{}», у лестницы «{}») (эпоха {}, в очереди {},"
            " сброшено {}, подавлено {}, шаг отвергнут {})",
            run == Run::Shadow ? "ТЕНЬ" : "РЕШЕНИЕ",
            ctx.World.Name(), chosen.Name(), relevance, NameOf(st.Running),
            ctx.Peer ? ctx.Peer : "?",
            st.AssignmentEpoch, st.Queue.size(), st.BidsDropped, st.BidsSuppressed,
            st.StepsRefused);
    }

    // -----------------------------------------------------------------------------------------
    // §13 — чтение значения по одному `Ctx`. Тело здесь, а не в заголовке, потому что
    // только здесь оба типа — `EngineState` и `Engine` — полные.
    //
    // СОСТОЯНИЯ НЕТ — ПУСТОЙ БУФЕР, А НЕ ПАДЕНИЕ. Статический пустой экземпляр
    // на каждый тип буфера: читатель получает что-то обходимое и пустое, и ни одно
    // действие не обязано проверять указатель.
    // -----------------------------------------------------------------------------------------
    template <ValueId Id>
    typename ValueTraits<Id>::Type const& Val(Ctx& ctx)
    {
        using T = typename ValueTraits<Id>::Type;
        static T const empty{};
        if (!ctx.St)
            return empty;
        return Engine::Instance().Val<Id>(*ctx.St, ctx);
    }

    // Явные инстанциации — по тому же списку, что объявил сами значения: тело в .cpp,
    // значит без них любой читатель упадёт на компоновке. Список один, расхождения нет.
#define CONSTELLATION_VALUE_INSTANTIATE(name, text, type) \
    template type const& Val<ValueId::name>(Ctx&);
    CONSTELLATION_VALUES(CONSTELLATION_VALUE_INSTANTIATE)
#undef CONSTELLATION_VALUE_INSTANTIATE

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
        // §31 — ДОРОГА ТОЖЕ БРОШЕНА. Здесь различие с таблицей отсрочек, и оно противоположное:
        // отсрочка — память о РЕЗУЛЬТАТЕ и переживает отказ от работы, а прогресс ходьбы — снимок
        // ИСПОЛНЯЕМОГО действия. Работа брошена — значит и дорога. Без этой строки повторная
        // попытка к той же цели унаследовала бы чужой счётчик застревания и оборвалась бы, не
        // начавшись: сравнение `Subject` ловит смену цели, но не новую попытку к прежней
        // (Кодекс, пункты 1 и 3).
        st.Walk = WalkProgress();
        // И ЯРУСОМ НИЖЕ — ТОЖЕ. `Walk` выше отвечает «стоит ли ещё идти», `Move` — «как сделать
        // следующий шаг»: маршрут ядра, место в нём, отступы вбок. Работа брошена — значит и
        // маршрут: без этой строки новая попытка шла бы по точкам чужой дороги. Дефект нашёл
        // Кодекс, и нашёл ровно там, где строкой выше объяснено, зачем чистится `Walk`.
        st.Move = MoveState();
        // `NoCombatMs` ЗДЕСЬ НЕТ НАМЕРЕННО: он мера МИРА, а не снимок брошенной работы. Бросить
        // цель — не то же самое, что подраться, и обнулять по смене работы значило бы никогда не
        // дорастать до отступления порога у того, кто как раз и не может ни за кого зацепиться.
        ++st.AssignmentEpoch;       // §4.4′ — new work, new salt; spreading must not be permanent
    }

    namespace
    {
        // Просрочка считается ИНТЕРВАЛОМ — см. комментарий у `BackoffEntry`.
        inline bool Expired(EngineState::BackoffEntry const& e, uint32 nowMs)
        {
            return (nowMs - e.SetAtMs) >= e.TtlMs;
        }
        inline uint32 Remaining(EngineState::BackoffEntry const& e, uint32 nowMs)
        {
            uint32 const gone = nowMs - e.SetAtMs;
            return gone >= e.TtlMs ? 0u : e.TtlMs - gone;
        }
    }

    bool Engine::Deferred(EngineState const& st, BackoffKey const& key, uint32 nowMs)
    {
        // ПРОСТО ЧИТАЕТ. Раньше этот проход попутно подметал просроченное, и из-за этого его
        // нельзя было позвать оттуда, где состояние константно, — а именно оттуда его и зовёт
        // политика выбора квеста. Подметание переехало в `Defer`: он и так обходит таблицу ради
        // проверки на дубль и вытесняет просроченное первым, так что течь ей неоткуда.
        for (uint8 i = 0; i < st.BackoffCount; ++i)
            if (st.Backoffs[i].Key == key && !Expired(st.Backoffs[i], nowMs))
                return true;
        return false;
    }

    bool Engine::Defer(EngineState& st, BackoffKey const& key, uint32 ttlMs, uint32 nowMs)
    {
        if (key.Kind == BackoffKind::None || !ttlMs)
            return false;

        // ПОДМЕТАНИЕ ЖИВЁТ ЗДЕСЬ, потому что `Deferred` теперь только читает. Один проход, и
        // он же нужен для проверки на дубль ниже — лишней работы не появилось.
        for (uint8 i = 0; i < st.BackoffCount; )
        {
            if (Expired(st.Backoffs[i], nowMs))
            {
                st.Backoffs[i] = st.Backoffs[st.BackoffCount - 1];
                --st.BackoffCount;
                continue;               // на месте i теперь другая запись — её тоже проверить
            }
            ++i;
        }

        // Повторный запрет по тому же ключу ПРОДЛЕВАЕТ, а не заводит вторую запись: иначе
        // таблица заполнилась бы копиями одного отказа.
        for (uint8 i = 0; i < st.BackoffCount; ++i)
            if (st.Backoffs[i].Key == key)
            {
                st.Backoffs[i].SetAtMs = nowMs;
                st.Backoffs[i].TtlMs   = ttlMs;
                return false;
            }

        if (st.BackoffCount < BACKOFF_CAP)
        {
            st.Backoffs[st.BackoffCount].Key     = key;
            st.Backoffs[st.BackoffCount].SetAtMs = nowMs;
            st.Backoffs[st.BackoffCount].TtlMs   = ttlMs;
            ++st.BackoffCount;
            return false;
        }

        ++st.BackoffFull;

        // Сперва просроченное — оно уже ничего не стоит.
        for (uint8 i = 0; i < st.BackoffCount; ++i)
            if (Expired(st.Backoffs[i], nowMs))
            {
                st.Backoffs[i].Key     = key;
                st.Backoffs[i].SetAtMs = nowMs;
                st.Backoffs[i].TtlMs   = ttlMs;
                return false;
            }

        // Затем — с НАИМЕНЬШИМ ОСТАТКОМ. Не старейшая: возраст создания не равен оставшейся
        // ценности, и «вытесняем старейшее» это способ заставить занятого спутника забыть
        // именно то, что ему нужнее всего помнить.
        uint8  victim = 0;
        uint32 least  = Remaining(st.Backoffs[0], nowMs);
        for (uint8 i = 1; i < st.BackoffCount; ++i)
        {
            uint32 const r = Remaining(st.Backoffs[i], nowMs);
            if (r < least)
                { least = r; victim = i; }
        }
        ++st.BackoffEvictedLive;        // ДЕФЕКТ ЁМКОСТИ, а не рабочий режим — по этому счётчику
        st.Backoffs[victim].Key     = key;   // и выбирается BACKOFF_CAP при росте состава
        st.Backoffs[victim].SetAtMs = nowMs;
        st.Backoffs[victim].TtlMs   = ttlMs;
        return true;
    }

    void Engine::Allow(EngineState& st, BackoffKey const& key)
    {
        for (uint8 i = 0; i < st.BackoffCount; ++i)
            if (st.Backoffs[i].Key == key)
            {
                st.Backoffs[i] = st.Backoffs[st.BackoffCount - 1];
                --st.BackoffCount;
                return;
            }
    }

    namespace
    {
        // Общая часть обоих адаптеров: собрать ключ и спросить таблицу.
        inline bool EngineRemembers(void const* user, BackoffKind kind, Subject const& about)
        {
            Ctx const* ctx = static_cast<Ctx const*>(user);
            if (!ctx || !ctx->St)
                return false;
            BackoffKey key;
            key.Kind  = kind;
            key.About = about;
            return Engine::Deferred(*ctx->St, key, ctx->NowMs);
        }
    }

    namespace
    {
        // ЕДИНСТВЕННЫЙ ОТПРАВЩИК ДВИЖКА, И ОН ЗДЕСЬ, А НЕ У ДЕЙСТВИЯ.
        //
        // Двигатель шлёт три опкода; у двери есть метод ровно под один. Прыжок и приземление
        // отвергаются, и это ШТАТНЫЙ отказ, а не авария: `UnstickCore` получит ложь и уйдёт в
        // свою ветку отступа вбок — спутник обходит препятствие вместо того, чтобы его
        // перепрыгнуть, пока такой двери нет.
        //
        // Дверь при этом считает шаг в свой бюджет такта, режет его до предела шага и записывает
        // отказ. Ради этого учёта отправка и идёт через неё, а не мимо.
        bool SendThroughDoor(void* user, OpcodeClient opcode, MovementInfo& mi)
        {
            Ctx* ctx = static_cast<Ctx*>(user);
            if (!ctx)
                return false;
            if (opcode != CMSG_MOVE_HEARTBEAT)
                return false;
            return ctx->Act.Step(mi.pos, mi.flags);
        }
    }

    bool WalkTowards(Ctx& ctx, Position const& to, float stopAt, float dt)
    {
        if (!ctx.St)
            return false;
        return ctx.World.StepFor(ctx.St->Move, to, stopAt, dt, &SendThroughDoor, &ctx);
    }

    // НА СКОЛЬКО ЗАБЫТЬ ОСОБЬ, ДО КОТОРОЙ НЕ ДОБРАТЬСЯ. Десять минут — срок лестницы для
    // недостижимых собеседников; у неё этот набор вообще без срока, и это осознанная разница:
    // её множество растёт без предела, наша таблица ограничена шестнадцатью записями.
    inline constexpr uint32 UNREACHABLE_UNIT_MS = 600000;

    // ПОТОЛОК СЧЁТА ПРОСТОЯ — час, число лестницы (`Constellation.cpp:2267`). Он нужен не ради
    // памяти, а ради смысла: без него счётчик уходит в бесконечность и перестаёт различать
    // «давно не дрался» и «не дрался никогда».
    inline constexpr uint32 NO_COMBAT_CAP_MS = 3600000;

    // РАЗРЫВ В НАБЛЮДЕНИИ. Число не выбрано, а взято у двери: `STEP_SLICE_CAP_SECONDS` — это уже
    // объявленная в модуле граница, за которой измеренный срез такта перестаёт быть достоверным.
    // Заводить здесь второе такое число значило бы завести второе мнение о том же.
    inline constexpr uint32 OBSERVATION_GAP_MS = 1000;

    WalkVerdict AdvanceWalk(Ctx& ctx, Subject const& toward, float dist, uint32 sliceMs, bool stalled)
    {
        if (!ctx.St)
            return WalkVerdict::Going;      // без состояния судить не о чем — и не мешать
        WalkProgress& w = ctx.St->Walk;

        // НОВАЯ ЦЕЛЬ — НОВАЯ ДОРОГА. Перезапуск здесь, а не у вызывающего: забытый перезапуск
        // означал бы, что счётчик застревания пришёл из прошлого пути и оборвал бы новый.
        if (!(w.Toward == toward))
            w.Restart(toward, ctx.NowMs);

        // ПРИБЛИЖЕНИЕ — С ЗАПАСОМ. Правило и число у лестницы (`Constellation.cpp:3375`).
        // Первый замер прогрессом не считается и не наказывается: сравнивать ещё не с чем.
        if (!w.Measured || dist < w.Best - WALK_PROGRESS_YARDS)
        {
            w.Measured = true;
            w.Best     = dist;
            w.StuckMs  = 0;
        }
        else
            w.StuckMs += sliceMs;

        if (stalled)
            return WalkVerdict::Stalled;
        if (w.StuckMs > WALK_NO_PROGRESS_MS)
            return WalkVerdict::NoProgress;
        if (ctx.NowMs - w.StartedMs > Tuning().WalkCapMs)
            return WalkVerdict::TooLong;
        return WalkVerdict::Going;
    }

    bool QuestRefusedByEngine(void const* user, uint32 questId)
    {
        return EngineRemembers(user, BackoffKind::CoreRefused, Subject::OfQuest(questId));
    }

    // ДВА ВИДА, А НЕ ОДИН, потому что точка не годится по двум РАЗНЫМ причинам: до неё не
    // добраться, или мы только что оттуда и брать там нечего. Один вид на оба заставил бы журнал
    // говорить «не дойти» о точке, до которой дошли.
    bool SpawnBackedOffByEngine(void const* user, uint32 spawnId)
    {
        Subject const point = Subject::OfSpawn(spawnId);
        return EngineRemembers(user, BackoffKind::Unreachable, point)
            || EngineRemembers(user, BackoffKind::Visited, point);
    }

    namespace
    {
        // Пара видов упакована в `PairKey(a, b) = (a << 32) | b` (`Constellation.cpp:2329`).
        // Распаковка стоит здесь и нигде больше: разойдясь с упаковкой, она отвечала бы про
        // чужую клетку, и заметить это было бы нечем.
        inline Subject PairSubject(uint64 key)
        {
            return Subject::OfEntryPair(uint32(key >> 32), uint32(key & 0xFFFFFFFFull));
        }

        bool FightBannedByEngineId(void const* user, FightBan why, uint64 key)
        {
            switch (why)
            {
                case FightBan::TalkKind:
                    return EngineRemembers(user, BackoffKind::TalkSpeciesDone,
                                           Subject::OfSpecies(uint32(key)));
                case FightBan::FreeUse:
                    return EngineRemembers(user, BackoffKind::FreeUseDone, PairSubject(key));
                case FightBan::GatherPoint:
                    return EngineRemembers(user, BackoffKind::ApproachesDone,
                                           Subject::OfSpawn(uint32(key)));
                default:
                    return false;
            }
        }

        bool FightBannedByEngineGuid(void const* user, FightBan why, ObjectGuid guid)
        {
            switch (why)
            {
                case FightBan::Unreachable:
                    return EngineRemembers(user, BackoffKind::Unreachable, Subject::OfUnit(guid));
                case FightBan::TargetRefused:
                    return EngineRemembers(user, BackoffKind::CombatUnreachable,
                                           Subject::OfUnit(guid));
                // `TalkRetry` — стадия, а не исход: движок на неё запрета не держит.
                default:
                    return false;
            }
        }

        // «ВПЕРВЫЕ» — ЭТО «ЗАПРЕТА ЕЩЁ НЕ БЫЛО», и спрашивается это ДО постановки. `Engine::Defer`
        // возвращает другое — вытеснил ли он живую запись, — и принять одно за другое значило бы
        // печатать строку прибора при каждом переполнении таблицы.
        bool FightNoteByEngine(void* user, FightBan why, ObjectGuid guid)
        {
            Ctx* ctx = static_cast<Ctx*>(user);
            if (!ctx || why != FightBan::Unreachable)
                return false;
            Subject const about = Subject::OfUnit(guid);
            BackoffKey key;
            key.Kind  = BackoffKind::Unreachable;
            key.About = about;
            bool const fresh = !ctx->St || !Engine::Deferred(*ctx->St, key, ctx->NowMs);
            Defer(*ctx, BackoffKind::Unreachable, about, 0, UNREACHABLE_UNIT_MS);
            return fresh;
        }
    }

    FightMemory EngineFightMemory(Ctx& ctx)
    {
        // `toolBusy=false` — УТВЕРЖДЕНИЕ, И СЕГОДНЯ ОНО ВЕРНО: у движка нет ни одного действия
        // с предметом от квеста, значит занят им он не бывает. Ложью оно станет в тот день, когда
        // такое действие появится, и тогда сюда обязано прийти настоящее значение.
        //
        // А ВОТ ГОЛОД ТЕПЕРЬ НАСТОЯЩИЙ. Ноль здесь стоял на том основании, что «эквивалента нет»,
        // и это было неверно: правило лестницы выводится из того, что фасад уже отдаёт, и заняло
        // три строки в такте. Ценой нуля была бы не неточность, а тупик — порог стаи не отступал
        // бы никогда, и цели, которые водятся только стаями, стали бы для движка невыполнимы
        // навсегда. Тень успела показать это одним расхождением до того, как ветка переключена.
        return FightMemory(&FightBannedByEngineId, &FightBannedByEngineGuid, &FightNoteByEngine,
                           &ctx, /*toolBusy=*/false,
                           ctx.St ? ctx.St->NoCombatMs : 0u);
    }

    void Defer(Ctx& ctx, BackoffKind kind, Subject const& about, uint8 detail, uint32 ttlMs)
    {
        if (!ctx.St)
            return;
        BackoffKey key;
        key.Kind   = kind;
        key.About  = about;
        key.Detail = detail;
        if (Engine::Defer(*ctx.St, key, ttlMs, ctx.NowMs) && !ctx.St->BackoffOverflowLogged)
        {
            // ОДИН РАЗ НА СПУТНИКА. Переполнение — дефект ЁМКОСТИ: таблица рассчитана на пик
            // живых ключей, и если живую запись пришлось выбросить, значит пик оценён неверно.
            // По этой строке и правится `BACKOFF_CAP` при росте состава — по ней, а не по
            // ощущению, потому что сегодняшние восемь спутников про 122 ничего не доказывают.
            ctx.St->BackoffOverflowLogged = true;
            TC_LOG_ERROR("server.worldserver",
                "Constellation ДВИЖОК {}: таблица отсрочек переполнена, вытеснена ЖИВАЯ запись "
                "«{}» — ёмкости {} не хватает, поднять её",
                ctx.World.Name(), NameOf(kind), uint32(BACKOFF_CAP));
        }
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
        // §6 — СРЕЗ ТАКТА СЧИТАЕТСЯ ВСЕГДА, А ДВЕРЬ ОТКРЫВАЕТСЯ ТОЛЬКО В РЕШАЮЩЕМ ПРОХОДЕ.
        //
        // Раньше срез считался внутри `if (run == Run::Decide)`, и тень его не знала вовсе. Пока
        // им пользовалась одна дверь, это было верно; теперь по нему растёт счёт простоя, а тень
        // с нулевым простоем мерила бы не то поведение, которое потом будет исполняться.
        //
        // Ноль на первом такте значит «мерить не с чем»: дверь возьмёт заявленную частоту, а
        // счётчику простоя прибавится ноль — оба ответа верные.
        uint32 const slice = st.HasTicked ? (ctx.NowMs - st.LastTickMs) : 0u;
        bool const firstTick = !st.HasTicked;

        // ПРАВИЛО — ЛЕСТНИЦЫ, слово в слово (`Constellation.cpp:2263-2268`): в бою ноль, вне боя
        // прибавляем срез, и прибавляем, только пока не перевалили за час.
        //
        // ПЕРЕВАЛИТЬ МОЖНО, И ЭТО НЕ ОПЕЧАТКА, А ТОЧНЫЙ ПЕРЕНОС: у лестницы условие тоже
        // проверяет СТАРОЕ значение, так что последний прибавленный срез уносит счётчик чуть за
        // потолок. Порог сравнения — пять минут, потолок — час, разница в один такт роли не
        // играет; а расходиться с лестницей ради красоты числа в переносе нельзя. Разбор поймал
        // здесь мой комментарий, а не код: было написано «час сверху», чего ни одна из двух
        // реализаций не делает.
        //
        // РАЗРЫВ В НАБЛЮДЕНИИ — НЕ ПРОСТОЙ. Пока `Tick` не зовётся — движок выключен, не готов,
        // спутник вне мира — обнуления в бою не происходит, и накопленное потом отменило бы порог
        // стаи по неведению (Кодекс). Разрыв значит «не знаю, дрался ли», а не «не дрался»,
        // поэтому считаем строгую сторону и начинаем заново. Порог разрыва не выбран, а взят у
        // двери: секунда — то место, где модуль уже объявил измеренный срез недостоверным.
        if (firstTick || slice > OBSERVATION_GAP_MS || ctx.World.IsInCombat())
            st.NoCombatMs = 0;
        else if (st.NoCombatMs < NO_COMBAT_CAP_MS)
            st.NoCombatMs += slice;

        st.HasTicked  = true;
        st.LastTickMs = ctx.NowMs;

        if (run == Run::Decide)
        {
            ctx.Act.ResetTick(slice, &st.StepsRefused);
        }

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
            {
                // §11′ — ВОТ ГДЕ СТОЯЛО МОЛЧАНИЕ, и вот где оно убрано. Простой `continue`
                // делал дефект неотличимым от обычного выбора: кто-то ставит на действие,
                // которого нет, каждый такт и вечно, а в журнале это выглядит как «выбралось
                // что-то другое». Кричим ОДИН раз на спутника: при 114 спутниках и 4 Гц повторный
                // крик утопил бы журнал быстрее, чем донёс бы мысль.
                // ОДИН КРИК НА ИДЕНТИФИКАТОР, А НЕ НА СПУТНИКА: иначе второе, другое
                // ненаписанное действие навсегда спрячется за первым.
                uint32 const bit = uint32(bid.Action) < 32u ? (1u << uint32(bid.Action)) : 0u;
                if (bit && !(st.UnbackedSeen & bit))
                {
                    st.UnbackedSeen |= bit;
                    TC_LOG_ERROR("server.worldserver",
                        "Constellation ДВИЖОК {}: ставка на «{}», а такого действия не"
                        " зарегистрировано — оно не исполнится никогда",
                        ctx.World.Name(), NameOf(bid.Action));
                }
                ++st.BidsUnbacked;
                continue;
            }

            // §14 — ОТСРОЧКА ПРОВЕРЯЕТСЯ ДО `Useful`, И ПО ТОЧНОМУ КЛЮЧУ.
            //
            // До — потому что смысл запрета в том, чтобы не спрашивать вовсе: `Useful` у
            // взятия квеста вынужден был бы отвечать «да» (знак над головой на месте), и
            // пустой квестодатель выбирался бы каждый такт вечно.
            //
            // По ТОЧНОМУ ключу, а не по предмету: у одного NPC «взять нечего» не должно
            // запрещать сдать ему другой квест. Вид запрета объявляет само действие.
            BackoffKey suppress;
            suppress.Kind   = action->DeferKind();
            suppress.About  = bid.About;
            suppress.Detail = action->DeferDetail(bid);
            if (suppress.Kind != BackoffKind::None && Deferred(st, suppress, now))
            {
                // СЧИТАЕТСЯ ОТДЕЛЬНО: без этого в теневом режиме подавленное действие выглядит
                // ровно как непредложенное, а теневой отчёт — это мера всего переноса.
                ++st.BidsSuppressed;
                continue;
            }

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
                // §14 — УСПЕХ СНИМАЕТ ЗАПРЕТ, иначе он переживёт то изменение мира, которое
                // сделало его неверным. НО НЕ В ТЕНИ: там `ran` выставлен без исполнения, и
                // снимать по несостоявшемуся успеху значило бы чистить то, что не заработано.
                if (run != Run::Shadow && suppress.Kind != BackoffKind::None)
                    Allow(st, suppress);

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
