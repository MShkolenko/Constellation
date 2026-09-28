/*
 * Constellation — FollowOwner: walk behind the player who owns this companion. The last ladder
 * behaviour ported (2026-09-14, port step two); the body is the ladder's FollowingOwner
 * (`Constellation.cpp:3452-3492`) over the engine's walk.
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

    // ЧИСЛА ЛЕСТНИЦЫ: не дойти / полминуты без движения — десять секунд не возвращаться
    // (`FollowCooldownMs = 10000`, `:3488`); полминуты — у `AdvanceWalk` свои (WALK_NO_PROGRESS_MS).
    inline constexpr uint32 FOLLOW_RETRY_MS = 10000;
    inline constexpr uint8  FOLLOW_DETAIL   = 2;    // ключ отсрочки при пустом предмете (у отдыха — 1)
    // ЗА ЛИДЕРОМ ОТРЯДА: отстал дальше сорока - догоняем, до пятнадцати; дальше двухсот пятидесяти
    // не идём - там отряд распадается своим правилом.
    inline constexpr float SQUAD_KEEP_FAR_YARDS  = 40.0f;
    inline constexpr float SQUAD_KEEP_STOP_YARDS = 15.0f;
    inline constexpr float SQUAD_KEEP_MAX_YARDS  = 250.0f;

    class FollowOwnerAction final : public Action
    {
    public:
        FollowOwnerAction() : Action(ActionId::FollowOwner) { }

        BackoffKind DeferKind() const override { return BackoffKind::Visited; }
        uint8 DeferDetail(Bid const&) const override { return FOLLOW_DETAIL; }

        bool Useful(Ctx& ctx, Bid const& bid) override
        {
            // ЗА ЛИДЕРОМ ОТРЯДА (кандидат 1): своя дальность - догоняем, когда отстали дальше сорока, до
            // двухсот пятидесяти (дальше отряд и так распадётся), останавливаемся в пятнадцати.
            if (bid.About.What() == Subject::Kind::Unit)
            {
                if (!ctx.St || !Tuning().Squads || ctx.World.SquadLeader() != bid.About.Guid())
                    return false;
                std::optional<Position> const lead = ctx.World.WhereIs(bid.About.Guid());
                if (!lead)
                    return false;
                float const d = ctx.World.DistanceTo2d(*lead);
                return d > SQUAD_KEEP_STOP_YARDS && d <= SQUAD_KEEP_MAX_YARDS;
            }
            Position owner;
            if (!ctx.St || !Tuning().Follow || !ctx.World.FollowTarget(&owner))
                return false;
            float const d = ctx.World.DistanceTo2d(owner);
            return d <= Tuning().FollowMaxRange && d > Tuning().FollowDistance;
        }

        bool Possible(Ctx& ctx, Bid const&) override { return ctx.St != nullptr; }

        bool Execute(Ctx& ctx, Bid const& bid) override
        {
            bool const squad = bid.About.What() == Subject::Kind::Unit;
            Position owner;
            float stopAt = Tuning().FollowDistance;
            float maxRange = Tuning().FollowMaxRange;
            if (squad)
            {
                std::optional<Position> const lead = ctx.St ? ctx.World.WhereIs(bid.About.Guid()) : std::nullopt;
                if (!lead)
                    return false;
                owner = *lead;
                stopAt = SQUAD_KEEP_STOP_YARDS;
                maxRange = SQUAD_KEEP_MAX_YARDS;
            }
            else if (!ctx.St || !ctx.World.FollowTarget(&owner))
                return false;                       // «идти не за кем» (`:3465`)
            float const d = ctx.World.DistanceTo2d(owner);
            if (d > maxRange)
                return false;                       // «хозяин слишком далеко» (`:3470`)
            float const dt = ctx.Act.SliceSeconds();
            bool const going = WalkTowards(ctx, owner, stopAt, dt);
            uint32 const sliceMs = uint32(dt * 1000.0f);
            bool const stalled = !going && d > stopAt && ctx.St->Move.Stalled;
            if (AdvanceWalk(ctx, bid.About, d, sliceMs, stalled) != WalkVerdict::Going)
            {
                Defer(ctx, BackoffKind::Visited, bid.About, FOLLOW_DETAIL, FOLLOW_RETRY_MS);
                return false;                       // «до хозяина не дойти» / «полминуты без движения»
            }
            return true;
        }
    };
}

namespace Constellation::Ai
{
    // СЛЕДОВАНИЕ — СВОЯ СТРАТЕГИЯ (§3.4; П1 2026-09-15). Ставка жила в `Survival`, и бит 32 маски
    // ничего не включал. Место и цена те же: последним, когда больше нечего делать (последняя
    // ветка Idle лестницы, `:3390-3396`) — REL_BACKGROUND, вне боя; далеко — не ставится (`Useful`).
    class FollowStrategy final : public Strategy
    {
    public:
        FollowStrategy() : Strategy(StrategyId::Follow) { }

        void DefaultBids(Ctx& ctx, BidSink& sink) const override
        {
            if (!ctx.St || !Tuning().Follow || ctx.World.IsInCombat())
                return;
            Position owner;
            if (ctx.World.FollowTarget(&owner))
                sink.Add(ActionId::FollowOwner, REL_BACKGROUND, Subject());
            // УЧАСТНИК ДЕРЖИТСЯ ЛИДЕРА И ВО ВРЕМЯ ЕГО СВОИХ ДЕЛ (оператор 2026-09-28, кандидат 1): 14 из 15
            // выходов из отрядов окна 10:28 - «две минуты вдали от лидера». Отстал дальше сорока - к
            // лидеру ценой REL_NORMAL: выше похода за своей целью, ниже сдачи и боя.
            else if (Tuning().Squads)
                if (ObjectGuid const lead = ctx.World.SquadLeader(); !lead.IsEmpty())
                    if (std::optional<Position> const at = ctx.World.WhereIs(lead))
                        if (ctx.World.DistanceTo2d(*at) > SQUAD_KEEP_FAR_YARDS)
                            sink.Add(ActionId::FollowOwner, REL_NORMAL, Subject::OfUnit(lead));
        }
    };
}

namespace Constellation::Ai
{
    void RegisterFollowActions(Engine& engine)
    {
        engine.Register(std::make_unique<FollowOwnerAction>());
        engine.Register(std::make_unique<FollowStrategy>());
    }
}
