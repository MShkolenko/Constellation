/*
 * Constellation — TalkToTarget: the objective that is closed by talking, clicking or using a
 * quest item, not by fighting.
 *
 * The ladder's `case Behavior::Talking` (Constellation.cpp:5056-5562) moved as ONE unit in three
 * lifts — the plan (TalkPlanCore), the arrival (TalkArrivedCore) and the engage with its outcome
 * handlers (TalkEngageCore) — so what is here is only what differs between the two mechanisms:
 * where the state lives, what the memory is, and how the walk is judged.
 *
 * Copyright (C) 2026 Constellation contributors. Licensed under the GNU AGPL v3 — see COPYING.
 */
#include "../Values.h"
#include "../Engine.h"
#include "../ClientAct.h"

#include <algorithm>
#include <memory>

namespace
{
    using namespace Constellation::Ai;

    // ЧИСЛА ЛЕСТНИЦЫ. Ближе — дешевле тем же наклоном, что у квестовых ставок.
    inline constexpr float YARD_COST = 0.01f;
    // «Не дойти» — тот же срок, что у боевой цели (Engine.cpp, UNREACHABLE_UNIT_MS): десять минут.
    inline constexpr uint32 TALK_UNREACHABLE_MS = 600000;

    // ПОГОВОРИТЬ, А НЕ ДРАТЬСЯ. Замер 2026-09-11: 5 из 135 расхождений в узле «стою» —
    // лестница «надо поговорить, а не драться», у движка действия не было.
    //
    // ЧЕГО ЗДЕСЬ НЕТ, НАЗВАНО:
    //   * `ApproachPoint`/`FindReachableApproach`/`ProgressDist` (`:5155-5196`) — подход лестницы
    //     со своей точкой подхода и повторным поиском дороги. Движок идёт к самому существу и
    //     судит дорогу `AdvanceWalk`; «дошёл» решает тот же поднятый вопрос, что и у лестницы;
    //   * `TravelCooldownMs`-подобной паузы нет; особь после «не дойти» — `Unreachable` на десять
    //     минут, как у боевой цели; после «закрыть нечем»/«без зачёта» тело ставит её на час
    //     (`TALK_INDIVIDUAL_MS`) — лестница держит `TalkUnreachable` до переполнения (>40);
    //   * `Cfg().Fight`/`BrokenForFight` — движок не смотрит на экипировку нигде.
    class TalkToTargetAction final : public Action
    {
    public:
        TalkToTargetAction() : Action(ActionId::TalkToTarget) { }

        // Особь, к которой «не дойти» или «закрыть нечем», — тот же ключ, что читает обход целей
        // (`FightBan::Unreachable`), поэтому предварительный фильтр и обход отвечают одно.
        BackoffKind DeferKind() const override { return BackoffKind::Unreachable; }

        bool Useful(Ctx& ctx, Bid const& bid) override
        {
            if (bid.About.What() != Subject::Kind::Unit)
                return false;
            ObjectiveScan const& scan = Val<ValueId::Objectives>(ctx);
            return scan.Talk == bid.About.Guid() && ctx.World.IsAliveUnit(scan.Talk);
        }

        bool Possible(Ctx& ctx, Bid const&) override
        {
            // Пауза всему действию после шести особей без зачёта — обход её уже учёл
            // (`toolBusy`), здесь второй замок на ту же дверь для ставки, что пережила пересчёт.
            return ctx.St && !ctx.St->TalkPaused(ctx.NowMs);
        }

        float Score(Ctx& ctx, Bid const& bid, float relevance) const override
        {
            if (bid.About.What() != Subject::Kind::Unit)
                return relevance;
            std::optional<float> const d = ctx.World.DistanceTo(bid.About.Guid());
            return d ? relevance - *d * YARD_COST : relevance;
        }

        bool Execute(Ctx& ctx, Bid const& bid) override
        {
            if (!ctx.St)
                return false;
            ObjectGuid const who = bid.About.Guid();
            TalkState& st = ctx.St->Talk;

            // ПЛАН — каждый такт, как и у лестницы (`:5078`): данные существа дёшевы, а флаг
            // клика снимается правилом в момент подъёма.
            TalkPlan plan;
            if (!ctx.World.TalkPlanOf(who, &plan))
            {
                if (plan.Why == TalkPlan::None)
                    return false;                       // собеседник исчез
                TalkRefuseThroughDoor(ctx, who, plan, st);
                return false;
            }

            bool const arrived = ctx.World.TalkArrivedAt(who, plan);
            if (!arrived && !st.WaitMs)
            {
                std::optional<Position> const at = ctx.World.WhereIs(who);
                std::optional<float> const d = ctx.World.DistanceTo(who);
                if (!at || !d)
                    return false;
                float const dt = ctx.Act.SliceSeconds();
                // Порог шага — как у лестницы (`:5166`): беседе её reach, клику и предмету на
                // ярд ближе, потому что «дошёл» там мерится точным расстоянием до самой цели.
                bool const gossip = plan.What == TalkPlan::Gossip;
                float const stop = gossip ? plan.Reach : std::max(1.0f, plan.Reach - 1.0f);
                bool const going = WalkTowards(ctx, *at, stop, dt);
                if (AdvanceWalk(ctx, bid.About, *d, uint32(dt * 1000.0f), !going) != WalkVerdict::Going)
                {
                    Defer(ctx, BackoffKind::Unreachable, bid.About, 0, TALK_UNREACHABLE_MS);
                    return false;
                }
                return true;
            }

            switch (TalkThroughDoor(ctx, who, plan, st))
            {
                case TalkOutcome::Waiting:
                case TalkOutcome::Sent:
                case TalkOutcome::Credited:
                case TalkOutcome::Talked:
                    return true;
                // Память по каждому из этих исходов уже записана телом (вид, особь, «позже»,
                // пауза); здесь только «не бегу за этим дальше».
                case TalkOutcome::Fruitless:
                case TalkOutcome::NotByConditions:
                case TalkOutcome::ToolNotReady:
                case TalkOutcome::NothingToSay:
                case TalkOutcome::TalkFailed:
                    return false;
            }
            return false;
        }

        // ВЫХОД ИЗ РАЗГОВОРА ОБНУЛЯЕТ ОКНО И СЧЁТЧИК БЕСПЛОДНЫХ — то, что у лестницы делает
        // `Switch` (`:7447`): иначе следующий разговор с другой целью начинался бы со старым
        // окном. Снимок `Was` остаётся: по нему ловится зачёт, пришедший после окна.
        void Cancel(Ctx& ctx, Subject const&, CancelReason) override
        {
            if (!ctx.St)
                return;
            ctx.St->Talk.WaitMs = 0;
            ctx.St->Talk.Fruitless = 0;
        }
    };
}

namespace Constellation::Ai
{
    void RegisterTalkActions(Engine& engine)
    {
        engine.Register(std::make_unique<TalkToTargetAction>());
    }
}
