/*
 * Constellation — VisitVendor: repair and sell, the first action ported after the engine became
 * the master (2026-09-13). The bodies are the ladder's (VendorNeedCore, TradeAtCore); this file is
 * only what differs between the mechanisms — where the state lives, what the memory is, how the
 * walk is judged.
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

    inline constexpr float  YARD_COST = 0.01f;
    // ЧИСЛА ЛЕСТНИЦЫ (`Vending`, `:3512-3653`): в обзоре за 60 ярдов от точки ищем вид в 40;
    // «пришли на точку» — 6 ярдов; после визита — минута с разбросом по спутнику; не дошли
    // или на точке никого — пять минут.
    inline constexpr float  MAP_LOOK_YARDS    = 60.0f;
    inline constexpr float  MAP_SCAN_YARDS    = 40.0f;
    inline constexpr float  MAP_ARRIVED_YARDS = 6.0f;
    inline constexpr float  MAP_STOP_YARDS    = 5.0f;
    inline constexpr uint32 MAP_SCAN_EVERY_MS = 2000;
    inline constexpr uint32 AFTER_TRADE_MS    = 60000;
    inline constexpr uint32 AFTER_FAIL_MS     = 300000;

    // ПОХОД К ТОРГОВЦУ. Ставится, когда есть повод (сломан, сумки полны, хлам, изношен и торговец
    // рядом) и адресат — в обзоре или по карте. Прилавок — общее тело `TradeAtCore`.
    //
    // ЧЕГО ЗДЕСЬ НЕТ, НАЗВАНО: `ApproachPoint`/`FindReachableApproach` лестницы — движок идёт к
    // самому торговцу и судит дорогу `AdvanceWalk`; «можно ли торговать» решает тот же вопрос
    // ядра, что у обработчика прилавка (`CanInteractWithNpc`).
    class VisitVendorAction final : public Action
    {
    public:
        VisitVendorAction() : Action(ActionId::VisitVendor) { }

        BackoffKind DeferKind() const override { return BackoffKind::VendorPause; }

        bool Useful(Ctx& ctx, Bid const& bid) override
        {
            VendorNeed const& v = Val<ValueId::VendorTrip>(ctx);
            if (v.Reason == VendorNeed::None)
                return false;
            if (bid.About.What() == Subject::Kind::Unit)
                return !v.ByMap && v.Near == bid.About.Guid();
            if (bid.About.What() == Subject::Kind::Species)
                return v.ByMap && v.MapEntry == bid.About.Id();
            return false;
        }

        bool Possible(Ctx& ctx, Bid const&) override { return ctx.St != nullptr; }

        float Score(Ctx& ctx, Bid const& bid, float relevance) const override
        {
            VendorNeed const& v = Val<ValueId::VendorTrip>(ctx);
            if (bid.About.What() == Subject::Kind::Unit)
            {
                std::optional<float> const d = ctx.World.DistanceTo(bid.About.Guid());
                return d ? relevance - *d * YARD_COST : relevance;
            }
            return relevance - ctx.World.DistanceTo2d(v.MapWhere) * YARD_COST;
        }

        bool Execute(Ctx& ctx, Bid const& bid) override
        {
            if (!ctx.St)
                return false;
            VendorNeed const& v = Val<ValueId::VendorTrip>(ctx);
            if (v.Reason == VendorNeed::None)
                return false;
            float const dt = ctx.Act.SliceSeconds();
            uint32 const sliceMs = uint32(dt * 1000.0f);

            // ---- ПО КАРТЕ: к точке, у точки — в обзор ----------------------------------------
            if (bid.About.What() == Subject::Kind::Species)
            {
                float const d = ctx.World.DistanceTo2d(v.MapWhere);
                // ОБЗОР У ТОЧКИ — РАЗ В ДВЕ СЕКУНДЫ, как лестница (`:3514-3519`): обход сетки на
                // каждом такте — лишнее (Кодекс).
                if (d < MAP_LOOK_YARDS && ctx.NowMs - ctx.St->VendorMapScanAtMs >= MAP_SCAN_EVERY_MS)
                {
                    ctx.St->VendorMapScanAtMs = ctx.NowMs;
                    if (std::optional<ObjectGuid> const seen = ctx.World.NearestCreatureOfEntry(bid.About.Id(), MAP_SCAN_YARDS))
                        return Trade(ctx, *seen, bid, dt, sliceMs);   // нашёлся — дальше как в обзоре
                }
                if (d <= MAP_ARRIVED_YARDS)
                {
                    // пришли на точку, а его нет: не та фаза, не возродился, ушёл маршрутом
                    Defer(ctx, BackoffKind::VendorPause, Subject(), 0, AFTER_FAIL_MS);
                    ctx.St->Values.VendorTrip.Invalidate();
                    return false;
                }
                bool const going = WalkTowards(ctx, v.MapWhere, MAP_STOP_YARDS, dt);
                if (AdvanceWalk(ctx, bid.About, d, sliceMs, !going) != WalkVerdict::Going)
                {
                    Defer(ctx, BackoffKind::VendorPause, Subject(), 0, AFTER_FAIL_MS);
                    ctx.St->Values.VendorTrip.Invalidate();
                    return false;
                }
                return true;
            }
            return Trade(ctx, bid.About.Guid(), bid, dt, sliceMs);
        }

    private:
        bool Trade(Ctx& ctx, ObjectGuid vendor, Bid const& bid, float dt, uint32 sliceMs)
        {
            if (!ctx.World.IsAliveUnit(vendor))
            {
                ctx.St->Values.VendorTrip.Invalidate();         // торговец пропал — искать заново, не перебирать ставку
                return false;
            }
            if (!ctx.World.CanInteractWithNpc(vendor))
            {
                std::optional<Position> const at = ctx.World.WhereIs(vendor);
                std::optional<float> const d = ctx.World.DistanceTo(vendor);
                if (!at || !d)
                {
                    ctx.St->Values.VendorTrip.Invalidate();
                    return false;
                }
                bool const going = WalkTowards(ctx, *at, MAP_STOP_YARDS, dt);
                if (AdvanceWalk(ctx, Subject::OfUnit(vendor), *d, sliceMs, !going) != WalkVerdict::Going)
                {
                    // не дошёл до торговца — пять минут, как лестница (`:3597`)
                    Defer(ctx, BackoffKind::VendorPause, Subject(), 0, AFTER_FAIL_MS);
                    ctx.St->Values.VendorTrip.Invalidate();
                    return false;
                }
                return true;
            }
            // ДОШЛИ. Прилавок — общее тело; потом минута с разбросом, чтобы не ходить строем.
            TradeThroughDoor(ctx, vendor);
            Defer(ctx, BackoffKind::VendorPause, Subject(), 0,
                  AFTER_TRADE_MS + (ctx.World.Guid().GetCounter() % 47) * 1000);
            ctx.St->Values.VendorTrip.Invalidate();
            return true;
        }
    };
}

namespace Constellation::Ai
{
    void RegisterVendorActions(Engine& engine)
    {
        engine.Register(std::make_unique<VisitVendorAction>());
    }
}
