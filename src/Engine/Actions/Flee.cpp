/*
 * Constellation — FleeCombat and EndEmptyCombat: in combat with broken gear, walk away from the
 * nearest attacker; in combat with nobody attacking, drop the stale combat flag. The third item
 * ported after the engine became the master (2026-09-13). The bodies are the ladder's Idle
 * (`Constellation.cpp:2998-3132`): the fan of eight retreat points is `FleePointCore`; the
 * timers, the pause and the walk are here, over the engine's own state.
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

    // ЧИСЛА ЛЕСТНИЦЫ (`:3058-3122`): точка меняется через 30 с, минута на одну точку, полторы
    // минуты на весь отход — и пять минут покоя после любого провала.
    inline constexpr uint32 FLEE_REPICK_MS = 30000;
    inline constexpr uint32 FLEE_POINT_MS  = 60000;
    inline constexpr uint32 FLEE_TOTAL_MS  = 90000;
    inline constexpr uint32 FLEE_PAUSE_MS  = 300000;

    inline bool FleePaused(Ctx& ctx)
    {
        BackoffKey k;
        k.Kind = BackoffKind::FleePause; k.About = Subject();
        return ctx.St && Engine::Deferred(*ctx.St, k, ctx.NowMs);
    }

    // ОТХОД: В БОЮ, А ДРАТЬСЯ НЕЧЕМ — ОТХОДИМ, А НЕ СТОИМ (`:2998`). Под живым движком этого не
    // было вовсе: движок «на экипировку не смотрел нигде», сломанный дрался, а Idle лестницы с
    // отходом не достигался. Точка отхода — общее тело (`FleePointCore`): 45 ярдов от ближайшего
    // нападающего веером из восьми углов, без обрывов, воды и толпы в 15 ярдах.
    class FleeCombatAction final : public Action
    {
    public:
        FleeCombatAction() : Action(ActionId::FleeCombat) { }

        BackoffKind DeferKind() const override { return BackoffKind::FleePause; }

        bool Useful(Ctx& ctx, Bid const&) override
        {
            return ctx.St && ctx.World.IsInCombat() && ctx.World.BrokenGear() > 0 && !FleePaused(ctx);
        }

        bool Possible(Ctx& ctx, Bid const&) override { return ctx.St != nullptr; }

        bool Execute(Ctx& ctx, Bid const&) override
        {
            if (!ctx.St)
                return false;
            EngineState::FleeState& f = ctx.St->Flee;
            std::optional<ObjectGuid> const nearest = ctx.World.NearestAttacker();
            if (!nearest)
                return false;                       // это случай EndEmptyCombat, не наш
            float const dt = ctx.Act.SliceSeconds();
            uint32 const sliceMs = uint32(dt * 1000.0f);

            f.Ms += sliceMs;
            if (!f.Noted)
            {
                f.Noted = true;
                TC_LOG_INFO("server.worldserver",
                    "Constellation ОТХОД {}: в бою с {} ({}), драться нечем — отхожу",
                    ctx.World.Name(), ctx.World.NameOf(*nearest), ctx.World.EntryOf(*nearest));
            }
            // ОБЩИЙ БЮДЖЕТ ОТХОДА: смена точки его не обнуляет (`:3057-3066`).
            f.TotalMs += sliceMs;
            if (f.TotalMs >= FLEE_TOTAL_MS)
            {
                TC_LOG_INFO("server.worldserver",
                    "Constellation ОТХОД {}: полторы минуты не отрываюсь — жду пять минут", ctx.World.Name());
                return Pause(ctx, f);
            }
            if (!f.HasPoint || f.Ms >= FLEE_REPICK_MS)
            {
                f.Ms = f.HasPoint ? 0 : f.Ms;       // сменили точку — время заново (`:3070`)
                f.HasPoint = ctx.World.FleePointFrom(*nearest, &f.To);
                if (!f.HasPoint)
                {
                    TC_LOG_INFO("server.worldserver",
                        "Constellation ОТХОД {}: некуда отойти — жду пять минут", ctx.World.Name());
                    return Pause(ctx, f);
                }
            }
            // НАСТОЯЩИЙ ПРЕДЕЛ (`:3116-3123`): ТУПИК или минута на точку — пять минут покоя.
            // Именно `Move.Stalled`, как у лестницы, а не «шаг не вышел»: `StepTowardCore` отвечает
            // false и по приходу к точке (`:1386`, `:1481`) — удачный отход не повод для паузы.
            WalkTowards(ctx, f.To, 0.0f, dt);
            if (ctx.St->Move.Stalled || f.Ms >= FLEE_POINT_MS)
                return Pause(ctx, f);
            return true;
        }

        // Вышли из боя или ставку отняли: как `:3128-3131` — счётчики заново.
        void Cancel(Ctx& ctx, Subject const&, CancelReason) override
        {
            if (ctx.St)
                ctx.St->Flee = EngineState::FleeState();
        }

    private:
        static bool Pause(Ctx& ctx, EngineState::FleeState& f)
        {
            Defer(ctx, BackoffKind::FleePause, Subject(), 0, FLEE_PAUSE_MS);
            f = EngineState::FleeState();
            return false;
        }
    };

    // В БОЮ БЕЗ НАПАДАЮЩИХ — СБРАСЫВАЕМ (`:3020-3038`): флаг боя завис, а бить некого. Это
    // единственная запись в мир без клиентского пакета во всём модуле (заголовок `ClientAct.h`);
    // она переехала в дверь как была, не стала второй.
    class EndEmptyCombatAction final : public Action
    {
    public:
        EndEmptyCombatAction() : Action(ActionId::EndEmptyCombat) { }

        bool Useful(Ctx& ctx, Bid const&) override
        {
            return ctx.St && ctx.World.IsInCombat() && ctx.World.BrokenGear() > 0
                && !FleePaused(ctx) && !ctx.World.NearestAttacker();
        }

        bool Possible(Ctx& ctx, Bid const&) override { return ctx.St != nullptr; }

        bool Execute(Ctx& ctx, Bid const&) override
        {
            if (!ctx.St)
                return false;
            EngineState::FleeState& f = ctx.St->Flee;
            TC_LOG_INFO("server.worldserver",
                "Constellation БОЙ-ПУСТОЙ {}: в бою без нападающих — сбрасываю (бегство {} мс, всего {} мс, точка {})",
                ctx.World.Name(), f.Ms, f.TotalMs, f.HasPoint ? 1 : 0);
            ctx.Act.CombatStop();
            f.Ms = 0;
            f.HasPoint = false;
            return false;                           // сделано за такт — ставка снята
        }
    };
}

namespace Constellation::Ai
{
    void RegisterFleeActions(Engine& engine)
    {
        engine.Register(std::make_unique<FleeCombatAction>());
        engine.Register(std::make_unique<EndEmptyCombatAction>());
    }
}
