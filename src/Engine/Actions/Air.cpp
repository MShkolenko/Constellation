/*
 * Constellation — UseHearthstone and TakeFlight: the two ways a road gets shorter. The fourth
 * item ported after the engine became the master (2026-09-13). The bodies are the ladder's
 * (`HearthWorthCore`/`HearthCastCore`, `PlanFlight`, `FindFlightMasterCore`, `TakeFlightCore`)
 * over the companion's slot; the plan is asked by the travelling actions as a PREREQUISITE of
 * their road — the ladder asked it at the same moment, before switching to the road's mode.
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

    // ЧИСЛА ЛЕСТНИЦЫ (`TakingFlight`, `:3631-3634`, `:3649`, `:3689`): к точке мастера — стоп 15,
    // «пришли» — 20; не дойти — десять минут откатa на полёты.
    inline constexpr float  MASTER_ARRIVED_YARDS = 20.0f;
    inline constexpr float  MASTER_STOP_YARDS    = 15.0f;
    inline constexpr float  MASTER_TALK_YARDS    = 5.0f;
    inline constexpr uint32 FLIGHT_FAIL_MS       = 600000;
    // НЕ ЧАЩЕ РАЗА В МИНУТУ НА СПУТНИКА (замер 10:13-10:36: 1859 планов за 23 минуты, ноль
    // взлётов): две дороги, сменяющие друг друга после каждого боя, перестраивали план на каждой
    // смене — маршруты графа раз в шесть секунд. Метка ставится ДО попытки, чтобы и неудачный
    // план ждал минуту (Кодекс).
    inline constexpr uint32 FLIGHT_REPLAN_MS     = 60000;

    // КАМЕНЬ. С 2026-09-14 читается прямо из начала дороги (`AirAtRoadStart`); действие оставлено
    // зарегистрированным, но ставок на него больше нет — снять вместе с `HearthTarget` при уборке.
    class UseHearthstoneAction final : public Action
    {
    public:
        UseHearthstoneAction() : Action(ActionId::UseHearthstone) { }

        bool Useful(Ctx& ctx, Bid const& bid) override
        {
            return ctx.St && ctx.St->HearthTargetSet && ctx.World.HearthWorth(ctx.St->HearthTarget);
        }

        bool Possible(Ctx& ctx, Bid const&) override { return ctx.St != nullptr; }

        bool Execute(Ctx& ctx, Bid const& bid) override
        {
            if (!ctx.St)
                return false;
            // Откат в слоте выставляет само тело: минута, если ядро каст не начало, десять — если
            // начало. НАЧАТЫЙ КАСТ — «true»: движок фиксирует действие и выходит из такта, как
            // лестница выходила `return;` (`:3132`); иначе переочередённая дорога сделала бы шаг
            // тем же тактом и оборвала бы десятисекундное чтение. Следующий такт держит
            // страховка `HearthCastMs` (`:2690`). Не начатый — «false»: дорога продолжится пешком.
            bool const casting = ctx.World.HearthCast(ctx.St->HearthTarget, ctx.Act, ctx.St->Move);
            ctx.St->HearthTargetSet = false;
            return casting;
        }
    };

    // ПОЛЁТ. План уже в слоте (`PlanFlight`, спрошен предпосылкой дороги): к точке мастера, у точки
    // — тот же вид, что в плане; подойти; живой узел, маршрут, оплата, взлёт — общее тело.
    //
    // ЧЕГО ЗДЕСЬ НЕТ, НАЗВАНО: `ApproachPoint`/`FindReachableApproach` лестницы (`:3681-3685`) —
    // движок идёт к самому мастеру и судит дорогу `AdvanceWalk`, как к торговцу.
    class TakeFlightAction final : public Action
    {
    public:
        TakeFlightAction() : Action(ActionId::TakeFlight) { }

        bool Useful(Ctx& ctx, Bid const& bid) override
        {
            FlightPlan plan;
            return ctx.St && bid.About.What() == Subject::Kind::Species
                && ctx.World.FlightPlanned(&plan) && plan.MasterEntry == bid.About.Id();
        }

        bool Possible(Ctx& ctx, Bid const&) override { return ctx.St != nullptr; }

        bool Execute(Ctx& ctx, Bid const& bid) override
        {
            if (!ctx.St)
                return false;
            FlightPlan plan;
            if (!ctx.World.FlightPlanned(&plan) || plan.MasterEntry != bid.About.Id())
                return false;
            ctx.St->FlightRoadMs = ctx.NowMs;       // план исполняется — аренда живая
            float const dt = ctx.Act.SliceSeconds();
            uint32 const sliceMs = uint32(dt * 1000.0f);
            float const d = ctx.World.DistanceTo2d(plan.MasterWhere);

            if (d > MASTER_ARRIVED_YARDS)
            {
                bool const going = WalkTowards(ctx, plan.MasterWhere, MASTER_STOP_YARDS, dt);
                if (AdvanceWalk(ctx, bid.About, d, sliceMs, !going) != WalkVerdict::Going)
                {
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ПОЛЁТ {}: до полётного мастера {} не дойти — осталось {:.0f}",
                        ctx.World.Name(), plan.MasterEntry, d);
                    ctx.World.FlightAbort(FLIGHT_FAIL_MS);
                    return false;
                }
                return true;
            }
            // У ТОЧКИ: тот же вид в 30 ярдах (тело); нет — тело сняло план и поставило откат.
            std::optional<ObjectGuid> const master = ctx.World.FlightMasterAt();
            if (!master)
                return false;
            if (!ctx.World.CanInteractWithFlightMaster(*master))
            {
                std::optional<Position> const at = ctx.World.WhereIs(*master);
                std::optional<float> const dm = ctx.World.DistanceTo(*master);
                if (!at || !dm)
                {
                    ctx.World.FlightAbort(FLIGHT_FAIL_MS);
                    return false;
                }
                bool const going = WalkTowards(ctx, *at, MASTER_TALK_YARDS, dt);
                if (AdvanceWalk(ctx, Subject::OfUnit(*master), *dm, sliceMs, !going) != WalkVerdict::Going)
                {
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ПОЛЁТ {}: к полётному мастеру {} не подойти", ctx.World.Name(), plan.MasterEntry);
                    ctx.World.FlightAbort(FLIGHT_FAIL_MS);
                    return false;
                }
                return true;
            }
            // Тело печатает исход само; план снят им же на всех выходах. ВЗЛЕТЕЛИ — «true»:
            // движок выходит из такта (лестница: `return;` `:3751`), иначе переочередённая дорога
            // послала бы шаг уже в полёте. Дальше такт не доходит до движка (`IsInFlight`, `:2683`),
            // а после посадки дорога планируется заново. Не взлетели — «false», пешком.
            ctx.World.TakeFlight(*master, ctx.Act, ctx.St->Move);
            return ctx.World.IsInFlight();
        }
    };
}

namespace Constellation::Ai
{
    void RegisterAirActions(Engine& engine)
    {
        engine.Register(std::make_unique<UseHearthstoneAction>());
        engine.Register(std::make_unique<TakeFlightAction>());
    }

    // ПРЕДПОСЫЛКА ДОРОГИ — там же, где лестница спрашивала камень и полёт: перед тем как выйти
    // (`:3131-3137`, `:3320-3326`, `:3361-3364`). Только в начале дороги (до первого `AdvanceWalk`
    // к этой цели) — план стоит дорого (маршруты графа); ставший план держится в слоте и ставится,
    // пока не отработан. Сдача и поход спрашивают камень; поиск квестодателя — только полёт, как
    // у лестницы.
    // НАЧАЛО ДОРОГИ — там, где лестница спрашивала камень и полёт перед `Switch` в режим дороги
    // (`:3131-3137`, `:3320-3326`, `:3361-3364`): первый такт исполнения выбранной дороги
    // (`Running` ещё не она). Камень — читаем сразу и отдаём такт (страховка `:2690` держит спутника
    // на время чтения); полёт — план в слот, такт отдаём, следующим тактом стратегия ставит
    // `TakeFlight` выше боёв (REL_MOVE), как лестница уходила в `TakingFlight`. true = такт потрачен на воздух, дорога вернётся пешком или после посадки.
    bool AirAtRoadStart(Ctx& ctx, Bid const& bid, Subject const& road, Position const& target, bool hearth)
    {
        if (!ctx.St || !Tuning().Flying)
            return false;
        if (ctx.St->Running == bid.Action && ctx.St->RunningAbout == bid.About)
            return false;                           // дорога уже идёт — воздух спрошен при её начале
        if (ctx.World.FlightPlanned(nullptr))
        {
            if (ctx.St->FlightRoad == road)
                return false;                       // свой план — стратегия ставит полёт выше боёв
            if (ctx.NowMs - ctx.St->FlightRoadMs < FLIGHT_REPLAN_MS)
                return false;                       // чужой, но свежий — пешком, не перестраивать
            ctx.World.FlightAbort(0);               // чужой и старый — исполняется другая дорога
        }
        else if (ctx.St->FlightRoadMs && ctx.NowMs - ctx.St->FlightRoadMs < FLIGHT_REPLAN_MS)
            return false;                           // недавно планировали (и план ушёл) — не чаще минуты
        if (hearth && ctx.World.HearthWorth(target) && ctx.World.HearthCast(target, ctx.Act, ctx.St->Move))
            return true;                            // читаем камень — такт отдан
        ctx.St->FlightRoadMs = ctx.NowMs;           // метка ДО попытки: неудача тоже ждёт минуту
        FlightPlan plan;
        if (ctx.World.PlanFlight(target, &plan))
        {
            ctx.St->FlightRoad = road;
            return true;                            // план в слоте — следующим тактом полетим
        }
        return false;
    }
}
