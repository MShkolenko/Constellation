/*
 * Constellation — the first action of the engine that hits something, and the strategy that bids it.
 *
 * WHAT A PLAYER DOES, IN THE SAME ORDER AND WITH THE SAME PACKETS: walk until the core says the
 * target is in melee range, turn to face it, select it, swing. Every one of those four is a door
 * method, and the door is the only thing that writes.
 *
 * THE ORDER OF PRECEDENCE IS THE LADDER'S, READ RATHER THAN CHOSEN. Its `Idle` branch checks, in
 * this order: hand in, FIGHT (`Constellation.cpp:3154`), cage, talker, gather, travel to objective,
 * take a quest, walk to a giver by the map. So fighting sits above taking and seeking, and below
 * handing in.
 *
 * AND THERE IS A DECLARED DIFFERENCE HERE, NOT A QUIET ONE. A `switch` can only express absolute
 * precedence: whatever is checked first wins. A bid queue PRICES instead, and at equal relevance
 * distance decides. So this bids at REL_HIGH — beside the hand-in rather than under it — which
 * means a distant hand-in will lose to a nearby fight, something the ladder could not do. That is
 * the whole point of the queue, but it IS a change, and the shadow measures it rather than me
 * declaring it correct.
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

    // Тот же наклон, что у квестовых действий: ярд стоит немного, но стоит.
    inline constexpr float YARD_COST = 0.01f;

    // ДО ЦЕЛИ НЕ ДОБРАТЬСЯ — НА ДЕСЯТЬ МИНУТ. Столько же держит лестница у недостижимых
    // собеседников, и по той же причине: за это время меняется и уровень, и обстановка вокруг.
    inline constexpr uint32 COMBAT_UNREACHABLE_MS = 600000;

    // ОСТАНАВЛИВАЕМСЯ НЕ У САМОЙ ТОЧКИ. Ноль означал бы «встань в него», а достаточную близость
    // решает ядро своим `IsWithinMeleeRange`; этот порог — лишь то, на чём двигатель прекращает
    // шагать, и он взят с запасом внутрь боевого охвата.
    inline constexpr float MELEE_STOP_YARDS = 2.0f;

    class KillObjectiveAction final : public Action
    {
    public:
        KillObjectiveAction() : Action(ActionId::KillObjective) { }

        // §14 — ПОД КАКИМ ЗАПРЕТОМ ХОДИТ ЭТО ДЕЙСТВИЕ. Тот, который оно само и ставит, когда
        // дорога не вышла. Второй вид, «не собеседник и не дойти», ловится значением: обход
        // спрашивает оба и такую цель просто не назовёт.
        BackoffKind DeferKind() const override { return BackoffKind::CombatUnreachable; }

        bool Useful(Ctx& ctx, Bid const& bid) override
        {
            if (bid.About.What() != Subject::Kind::Unit)
                return false;
            ObjectGuid const victim = bid.About.Guid();
            if (victim.IsEmpty())
                return false;
            // ВСЁ ЕЩЁ ТА ЖЕ ЦЕЛЬ. Обход пересчитывается раз в секунду и вполне может выбрать
            // другую — ставка прошлой секунды тогда больше не нужна.
            return Val<ValueId::Objectives>(ctx).Fight == victim;
        }

        // ЖИВ ЛИ ОН ЕЩЁ И ВИДИМ ЛИ — вопрос к ядру, между выбором и исполнением проходит время.
        bool Possible(Ctx& ctx, Bid const& bid) override
        {
            return ctx.World.CanSee(bid.About.Guid());
        }

        float Score(Ctx& ctx, Bid const& bid, float relevance) const override
        {
            if (std::optional<float> const d = ctx.World.DistanceTo(bid.About.Guid()))
                return relevance - *d * YARD_COST;
            return relevance;
        }

        bool Execute(Ctx& ctx, Bid const& bid) override
        {
            ObjectGuid const victim = bid.About.Guid();
            if (victim.IsEmpty() || !ctx.St)
                return false;

            // ДОСТАЮ ЛИ — РЕШАЕТ ЯДРО. Оно считает охват с учётом размеров обоих тел, и своя
            // мерка здесь уже однажды стоила модулю 916 кругов и ноль сдач.
            if (!ctx.World.InMeleeRange(victim))
            {
                // ОДИН ВОПРОС, А НЕ ДВА. Раньше здесь спрашивалось расстояние, а потом
                // отдельно положение, и между двумя чтениями цель могла исчезнуть — тогда шаг
                // уходил в нулевые координаты (Кодекс). Положение отвечает и на «есть ли он».
                std::optional<Position> const where = ctx.World.WhereIs(victim);
                if (!where)
                    return false;               // цель исчезла между выбором и шагом
                std::optional<float> const d = ctx.World.DistanceTo(victim);
                if (!d)
                    return false;

                float const dt = ctx.Act.SliceSeconds();
                bool const going = WalkTowards(ctx, *where, MELEE_STOP_YARDS, dt);

                uint32 const sliceMs = uint32(dt * 1000.0f);
                if (AdvanceWalk(ctx, bid.About, *d, sliceMs, !going) != WalkVerdict::Going)
                {
                    Defer(ctx, BackoffKind::CombatUnreachable, bid.About, 0, COMBAT_UNREACHABLE_MS);
                    return false;
                }
                return true;
            }

            // ПРИШЛИ. Дальше — ровно то, что делает игрок мышью, и в том же порядке.
            //
            // ПОВОРОТ ПЕРВЫМ, И ЭТО НЕ ВЕЖЛИВОСТЬ: `Unit::UpdateMeleeAttackingState` требует
            // `HasInArc`, и без него ядро отвергнет каждый замах. Дверь возвращает истину и
            // тогда, когда поворачиваться уже не нужно.
            if (!ctx.Act.Face(victim))
                return false;
            if (!ctx.Act.SetSelection(victim))
                return false;
            return ctx.Act.AttackSwing(victim);
        }
    };

    // §9 — СТАВИТ КАЖДЫЙ ТАКТ, как и квестовая. «Есть кого бить» — состояние, а не происшествие.
    class CombatStrategy final : public Strategy
    {
    public:
        CombatStrategy() : Strategy(StrategyId::Combat) { }

        void DefaultBids(Ctx& ctx, BidSink& sink) const override
        {
            ObjectiveScan const& scan = Val<ValueId::Objectives>(ctx);
            if (scan.Fight.IsEmpty())
                return;
            // REL_HIGH — РЯДОМ СО СДАЧЕЙ, А НЕ ПОД НЕЙ. Лестница ставит бой выше взятия и похода
            // (`Constellation.cpp:3154` против `:3262`) и ниже сдачи; очередь выражает «выше»
            // ценой, и при равной значимости решает расстояние.
            //
            // ТОЧНО: конкурировать эта ставка будет со ВСЕМИ ставками своего уровня, а не с одной
            // дальней сдачей, как было написано в первой редакции. Сегодня на REL_HIGH стоит
            // сдача; завтра встанет что-то ещё, и порядок между ними будет решать цена, а не этот
            // комментарий. Мерит это тень.
            sink.Add(ActionId::KillObjective, REL_HIGH, Subject::OfUnit(scan.Fight));
        }
    };
}

namespace Constellation::Ai
{
    void RegisterFightActions(Engine& engine)
    {
        engine.Register(std::make_unique<KillObjectiveAction>());
        engine.Register(std::make_unique<CombatStrategy>());
    }
}
