/*
 * Constellation — the three quest values, and the first thing in the engine that does real work.
 *
 * Contract: homelab/.agent/design/constellation-engine/values-tiers.md (second edition), plan
 * step 12. Each of these replaces a scan the ladder does by hand, and the reason they exist is a
 * measurement, not a preference: `FindTurnIn` walks the whole quest log and
 * `FindObjectiveTarget` sweeps the grid, both from the Idle branch, at four ticks a second and
 * 114 companions — the module's own comment at Constellation.cpp:2695 says so, and the world
 * thread went from 45 % to 84 % when they were called without a throttle.
 *
 * WHAT A VALUE MAY NOT DO, and the reason is not style:
 *
 *   * it may not write to `Companion` — `FindTurnIn` sets five fields and returns bool, which is
 *     why it is NOT called here and a pure list-producing scan was written instead;
 *   * it may not apply per-companion filters — backoffs, blacklists, preferences. Those live on
 *     `Companion`, `Calculate` cannot see them, and that is the point: a cache that expires
 *     because somebody's backoff changed is a cache whose interval means nothing.
 *
 * So each value answers a question about the WORLD, and the action that reads it answers the
 * question about itself.
 *
 * Copyright (C) 2026 Constellation contributors. Licensed under the GNU AGPL v3 — see COPYING.
 */
#include "../Values.h"
#include "../Engine.h"

#include <memory>

namespace
{
    using namespace Constellation::Ai;

    // ИНТЕРВАЛ ВЗЯТ У ВЕТКИ, КОТОРУЮ ЭТО ЗАМЕНЯЕТ, А НЕ ВЫБРАН ЗАНОВО. `Idle` уже дросселирует
    // свои сканы на `1000 + guid % 250` (Constellation.cpp:2591) — числом, которое пережило
    // измерение на живом составе. Джиттер добавляет `Value::Get` по гуиду, чтобы 114 пересчётов
    // не сошлись в один такт мирового потока.
    inline constexpr uint32 QUEST_SCAN_MS = 1000;

    // И ЭТОТ ИНТЕРВАЛ ТОЖЕ ВЗЯТ У ВЕТКИ, КОТОРУЮ ЗАМЕНЯЕТ. Перебор карты стоит дороже обзора:
    // лестница после каждого ставит `c.SeekCooldownMs = 300000 + guid % 61 c`
    // (Constellation.cpp:3695) и рядом пишет, зачем — «не перебирать карту каждые пять секунд»
    // (:3295). Джиттер добавляет `Value::Get` сам, тем же способом и по тому же гуиду, так что
    // берётся ровно число.
    inline constexpr uint32 GIVER_SEEK_SCAN_MS = 300000;

    class CompletedTurnInsValue final : public Value<TurnInList>
    {
    public:
        CompletedTurnInsValue() : Value(ValueId::CompletedTurnIns, QUEST_SCAN_MS) { }

    protected:
        void Calculate(Ctx& ctx, TurnInList& out) const override
        {
            out.Clear();        // ёмкость сохраняется — пересчёт не выделяет памяти вовсе
            ctx.World.ForEachCompletedTurnIn(
                [](void* user, TurnInCandidate const& t)
                {
                    static_cast<TurnInList*>(user)->Add(t);
                },
                &out);
        }
    };

    class GiversInSightValue final : public Value<GiverSightList>
    {
    public:
        GiversInSightValue() : Value(ValueId::GiversInSight, QUEST_SCAN_MS) { }

    protected:
        void Calculate(Ctx& ctx, GiverSightList& out) const override
        {
            out.Clear();
            ctx.World.ForEachQuestGiverInRange(Tuning().QuestGiverRange,
                [](void* user, GiverInSight const& g)
                {
                    static_cast<GiverSightList*>(user)->Add(g);
                },
                &out);
        }
    };

    // КУДА ИДТИ ЗА КВЕСТОМ — ОДИН ОТВЕТ, А НЕ СПИСОК, потому что идут всегда в одно место.
    //
    // ЛИЧНЫЕ ФИЛЬТРЫ ЗДЕСЬ ЕСТЬ, И ЭТО НЕ НАРУШЕНИЕ ПРАВИЛА СВЕРХУ. Правило запрещает фильтры,
    // живущие на `Companion`: `Calculate` их не видит, и кэш, протухающий от чужой отсрочки, —
    // кэш, у которого интервал ничего не значит. Таблица отсрочек ДВИЖКА лежит в `EngineState`,
    // то есть ровно там, куда `Calculate` дотягивается через `ctx.St`. Заголовок `Values.h` этот
    // случай уже разобрал: «`FindGiverByMap` сортирует по расстоянию ОТ ИГРОКА, фильтрует по ЕГО
    // фракции и ЕГО отсрочкам — значит каждое значение здесь на спутника».
    class GiverToSeekValue final : public Value<SeekTarget>
    {
    public:
        GiverToSeekValue() : Value(ValueId::GiverToSeek, GIVER_SEEK_SCAN_MS) { }

    protected:
        void Calculate(Ctx& ctx, SeekTarget& out) const override
        {
            out = SeekTarget();
            // БЕЗ СОСТОЯНИЯ НЕ СЧИТАЕМ ВОВСЕ, а не считаем без памяти. `SeekMemory::BackedOff ==
            // nullptr` — по её собственному заголовку «ЗАЩИТЫ НЕТ», а не «памяти нет»: выбор
            // вернул бы одну и ту же точку столько раз, сколько его спросят. Пустой ответ честнее
            // незащищённого.
            if (!ctx.St)
                return;
            SeekMemory mem;
            mem.Refused     = &QuestRefusedByEngine;
            mem.RefusedUser = &ctx;
            mem.BackedOff   = &SpawnBackedOffByEngine;
            mem.BackoffUser = &ctx;
            // `DiagOnce` НАМЕРЕННО НЕ СТАВИТСЯ. Строка «за потолком N точек» — прибор ОПЕРАТОРА,
            // чтобы решать про потолок, и её уже печатает лестница на того же спутника. Второй
            // такой же строкой движок сообщил бы не новость, а своё существование. Заголовок
            // `SeekMemory` разрешает это прямым текстом: «nullptr значит „не писать“».
            ctx.World.GiverToWalkTo(mem, &out);
        }
    };

    // КУДА ИДТИ ЗА ЦЕЛЬЮ ЗАДАНИЯ — ответ той же поднятой политики, что зовёт лестница в `Idle`
    // (`Constellation.cpp:3340`, `FindObjectiveSpotCore`). Личные фильтры здесь по тому же
    // праву, что у `GiverToSeek`: отсрочки движка лежат в `EngineState`, куда `Calculate`
    // дотягивается; гибели и смертельные места — у `ctx.Danger`, как у обхода целей.
    //
    // ИНТЕРВАЛ — ДВЕ СЕКУНДЫ, число лестницы для ПУСТОГО ответа (`c.TravelScanMs = 2000`,
    // `:3354`): найдя место, она ходит к нему не переспрашивая, а движок переоценивает — и
    // лестница на каждом пересчёте платит ту же цену (обход журнала и POI). Пересчёт ЧАЩЕ
    // двух секунд ничего не купил бы: ответ меняется, когда меняется журнал или отсрочка, и
    // на оба события действие зовёт `Invalidate` само.
    inline constexpr uint32 TRAVEL_SCAN_MS = 2000;

    class ObjectiveSpotValue final : public Value<TravelSpot>
    {
    public:
        ObjectiveSpotValue() : Value(ValueId::ObjectiveSpot, TRAVEL_SCAN_MS) { }

    protected:
        void Calculate(Ctx& ctx, TravelSpot& out) const override
        {
            // БЕЗ СОСТОЯНИЯ НЕ СЧИТАЕМ ВОВСЕ — то же правило, что у двух соседей выше.
            if (!ctx.St)
            {
                out = TravelSpot();
                return;
            }

            // НАЧАТЫЙ ПОХОД ДЕРЖИТСЯ ЗА СВОЮ ТОЧКУ — как лестница за `TravelPos` весь
            // `Travelling`. Без этого «ближайшее место» на ходу переизбирается между двумя
            // квестами, ставка роняется по несовпадению квеста, и `AdvanceWalk` заводит учёт
            // заново на каждой смене — ни «полминуты без приближения», ни потолок пути не
            // наступают никогда (Кодекс, поход, пункт 3). Точка отпускается тем же, чем у
            // лестницы кончается `Travelling`: приход и смертельное место откладывают квест
            // (и отсрочка снимает удержание здесь), неудача пути и отмена снимают `Running`.
            if (ctx.St->Running == ActionId::TravelToObjective && out.Worth
                && out.MapId == ctx.World.MapId()
                && ctx.St->RunningAbout == Subject::OfQuest(out.QuestId)
                && !QuestTravelBackedOffByEngine(&ctx, out.QuestId))
                return;

            out = TravelSpot();
            TravelMemory mem;
            mem.BackedOff = &QuestTravelBackedOffByEngine;
            mem.User      = &ctx;
            // Приборы («квест стоил гибелей», «место смертельно») не ставятся: те же две строки
            // на того же спутника уже печатает лестница — см. довод у `GiverToSeek`.
            ctx.World.ObjectiveSpotToWalkTo(ctx.Danger, mem, &out);
            out.MapId = ctx.World.MapId();
        }
    };

    class GiversByIndexValue final : public Value<GiverIndexList>
    {
    public:
        GiversByIndexValue() : Value(ValueId::GiversByIndex, QUEST_SCAN_MS) { }

    protected:
        void Calculate(Ctx& ctx, GiverIndexList& out) const override
        {
            out.Clear();
            ctx.World.ForEachGiverOnMap(Tuning().GiverSeekRange,
                [](void* user, GiverOnMap const& g)
                {
                    static_cast<GiverIndexList*>(user)->Add(g);
                },
                &out);
        }
    };
}

namespace Constellation::Ai
{
    // ОДНО МЕСТО, ГДЕ ВИДНО, ЧТО СУЩЕСТВУЕТ. Регистрация вразброс — это инвентарь, который
    // приходится собирать грепом; `Seal()` при этом всё равно откажет, но уже на живом мире.
    void RegisterQuestValues(Engine& engine)
    {
        engine.RegisterValue<ValueId::CompletedTurnIns>(std::make_unique<CompletedTurnInsValue>());
        engine.RegisterValue<ValueId::GiversInSight>(std::make_unique<GiversInSightValue>());
        engine.RegisterValue<ValueId::GiversByIndex>(std::make_unique<GiversByIndexValue>());
        engine.RegisterValue<ValueId::GiverToSeek>(std::make_unique<GiverToSeekValue>());
        engine.RegisterValue<ValueId::ObjectiveSpot>(std::make_unique<ObjectiveSpotValue>());
    }
}
