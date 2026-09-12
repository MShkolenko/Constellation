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
#include "Values.h"
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

        // §12 — КЭШ ЗНАЧЕНИЙ ЛЕЖИТ ЗДЕСЬ, А НЕ РЯДОМ, и это единственная причина,
        // по которой `Discard` покрывает его бесплатно: он обнуляет всю структуру. Склада
        // вне её нет ни одного, и пока его нет — третьей политики жизненного цикла не
        // существует. Буферы резервируются в конструкторах `CappedList`, один раз.
        ValueSlots Values;

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

        // §14 — ОТСРОЧКИ. Плоский массив фиксированного размера, ровно как `TriggerLastMs` и по
        // той же причине: `Action` и `Strategy` — ОБЩИЕ объекты, состояния держать не имеют
        // права (эту ошибку однажды сделали с `Trigger`, и минутный триггер срабатывал раз в
        // минуту НА ВЕСЬ состав), а буфер `Value` переписывается при каждом пересчёте, так что
        // значение накапливать память не может. Остаётся `EngineState`.
        //
        // ПЕРЕЖИВАЕТ `Reset` НАМЕРЕННО. `Reset` гасит КЭШИ — очередь, текущее действие,
        // значения; значение это снимок мира и пересчитывается дёшево. Отсрочка — память о
        // РЕЗУЛЬТАТЕ, у неё другое время жизни. Эпоха у спутника меняется несколько раз в час
        // (замер 2026-09-09: 1 → 4 за тридцать минут), так что чистка при `Reset` означала бы
        // забывание каждые несколько минут и приветствие пустому квестодателю снова и снова.
        // Умирает по сроку — или вместе со спутником в `Discard`, который обнуляет всю
        // структуру и потому покрывает таблицу бесплатно.
        // МОМЕНТ УСТАНОВКИ И СРОК, А НЕ МОМЕНТ ИСТЕЧЕНИЯ. `GameTime::GetGameTimeMS()` —
        // `uint32`, он переполняется, и проверка «сейчас >= до» через переполнение отвечает
        // наоборот. Весь остальной движок сравнивает интервалами (`nowMs - slot.LastMs >=
        // interval` в `Value<T>::Get`), потому что беззнаковая разность переполнение переживает.
        struct BackoffEntry
        {
            BackoffKey Key;
            uint32     SetAtMs = 0;
            uint32     TtlMs   = 0;
        };
        BackoffEntry Backoffs[BACKOFF_CAP];
        uint8  BackoffCount       = 0;

        // ДИАГНОСТИКА, А НЕ УКРАШЕНИЕ: по ней и только по ней выбирается `BACKOFF_CAP` при
        // возврате состава к 122. Вытеснение ЖИВОЙ записи — дефект ёмкости, а не рабочий режим.
        uint32 BackoffFull        = 0;
        uint32 BackoffEvictedLive = 0;

        // §14 — ПОДАВЛЕННЫЕ СТАВКИ СЧИТАЮТСЯ ОТДЕЛЬНО. Без этого счётчика в теневом режиме
        // подавленное действие выглядит ровно как непредложенное, а чтение теневых отчётов и
        // есть мера этого переноса.
        // §31 — ОДНА ДОРОГА НА СПУТНИКА. У него в каждый момент одно выбранное действие, значит
        // и одна цель пути; смена предмета ставки — это новая дорога, а не продолжение старой.
        WalkProgress Walk;

        // §31 — И САМО СОСТОЯНИЕ ДВИГАТЕЛЯ. `Walk` выше — прогресс уровня ДЕЙСТВИЯ: приближаемся
        // ли к цели. `Move` — внутренности ходьбы: маршрут ядра и место в нём, отступы вбок,
        // отсрочка построителя, примерзание. Два разных яруса, и путать их дорого: один отвечает
        // «стоит ли ещё идти», другой «как сделать следующий шаг».
        MoveState Move;

        // §6 — КОГДА ЭТОТ СПУТНИК ПРИНИМАЛ РЕШЕНИЕ В ПРОШЛЫЙ РАЗ. Из этой разности считается
        // бюджет шага: мерка движения обязана следовать за настоящим тактом, а не за числом в
        // заголовке. Именно РЕШАЮЩИЙ такт: в тени такт не открывается вовсе и отметка не
        // ставится — иначе комментарий обещал бы больше, чем делает код (Кодекс, пункт 3).
        //
        // ФЛАГ, А НЕ НОЛЬ КАК ПРИЗНАК. Ноль — возможное настоящее показание часов после
        // переполнения, и тогда следующий такт снова сочли бы первым (Кодекс, пункт 1). Та же
        // ошибка, что часовое `1.0e9f` в `WalkProgress`, сделанная часом позже в другом месте.
        // СКОЛЬКО Я НЕ ДРАЛСЯ. Правило — лестницы (`Constellation.cpp:2263-2268`): в бою ноль,
        // вне боя растёт, прибавляем только пока не перевалили за час. По нему отступает порог
        // стаи, и без него стайные цели стали бы невыполнимы навсегда — это её собственный
        // разбор, не мой довод.
        //
        // И РАЗРЫВ В НАБЛЮДЕНИИ СЧИТАЕТСЯ ЗА БОЙ, а не за простой: пока движок не тикал, он не
        // знает, дрался ли спутник, и строгая сторона здесь — начать счёт заново.
        //
        // ПЕРЕЖИВАЕТ `Reset`, как и таблица отсрочек: это мера МИРА, а не снимок брошенной
        // работы. Умирает с `Discard`.
        uint32 NoCombatMs = 0;

        // БОЙ — ОДИН МЕХАНИЗМ С ОДНИМ ВЛАДЕЛЬЦЕМ ЦЕЛИ (решение 2026-09-11, п. 1). Всё, что у
        // лестницы лежало в `Companion` порознь для двух режимов и шести помощников: кого
        // бьём, база телеметрии на момент подтверждённого входа, сторожа, память каста, счёт
        // лута. Позиция, жизнь, дистанция и снимок тапа ЧИТАЮТСЯ каждый такт, не хранятся.
        //
        // ПЕРЕЖИВАЕТ `Reset` — постановление Мастера, и вот почему: `c.ModeMs` идёт в прологе
        // лестницы независимо от того, кто взял такт, её предохранители (`WalkCapMs`) сработают
        // под боем движка и сменят эпоху — `Reset` — и бой, стёртый на середине замаха, был бы
        // тем же дефектом, что очередь, полная проигравших, только в другом пальто. Кончается
        // ТОЛЬКО терминальным исходом (победа, брошен, погиб) или `Discard`. `Cancel` по
        // `EnteredFromOutside` бой НЕ кончает — действие само это различает.
        struct FightState
        {
            ObjectGuid    Victim;               // пусто = боя нет
            uint32        VictimEntry = 0;
            bool          Engaged     = false;  // ядро приняло замах: `GetVictim() == Victim`
            BlowsSnapshot Base;                 // отсчёт ЭТОГО боя (`RegisterAndSnapshot`)
            // СВОЙ УРОН, А НЕ ЗДОРОВЬЕ ЦЕЛИ — у лестницы это поле звалось `VictimHp` и лгало
            // именем (Кодекс): максимум накопленного собственного урона, по нему сторож.
            uint64        DealtHigh   = 0;
            uint32        NoDamageMs  = 0;      // 30 с без СВОЕГО урона — не наша цель
            uint32        CastMs      = 0;      // раз в полторы секунды, первое — сразу
            uint32        WantedCheckMs = 0;    // раз в секунду: нужна ли ещё заданию
            uint32        FightMs     = 0;      // предохранитель — пять минут
            uint32        LastTickMs  = 0;      // из чего считается срез
            // ДИСТАНЦИЯ ВСТУПЛЕНИЯ — считается ОДИН РАЗ НА БОЙ (лестница: обход книги у 122
            // спутников на каждом такте — полтысячи обходов в секунду впустую). Флаг, а не
            // «-1 = посчитали», как у неё: число с двумя смыслами и здесь читалось бы неверно.
            float         EngageRange = 0.0f;
            bool          EngageRangeKnown = false;
            // ОТВОД: с чем шли в бой (заступники цели и центр пачки — снимок обхода на момент
            // выбора) и как идёт отход. `KiteMs` 0 = ещё не пробовали, `~0u` = некуда пятиться
            // (больше не пробуем в этом бою) — оба смысла лестницы, `:4014`, `:4024`.
            uint32        Assists     = 0;
            Position      PackCenter;
            bool          PackKnown   = false;
            bool          Kiting      = false;
            uint32        KiteMs      = 0;
            Position      KiteTo;
            CastMemory    Cast;
            LootCounters  Loot;                 // за всё время, как у лестницы
        };
        FightState Fight;

        // РАЗГОВОР — состояние попытки (то же, что `c.Talk` у лестницы), пауза всему действию
        // после шести особей без зачёта (лестница: `ToolActionMs`) и счётчик зачётов.
        // ВОСКРЕС — ОТДЫШАТЬСЯ, ПОКА НЕ ВОССТАНОВИЛСЯ, а не «пока ниже порога»: лестница входит
        // в `Recovering` по СОБЫТИЮ подъёма (`Constellation.cpp:2022`, `:2188`) и держит режим
        // до `RestedEnough`. Порог `NeedsRest` этого не видит: поднявшийся у тела бывает выше
        // него и ниже `RestedEnough` — и движок в тени шёл за квестом (замер: 3 расхождения
        // «перевожу дух «поднялся у своего тела»»). Событие видит только лестница — она и
        // ставит флаг и она же снимает — выходом из `Recovering`; движок его только читает.
        bool RestAfterRevive = false;

        // ОТДЫХ — РЕШЕНИЕ, КОТОРОЕ ПЕРЕЖИВАЕТ `Reset`, как и бой. Гистерезис отдыха («ушёл ниже
        // одного порога, вернусь выше другого») держался на `Running == Rest`, а `Reset` на смене
        // эпохи его стирает: в тени лестница сама входит в `Recovering` — это смена режима, эпоха,
        // `Reset` — и движок на следующем такте, стоя в полосе между порогами, отдыха уже «не
        // хочет» и ставит сдачу или поход (замер 12:13: «сдать квест» 14.9–17.9 при «было
        // перевести дух», согласие 44 %). Вживую тот же разрыв случился бы от любой смены эпохи
        // посреди отдыха. Флаг ставит `Reset`, снимает `RestWanted`, когда отдых кончен или запрещён.
        bool RestHeld = false;

        TalkState Talk;
        uint32    TalkPauseSetMs = 0;   // пауза — «когда поставлена + сколько», а не «до когда»:
        uint32    TalkPauseMs = 0;      // разность uint32 переживает переполнение часов, сумма — нет (Кодекс)
        uint32    Talked = 0;

        bool TalkPaused(uint32 nowMs) const { return TalkPauseMs && nowMs - TalkPauseSetMs < TalkPauseMs; }

        // СКОЛЬКО ПОДРЯД ОТДЫХАЕМ. Нужен ради потолка: у лестницы после `RestMaxMs` отдых
        // запрещается вдвое дольше, и её разбор объясняет, почему просто «выйти по сроку» не
        // работает — `NeedsRest` всё ещё истинно, и следующий такт возвращает в отдых.
        //
        // ОБНУЛЯЕТСЯ, КОГДА ОТДЫХ НЕ ВЫБРАН, а не по времени: «подряд» здесь и значит «подряд».
        uint32 RestingMs = 0;

        // ЗАНЯТОСТЬ ТАКТА — то, чего строка решения не измеряет по устройству: она пишется только
        // на СМЕНЕ выбора, значит короткие окна не видят устойчивых состояний вовсе. Здесь
        // считается, СКОЛЬКО тактов движку было чем заняться, а не сколько раз он передумал.
        uint32 TicksIdle      = 0;      // пробовать было нечего
        uint32 TicksAttempted = 0;      // взялся и не довёл
        uint32 TicksCommitted = 0;      // провёл действие через дверь
        uint32 ReportAtMs     = 0;      // когда сказать и обнулить
        // И КОГДА ОТДЫХАЛИ В ПОСЛЕДНИЙ РАЗ. Без этого «подряд» было неправдой: счётчик не
        // обнулялся, когда отдых просто переставали выбирать, и через минуту чужой работы
        // продолжал с прежнего, упираясь в потолок раньше срока (разбор). Разрыв лечится тем же
        // способом и тем же порогом, что и у счёта простоя.
        uint32 RestedAtMs = 0;


        bool   HasTicked          = false;
        uint32 LastTickMs         = 0;

        // §6 — ОТКАЗЫ ШАГА. Дверь отвергает по четырём причинам и каждый раз кричит в журнал, но
        // в сводке спутник с отказом на КАЖДЫЙ шаг неотличим от стоящего.
        uint32 StepsRefused       = 0;

        uint32 BidsSuppressed     = 0;

        // Крик о переполнении — ОДИН РАЗ на спутника, тем же приёмом, что `UnbackedSeen`.
        bool   BackoffOverflowLogged = false;

        // §11 — reported, because "456 ticks a second is affordable" is an argument, not a
        // measurement. A number that is never printed is a number nobody checks.
        uint32 BidsPushed       = 0;
        uint32 BidsDropped      = 0;         // hit a cap — a defect to find, not a cost to absorb
        // СЛИТО С УЖЕ СТОЯЩЕЙ. Стратегии ставят одни и те же пары «действие + предмет» каждый
        // такт, а ставка живёт три секунды — без слияния каждая лежала бы двенадцатью копиями
        // (замер: очередь 23-31 при крышке 32, `сброшено` до 428 на спутника). Число здесь —
        // доказательство, что слияние работает: оно идёт, а `сброшено` стоит. ЗА МИНУТУ:
        // обнуляется отчётом занятости, а не сбросом состояния.
        uint32 BidsMerged       = 0;
        uint32 BidsExpired      = 0;
        uint32 ActionsCancelled = 0;
        uint32 ReentriesReset   = 0;         // §2″ — how often somebody else moved us
        uint32 ValuesRecomputed = 0;         // §12 — число, по которому проверяют интервалы
        uint32 BidsUnbacked     = 0;         // §11′ — ставка на действие, которого нет. Дефект,
                                             // а не состояние: раньше это был `continue` в тишине
        // КАКОЙ ИМЕННО, А НЕ ТОЛЬКО СКОЛЬКО. Крик был один на спутника, и второй,
        // ДРУГОЙ незарегистрированный идентификатор оказался бы скрыт первым — а он вполне
        // может быть важнее (Кодекс, п. 4). Одно слово на тринадцать действий — тот же приём,
        // что у маски стратегий.
        uint32 UnbackedSeen     = 0;
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

        // §12 — ИДЕНТИФИКАТОР ЗАДАЁТ ТИП, А НЕ СОПРОВОЖДАЕТ ЕГО. Поставщик,
        // зарегистрированный под чужим идентификатором, — ошибка СБОРКИ, а не неверный
        // `static_cast` на живом рилме: тип берётся из той же строки того же списка,
        // что и сам идентификатор.
        template <ValueId Id>
        void RegisterValue(std::unique_ptr<Value<typename ValueTraits<Id>::Type>> value)
        {
            RegisterValueBase(Id, std::move(value));
        }

        // Чтение. Слот берётся из состояния СПУТНИКА, поэтому тень и шов, у которых
        // разные `EngineState`, имеют разные кэши — и тень меряет свой выбор, а не чужой.
        // Читать через свободную `Val(ctx)` из Values.h — она сама найдёт состояние в `Ctx`.
        // Этот вариант остаётся для тех, у кого состояние уже на руках — для самого такта.
        template <ValueId Id>
        typename ValueTraits<Id>::Type const& Val(EngineState& st, Ctx& ctx) const
        {
            using T = typename ValueTraits<Id>::Type;
            auto const* v = static_cast<Value<T> const*>(_values[size_t(Id)].get());
            auto& slot = ValueTraits<Id>::SlotOf(st.Values);
            bool const was = slot.Computed;
            uint32 const wasMs = slot.LastMs;
            T const& out = v->Get(ctx, slot, ctx.NowMs);
            if (!was || slot.LastMs != wasMs)
                ++st.ValuesRecomputed;
            return out;
        }

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
        // ЧЕМ КОНЧИЛСЯ ТАКТ — ТРИ ИСХОДА, А НЕ ДВА.
        //
        // Шву нужен НЕ «получилось ли», а «занят ли спутник»: действие вправе написать в мир и
        // потом вернуть ложь — поворот и выбор цели уходят раньше, чем может не удаться замах;
        // два приветствия уходят раньше, чем выяснится, что меню пусто; остановка уходит раньше,
        // чем сработает потолок отдыха. Отдав ход лестнице после такого отказа, мы получили бы
        // двух управляющих ВНУТРИ ОДНОГО ТАКТА.
        //
        // Поэтому `Attempted` и `Committed` для шва одно и то же — такт принадлежит движку, —
        // а различает их журнал и замер, которым нужен именно исход. Один `bool` на два вопроса
        // снова дал бы ответ, который читают не тем вопросом.
        enum class TickResult : uint8
        {
            Idle,       // пробовать было нечего: очередь пуста, всё подавлено или отсеяно
            Attempted,  // исполнение начиналось и не дошло до конца — мир уже мог быть тронут
            Committed,  // ровно одно действие проведено через дверь
        };

        TickResult Tick(EngineState& st, Ctx& ctx, uint32 modeEpoch, Run run = Run::Decide);

    private:
        // СЧЁТ В ОДНОЙ ТОЧКЕ. Возвратов у такта три, и три отдельных инкремента разошлись бы на
        // первой же правке; пропустить эту, не убрав `return`, нельзя.
        static TickResult Finish(EngineState& st, TickResult r);

    public:

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

    // §14 — ОТСРОЧКИ. Три операции, и все три на `EngineState`, а не на действии: действия
    // общие, состояния держать не имеют права.
    //
    //   `Deferred` — жив ли запрет по этому ключу. Просроченная запись при чтении считается
    //               отсутствующей и тут же освобождается: подметания нет и не нужно.
    //   `Defer`    — поставить запрет. При переполнении вытесняет сперва просроченное, затем
    //               запись с НАИМЕНЬШИМ ОСТАТКОМ — не старейшую: возраст создания не равен
    //               оставшейся ценности, и вытеснение старейшей это способ заставить занятого
    //               спутника забыть именно то, что нужнее всего. Вытеснение живой записи
    //               считается отдельно и является дефектом ЁМКОСТИ, а не рабочим режимом.
    //   `Allow`    — снять запрет. Зовётся при УСПЕХЕ: иначе запрет переживёт то изменение
    //               мира, которое сделало его неверным.
    // ЧИТАЮЩАЯ: не подметает. Подметание живёт в `Defer`, который и так обходит таблицу
    // ради проверки на дубль и вытесняет просроченное первым. Константность здесь не
    // украшение: память об отказах читается из политики выбора квеста, а та константна.
    static bool Deferred(EngineState const& st, BackoffKey const& key, uint32 nowMs);
    // Возвращает ИСТИНУ, если пришлось вытеснить ЖИВУЮ запись — вызывающий кричит об этом,
    // потому что имя спутника есть только у него, а безымянная жалоба бесполезна.
    static bool Defer(EngineState& st, BackoffKey const& key, uint32 ttlMs, uint32 nowMs);
    static void Allow(EngineState& st, BackoffKey const& key);

        // Сколько значений отвергнуто при регистрации — читается в `Seal()`, где есть журнал.
        uint32 ValuesRejected() const { return _valuesRejected; }

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
        // Нешаблонная половина регистрации: шаблон выше только связывает тип с именем.
        void RegisterValueBase(ValueId id, std::unique_ptr<ValueBase> value);
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
        std::vector<std::unique_ptr<ValueBase>>  _values;       // indexed by ValueId
        bool _ready       = false;
        bool _instantTaxi = false;           // read once at Seal; see Seal()'s comment

        // §9 — ОТКАЗ В РЕГИСТРАЦИИ НЕ ИСЧЕЗАЕТ БЕССЛЕДНО. Регистрация идёт до того, как
        // логирование заведомо настроено, поэтому кричать на месте нельзя — но и молчать
        // нельзя: отвергнутый триггер выглядит как ненаписанный, а Seal() при этом сказал бы
        // «готов». Считаем здесь, докладываем и отказываем в готовности там, где есть журнал.
        uint32 _rejected = 0;
        uint32 _valuesRejected = 0;      // дубль или неизвестный идентификатор значения
    };

    // §14 — КАК ДЕЙСТВИЕ ПРОСИТ ОТСРОЧКУ. Кодекс предлагал расширить РЕЗУЛЬТАТ `Execute`,
    // чтобы действие вообще не касалось состояния спутника. Не сделано так, и причина названа:
    // тип принудил бы каждое из тринадцати оставшихся действий нести церемонию ради средства,
    // которым воспользуются несколько. Возражение было про ТАЙНОЕ изменение общего состояния —
    // здесь оно не тайное: имя названо, состояние берётся из `Ctx`, и записывает по-прежнему
    // движок. Если действия начнут злоупотреблять, поменять на результат будет дешевле, чем
    // сейчас откатывать церемонию.
    void Defer(Ctx& ctx, BackoffKind kind, Subject const& about, uint8 detail, uint32 ttlMs);

    // §14 — ПАМЯТЬ ДВИЖКА В ВИДЕ, КОТОРЫЙ ПОНИМАЮТ ОБЩИЕ ПОЛИТИКИ. Одна пара на весь движок:
    // обе читают ту же таблицу отсрочек, только разными ключами — квест `CoreRefused`,
    // точка `Unreachable`. Обе принимают `Ctx const*` в `void const*`, потому что политика,
    // которая их зовёт, константна.
    bool QuestRefusedByEngine(void const* user, uint32 questId);
    bool SpawnBackedOffByEngine(void const* user, uint32 spawnId);
    // Память похода: «к этому квесту сходил впустую» и «до его места не дойти». Ключ — квест,
    // и на `OfQuest` эти два вида больше никто не ставит (`CoreRefused` — третий, свой).
    bool QuestTravelBackedOffByEngine(void const* user, uint32 questId);

    // Разговор через дверь: отправитель над `ctx.Act`, память над таблицей отсрочек.
    TalkOutcome TalkThroughDoor(Ctx& ctx, ObjectGuid who, TalkPlan const& plan, TalkState& st);
    bool TalkRefuseThroughDoor(Ctx& ctx, ObjectGuid who, TalkPlan const& plan, TalkState& st);
    void RegisterTalkActions(Engine& engine);

    // ОТДЫХ ХОЧЕТСЯ — тот же предикат, что у `Rest::Useful`: ниже порога, или уже отдыхаем и ещё не
    // восстановились, или поднялись и ещё не восстановились. Один на действие и на стратегии,
    // которые ему уступают: две копии разошлись бы на первом же изменении.
    bool RestWanted(Ctx& ctx);

    // ПАМЯТЬ ДВИЖКА ДЛЯ ОБХОДА ЦЕЛЕЙ — та же таблица отсрочек, только вопросов пять.
    //
    // Шестого, `TalkRetry`, здесь нет НАМЕРЕННО: по разбору дизайна это стадия управления, а не
    // исход, и таблица ведёт журнал исходов. Держать стадию в ней значило бы делать из неё
    // неявную копию `switch` — ровно то, ради ухода от чего движок и пишется.
    FightMemory EngineFightMemory(Ctx& ctx);

    // §31 — ОДИН ШАГ ДОРОГИ: обновить прогресс и сказать, идти дальше или бросить.
    //
    // `dist` — нынешнее расстояние до цели, `stalled` — ответ движка движения «упёрлись».
    // Первый вызов к новому предмету сам перезапускает счёт: вызывающему не надо помнить,
    // сменил он цель или нет, а забытый перезапуск — ровно тот дефект, который этот примитив
    // и убирает.
    WalkVerdict AdvanceWalk(Ctx& ctx, Subject const& toward, float dist, uint32 sliceMs, bool stalled);

    // §31 — ШАГ ДЕЙСТВИЯ К ТОЧКЕ. ОТПРАВЩИКА В ПОДПИСИ НЕТ, И ЭТО ГЛАВНОЕ.
    //
    // Первая версия принимала `MoveSendFn send, void* user` и объявляла в комментарии, что
    // инвариант 0 цел, «потому что пишет `ClientAct::Step` внутри отправщика». Тип этого не
    // требовал: любое действие могло передать свой способ записи, и шов честно бы его вызвал.
    // Нашёл Кодекс. Это тот же способ рассуждать, который в модуле уже разобран и отвергнут на
    // `Constellation.cpp:2801` — «исполнять она не может ПО ТИПУ, а не по тому, что её никто не
    // зовёт».
    //
    // Так что действие говорит только КУДА идти. ЧЕМ писать — не его выбор и не его параметр, а
    // состояние ходьбы берётся из `ctx.St->Move` по той же причине: два действия с разными
    // `MoveState` на одного спутника — это два маршрута, спорящих за одни ноги.
    //
    // `false` — «дошли или не можем», как у двигателя. Без состояния ходить негде, и это тоже
    // «не можем», а не отдельный исход.
    bool WalkTowards(Ctx& ctx, Position const& to, float stopAt, float dt);

    // ДВЕ ПОДНЯТЫЕ ПОЛИТИКИ БОЯ ЧЕРЕЗ ДВЕРЬ — той же формы, что `WalkTowards`: отправщика в
    // подписи нет, он один и строится здесь над `ctx.Act`. Действие говорит «кого» и «что»;
    // чем шлётся — не его параметр.
    bool CastThroughDoor(Ctx& ctx, ObjectGuid victim, CastMemory& m);
    bool LootThroughDoor(Ctx& ctx, ObjectGuid corpse, LootCounters& n);
}

#endif // CONSTELLATION_ENGINE_ENGINE_H
