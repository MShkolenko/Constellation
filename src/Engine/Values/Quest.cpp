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
    }
}
