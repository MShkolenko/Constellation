/*
 * Constellation — GatherObjective: open a quest object, loot a node, apply a blank or a rune at a
 * focus. The second action ported after the engine became the master (2026-09-13). The bodies
 * are the ladder's (`GatherFocusCore`, `GatherOpenCore`, `GatherLeaveCore`) and the gather
 * memory stays in the companion's slot — this file is only the walk, the door and the bid.
 *
 * Copyright (C) 2026 Constellation contributors. Licensed under the GNU AGPL v3 — see COPYING.
 */
#include "../Values.h"
#include "../Engine.h"
#include "../ClientAct.h"

#include <memory>

namespace
{
    using namespace Constellation::Ai;

    inline constexpr float YARD_COST = 0.01f;
    // ЧИСЛА ЛЕСТНИЦЫ (`Gathering`, `:4283`, `:4482`): к фокусу — 3 ярда, к объекту — 4; отсрочка
    // «до объекта не дойти» — пять минут (`:4514`), к фокусу — тоже (`:4293`).
    inline constexpr float  FOCUS_ARRIVED_YARDS  = 3.0f;
    inline constexpr float  OBJECT_ARRIVED_YARDS = 4.0f;
    inline constexpr uint32 UNREACHABLE_MS       = 300000;

    // СБОР. Точка — из `GatherSpot` (тот же отбор, что у `Idle`, память — слот спутника).
    //
    // ЧЕГО ЗДЕСЬ НЕТ, НАЗВАНО: отсчёты лестницы «20 с без приближения», «45 с всего»,
    // «маршрута нет» (`:4488-4515`) — их место занял `AdvanceWalk` с его собственными
    // порогами; исход тот же: пять минут отсрочки на точку. Приход к объекту решает ядро
    // (`UsableObjectAt`), а не расстояние; «пришёл, а объекта нет» — по расстоянию, как у
    // лестницы (`:4482`).
    class GatherObjectiveAction final : public Action
    {
    public:
        GatherObjectiveAction() : Action(ActionId::GatherObjective) { }

        BackoffKind DeferKind() const override { return BackoffKind::ObjectUnreachable; }

        bool Useful(Ctx& ctx, Bid const& bid) override
        {
            if (bid.About.What() != Subject::Kind::Spawn)
                return false;
            GatherSpot const& gs = Val<ValueId::GatherTarget>(ctx);
            return gs.SpawnId && gs.SpawnId == bid.About.Id() && gs.MapId == ctx.World.MapId();
        }

        bool Possible(Ctx& ctx, Bid const&) override { return ctx.St != nullptr; }

        float Score(Ctx& ctx, Bid const& bid, float relevance) const override
        {
            GatherSpot const& gs = Val<ValueId::GatherTarget>(ctx);
            if (!gs.SpawnId || gs.SpawnId != bid.About.Id())
                return relevance;
            return relevance - ctx.World.DistanceTo2d(gs.Where) * YARD_COST;
        }

        bool Execute(Ctx& ctx, Bid const& bid) override
        {
            if (!ctx.St)
                return false;
            GatherSpot const& gs = Val<ValueId::GatherTarget>(ctx);
            if (!gs.SpawnId || gs.SpawnId != bid.About.Id())
                return false;
            // СЛОТ ДОЛЖЕН БЫТЬ НАСТРОЕН НА ЭТУ ЖЕ ТОЧКУ: тела читают `c.Gather*`, и если их
            // сбросил уход (`GatherLeaveCore`) или чужой путь — ставка снята, отбор заново.
            if (ctx.World.GatherSpawn() != gs.SpawnId)
            {
                ctx.St->Values.GatherTarget.Invalidate();
                return false;
            }

            float const dt = ctx.Act.SliceSeconds();
            uint32 const sliceMs = uint32(dt * 1000.0f);
            float const d = ctx.World.DistanceTo(gs.Where);
            float const arrive = gs.Focus ? FOCUS_ARRIVED_YARDS : OBJECT_ARRIVED_YARDS;

            // ---- У ТОЧКИ: общее тело -----------------------------------------------------
            if (gs.Focus)
            {
                if (d <= arrive)
                    return Outcome(ctx, ctx.World.GatherFocus(ctx.Act, ctx.St->Move));
            }
            else if (ctx.World.UsableObjectAt(gs.SpawnId))
                return Outcome(ctx, ctx.World.GatherOpen(gs.SpawnId, ctx.Act, ctx.St->Move));
            else if (d <= arrive)
                return Outcome(ctx, ctx.World.GatherArrivedEmpty());

            // ---- ДОРОГА: движок судит сам ------------------------------------------------
            bool const going = WalkTowards(ctx, gs.Where, arrive, dt);
            bool const stalled = !going && d > arrive;
            if (AdvanceWalk(ctx, bid.About, d, sliceMs, stalled) != WalkVerdict::Going)
            {
                // ТОТ ЖЕ ИСХОД, ЧТО У ЛЕСТНИЦЫ: точке — отсрочка в памяти слота, ставка снята.
                ctx.World.GatherUnreachable(UNREACHABLE_MS);
                ctx.St->Values.GatherTarget.Invalidate();
                return false;
            }
            return true;
        }

        // СТАВКУ СНЯЛИ НА ПОЛПУТИ — точку отпускаем так же, как лестница уходит из `Gathering`
        // (`GatherLeave`): свой каст замка отменён, резерв снят, слот чист. Без отсрочки: точка
        // не виновата, и отбор может назвать её снова.
        void Cancel(Ctx& ctx, Subject const&, CancelReason) override
        {
            if (ctx.St)
                ctx.World.GatherCancel();
        }

    private:
        // nullptr — «стою, вернусь тем же тактом» (каст замка, откат); иначе ушли с причиной,
        // слот уже сброшен телом, точка отпускается.
        static bool Outcome(Ctx& ctx, char const* why)
        {
            if (!why)
                return true;
            ctx.St->Values.GatherTarget.Invalidate();
            return false;
        }
    };
}

namespace Constellation::Ai
{
    void RegisterGatherActions(Engine& engine)
    {
        engine.Register(std::make_unique<GatherObjectiveAction>());
    }
}
