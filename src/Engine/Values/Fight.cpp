/*
 * Constellation — the objective sweep as a cached value, and the first value that is not about quests.
 *
 * ONE EXPENSIVE PASS, THREE ANSWERS — AND THE ENGINE READS ONE OF THEM SO FAR. `ScanObjectives`
 * walks the grid once and reports what is worth fighting, whom to talk to instead, and which cage
 * would free an untouchable target. `KillObjective` reads the first; the talker and the cage are
 * computed and, for now, discarded on the next recompute, because the actions that would read them
 * are not written. Review called that out and it is worth saying plainly rather than letting the
 * heading imply three consumers exist.
 *
 * WHY IT IS A VALUE AND NOT A QUESTION AN ACTION ASKS: the module's own comment at
 * `Constellation.cpp:2695` says this sweep and the turn-in scan ran from the Idle branch at four
 * ticks a second across 114 companions, and the world thread went from 45 % to 84 % of a core when
 * they were called without a throttle. The interval below is the ladder's own, not a new number.
 *
 * Copyright (C) 2026 Constellation contributors. Licensed under the GNU AGPL v3 — see COPYING.
 */
#include "../Values.h"
#include "../Engine.h"

#include <memory>

namespace
{
    using namespace Constellation::Ai;

    // ИНТЕРВАЛ ВЗЯТ У ВЕТКИ, КОТОРУЮ ЭТО ЗАМЕНЯЕТ. `Idle` зовёт обход под `idleScan`
    // (`Constellation.cpp:3153`) — тем же дросселем, что и сканы квестов. Джиттер по гуиду
    // добавляет `Value::Get` сам, чтобы сто с лишним пересчётов не сошлись в один такт мирового
    // потока.
    //
    // ДВЕ ДРУГИЕ ВЕТКИ ЛЕСТНИЦЫ ЗОВУТ ЕГО БЕЗ ДРОССЕЛЯ (`:3324`, `:3799`), и это НЕ довод сделать
    // так же: там он зовётся в состояниях «ищу цель» и «подхожу», куда спутник попадает по
    // одному, а не сто двадцать два разом. У движка такого разделения нет — ставки считаются
    // каждому и каждый такт, — поэтому берётся дроссель, а не его отсутствие.
    inline constexpr uint32 FIGHT_SCAN_MS = 1000;

    class ObjectivesValue final : public Value<ObjectiveScan>
    {
    public:
        ObjectivesValue() : Value(ValueId::Objectives, FIGHT_SCAN_MS) { }

    protected:
        void Calculate(Ctx& ctx, ObjectiveScan& out) const override
        {
            // ЧИСТЫЙ ЛИСТ КАЖДЫЙ ПЕРЕСЧЁТ — И ЭТО РАЗНИЦА С ЛЕСТНИЦЕЙ, НАЗВАННАЯ ТОЧНО.
            //
            // У лестницы структура засевается из спутника, потому что отметка клетки СТОЯЩАЯ:
            // пока прошлая не потреблена, новую не ищут вовсе. Здесь её нет, и первая редакция
            // этого комментария утверждала, что ничего не теряется, — «ставка повторится».
            // Кодекс показал, что довод неполон: пока действие идёт к клетке, следующий пересчёт
            // вправе назвать ДРУГУЮ.
            //
            // У очереди на это свой ответ, и он не тот же самый: предмет ставки хранит выбранную
            // цель, а `Useful` сверяет его с текущим значением и роняет ставку, если значение
            // передумало. То есть лестница ДЕРЖИТСЯ за начатое, а движок переоценивает каждый
            // такт. Это ДРУГОЕ поведение, а не то же самое, и оно измеряется тенью — как и всё
            // остальное, чем очередь отличается от `switch`.
            out = ObjectiveScan();

            // БЕЗ СОСТОЯНИЯ НЕ СЧИТАЕМ ВОВСЕ. Пустая память отвечает «не запрещено» на всё, то
            // есть незащищённый обход выбрал бы то же самое столько раз, сколько его спросят.
            // Пустой ответ честнее незащищённого — то же правило, что у `GiverToSeek`.
            if (!ctx.St)
                return;

            FightMemory const mem = EngineFightMemory(ctx);
            ctx.World.Objectives(mem, ctx.Danger, &out);
        }
    };
}

namespace Constellation::Ai
{
    void RegisterFightValues(Engine& engine)
    {
        engine.RegisterValue<ValueId::Objectives>(std::make_unique<ObjectivesValue>());
    }
}
