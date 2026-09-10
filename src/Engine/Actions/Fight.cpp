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

    // ДО КЛЕТКИ ИДЁМ ВПЛОТНУЮ, а не до порога обзора: у объекта нет ни ног, ни маршрута — он
    // стоит там, где стоит, и «где-то рядом» тут не нужно. Разрешение всё равно даёт ядро, а
    // этот порог лишь говорит двигателю, когда перестать шагать.
    inline constexpr float CAGE_ARRIVED_YARDS = 2.0f;

    // И тот же десятиминутный срок, что у похода к квестодателю, к принимающему и к боевой цели:
    // случай один — «туда не дойти сейчас».
    inline constexpr uint32 CAGE_UNREACHABLE_MS = 600000;

    // ЧЕМ ОТЛИЧАЕТСЯ КЛЮЧ ОТСРОЧКИ ОТДЫХА. Предмет у него пуст — отдых про себя, — поэтому вид и
    // предмет одни и те же у любого такого запрета, и различает их деталь.
    inline constexpr uint8 REST_DETAIL = 1;

    // РАЗРЫВ, ПОСЛЕ КОТОРОГО ОТДЫХ СЧИТАЕТСЯ НАЧАТЫМ ЗАНОВО. То же число и та же причина, что у
    // счёта простоя: секунда — граница, за которой модуль уже объявил измеренный срез такта
    // недостоверным.
    inline constexpr uint32 REST_GAP_MS = 1000;

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

                // «ДОШЁЛ» — НЕ «ЗАСТРЯЛ», И ЭТО НЕ ПРИДИРКА К СЛОВУ. Двигатель отвечает
                // ложью на «дошли ИЛИ не можем», а приход здесь решает ядро (`InMeleeRange`),
                // и между его меркой и порогом остановки есть зазор. Передавая `!going` как
                // застревание, действие в такт прихода само ставило себе десятиминутный запрет
                // и второго такта, в котором ударило бы, уже не получало (разбор).
                uint32 const sliceMs = uint32(dt * 1000.0f);
                bool const stalled = !going && *d > MELEE_STOP_YARDS;
                if (AdvanceWalk(ctx, bid.About, *d, sliceMs, stalled) != WalkVerdict::Going)
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

    // ОТКРЫТЬ КЛЕТКУ, ЧТОБЫ ОСВОБОДИТЬ ЦЕЛЬ, КОТОРУЮ НЕЛЬЗЯ ТРОНУТЬ.
    //
    // Второй потребитель того же обхода: он уже считает клетку, а читал её до сих пор никто —
    // это признано долгом в заголовке значения, и вот его половина.
    //
    // ПОРЯДОК РАБОТ — ЛЕСТНИЦЫ, слово в слово (`Constellation.cpp:4404-4406`): пока далеко —
    // идём по счислению, у самой точки берём ЖИВОЙ объект по идентификатору спавна прямым
    // обращением к карте и спрашиваем разрешение у ядра. Расстояние взаимодействия не
    // выдумывается ни здесь, ни там.
    class OpenCageForTargetAction final : public Action
    {
    public:
        OpenCageForTargetAction() : Action(ActionId::OpenCageForTarget) { }

        // §14 — «ДО ЭТОЙ ТОЧКИ НЕ ДОБРАТЬСЯ». Отсрочка «этой клеткой ради этого пленника
        // пробовали» — другой вопрос и другой ключ (пара видов), и его задаёт обход, а не это
        // действие: он такую клетку просто не назовёт.
        BackoffKind DeferKind() const override { return BackoffKind::ObjectUnreachable; }

        bool Useful(Ctx& ctx, Bid const& bid) override
        {
            if (bid.About.What() != Subject::Kind::Spawn)
                return false;
            ObjectiveScan const& scan = Val<ValueId::Objectives>(ctx);
            return scan.CageSpawn && uint32(scan.CageSpawn) == bid.About.Id();
        }

        bool Possible(Ctx& ctx, Bid const&) override { return ctx.St != nullptr; }

        float Score(Ctx& ctx, Bid const& bid, float relevance) const override
        {
            ObjectiveScan const& scan = Val<ValueId::Objectives>(ctx);
            if (!scan.CageSpawn || uint32(scan.CageSpawn) != bid.About.Id())
                return relevance;
            return relevance - ctx.World.DistanceTo2d(scan.CagePos) * YARD_COST;
        }

        bool Execute(Ctx& ctx, Bid const& bid) override
        {
            if (!ctx.St)
                return false;
            ObjectiveScan const& scan = Val<ValueId::Objectives>(ctx);
            if (!scan.CageSpawn || uint32(scan.CageSpawn) != bid.About.Id())
                return false;

            // ЯДРО РЕШАЕТ, ДОСТАТОЧНО ЛИ БЛИЗКО, И ОНО ЖЕ — ПОЯВИЛСЯ ЛИ ОБЪЕКТ ВООБЩЕ.
            if (std::optional<ObjectGuid> const go = ctx.World.UsableObjectAt(scan.CageSpawn))
                return ctx.Act.UseGameObject(*go);

            float const dt = ctx.Act.SliceSeconds();
            bool const going = WalkTowards(ctx, scan.CagePos, CAGE_ARRIVED_YARDS, dt);

            float const d = ctx.World.DistanceTo2d(scan.CagePos);
            // «Дошёл» — не «застрял»: то же, что у боя. Приход к объекту решает ядро
            // (`UsableObjectAt`), а ходьба останавливается по расстоянию.
            uint32 const sliceMs = uint32(dt * 1000.0f);
            bool const stalled = !going && d > CAGE_ARRIVED_YARDS;
            if (AdvanceWalk(ctx, bid.About, d, sliceMs, stalled) != WalkVerdict::Going)
            {
                Defer(ctx, BackoffKind::ObjectUnreachable, bid.About, 0, CAGE_UNREACHABLE_MS);
                return false;
            }
            return true;
        }
    };

    // ПЕРЕВЕСТИ ДУХ. Первое действие движка, которое НИЧЕГО НЕ ШЛЁТ, и в этом вся его работа.
    //
    // Ценность его не в пакете, а в том, что он ЗАНИМАЕТ спутника: пока выбран он, не выбран бой.
    // Лестница в своей ветке шлёт остановку и ждёт; у движка остановка выходит сама — шаги
    // перестают отправляться, потому что ходьба живёт только внутри исполняемого действия.
    //
    // ПОТОЛОК — НАСТОЯЩИЙ, И ЭТО ПЕРЕЕЗД ЧУЖОГО РАЗБОРА, А НЕ МОЯ ОСТОРОЖНОСТЬ. Первая редакция
    // ветки просто выходила по сроку — а `NeedsRest` всё ещё истинно, и следующий такт возвращал
    // в отдых: вечный цикл, в котором спутник с застрявшим восстановлением стоит столбом. Выход
    // — ЗАПРЕТИТЬ отдых на вдвое больший срок и идти драться раненым: плохо, но из этого есть
    // выход, в отличие от стояния.
    class RestAction final : public Action
    {
    public:
        RestAction() : Action(ActionId::Rest) { }

        // §14 — «СХОДИЛ И ХВАТИТ»: вид тот же, что у похода, и случай он описывает точно.
        //
        // ПРЕДМЕТ ПУСТ, ПОТОМУ ЧТО ОТДЫХ ПРО СЕБЯ, А НЕ ПРО КОГО-ТО, — И ИМЕННО ПОЭТОМУ НУЖНА
        // ДЕТАЛЬ. Пустой предмет с этим видом совпал бы с любой другой отсрочкой такой же формы,
        // а деталь для того и заведена: «то, чем ключи различаются» (разбор). Она проставляется
        // и здесь, и при постановке — разойтись им негде, обе спрашивают одно и то же действие.
        BackoffKind DeferKind() const override { return BackoffKind::Visited; }
        uint8 DeferDetail(Bid const&) const override { return REST_DETAIL; }

        // ДВА ПОРОГА — ЗНАЧИТ ДВА УСЛОВИЯ, А НЕ ОДНО. Уходят отдыхать ниже одного, возвращаются
        // выше другого; у лестницы это выражено РЕЖИМОМ, который держится до `RestedEnough`.
        //
        // Первая редакция спрашивала здесь только `NeedsRest`, то есть бросала отдых на первом
        // же пороге — и в полосе между порогами движок предлагал драться, пока лестница ещё
        // отдыхала. Прибор показал это двумя строками через четыре минуты после выкладки. Я
        // перенёс оба числа и не перенёс то, ради чего они разные.
        //
        // ОПОРА — РЕШЕНИЕ, А НЕ ИСПОЛНЕНИЕ, и это вторая правка того же места.
        //
        // Сначала здесь стоял `RestingMs`, который растёт в `Execute`. Но ТЕНЬ `Execute` НЕ
        // ВЫЗЫВАЕТ — в этом её устройство, — значит в тени движок не бывает «уже отдыхающим»,
        // полоса между порогами для неё не существует, и верность отдыха ею не измерить. Замер
        // это и показал: походы при отдыхающей лестнице остались.
        //
        // `st.Running` ставится ПРИ ВЫБОРЕ и потому есть у обоих проходов. И это ближе к
        // оригиналу: у лестницы режим — решение («ушёл отдыхать и остаюсь»), а не факт сидения.
        bool Useful(Ctx& ctx, Bid const&) override
        {
            if (ctx.World.NeedsRest())
                return true;
            return ctx.St && ctx.St->Running == ActionId::Rest && !ctx.World.RestedEnough();
        }

        bool Possible(Ctx& ctx, Bid const&) override { return ctx.St != nullptr; }

        bool Execute(Ctx& ctx, Bid const& bid) override
        {
            if (!ctx.St)
                return false;

            // НАПАЛИ — НЕ ДО ОТДЫХА. Первая проверка ветки лестницы, и здесь она первая же.
            if (ctx.World.IsInCombat())
                return false;

            // ВСТАТЬ — ЭТО ДЕЙСТВИЕ, А НЕ ОТСУТСТВИЕ ДЕЙСТВИЯ. Перестать слать шаги не значит
            // остановиться: сервер ведёт туда, куда его отправили в последний раз. Лестница шлёт
            // остановку первым делом, и здесь она тоже первая (разбор поймал моё утверждение,
            // будто ходьба живёт только внутри исполняемого действия, — это свойство моего кода,
            // а не сервера).
            ctx.Act.StopMoving();

            // РАЗРЫВ В НАБЛЮДЕНИИ — НЕ ПРОДОЛЖЕНИЕ ОТДЫХА. Если отдых перестали выбирать, счёт
            // начинается заново: иначе через минуту чужой работы он упрётся в потолок раньше
            // срока. Порог разрыва — тот же, что у счёта простоя.
            if (ctx.NowMs - ctx.St->RestedAtMs > REST_GAP_MS)
                ctx.St->RestingMs = 0;
            ctx.St->RestedAtMs = ctx.NowMs;

            if (ctx.World.RestedEnough())
            {
                ctx.St->RestingMs = 0;
                return true;            // отдышался; в следующем такте `Useful` уже не позовёт
            }

            ctx.St->RestingMs += uint32(ctx.Act.SliceSeconds() * 1000.0f);
            uint32 const cap = Tuning().RestMaxMs;
            // `>=`, А НЕ `>`: «после RestMaxMs» — это не «строго больше» (разбор).
            if (cap && ctx.St->RestingMs >= cap)
            {
                ctx.St->RestingMs = 0;
                // УДВОЕНИЕ НЕ ПЕРЕПОЛНЯЕТСЯ: срок приходит из настройки, и умножение на два
                // вправе уйти за край. Насыщение вместо переполнения — иначе запрет вышел бы
                // короче самого отдыха.
                uint32 const ban = cap > (0xFFFFFFFFu / 2u) ? 0xFFFFFFFFu : cap * 2u;
                Defer(ctx, BackoffKind::Visited, bid.About, REST_DETAIL, ban);
                return false;           // восстановление не идёт — иду как есть
            }
            return true;
        }

        // ОТДЫХ НЕ ОТМЕНЯЕТСЯ РАССТОЯНИЕМ: он про себя, и идти до себя не надо. Отсюда его цена
        // ровно `REL_HIGH`, а у сдачи — `REL_HIGH` минус ярды.
        //
        // ЗНАЧИТ ОТДЫХ ВЫИГРЫВАЕТ У СДАЧИ НА ЛЮБОМ НЕНУЛЕВОМ РАССТОЯНИИ, и первая редакция этого
        // комментария утверждала ОБРАТНОЕ — «близкая сдача выигрывает». Перепутанное направление
        // хуже преувеличения: оно читается как объяснение и объясняет несуществующее (разбор).
        //
        // ПОВЕДЕНИЕ ВЫБРАНО СОЗНАТЕЛЬНО И ОГРАНИЧЕНО С ДВУХ СТОРОН: отдых перестаёт ставиться,
        // как только `RestedEnough` (порог возврата ВЫШЕ порога ухода), и запрещается вовсе после
        // `RestMaxMs`. Бесконечно откладывать сдачу он не может. Раненый теперь сперва
        // отдышится, а потом понесёт — и это лучше лестницы, которая шла сдавать раненой на
        // любое расстояние.
    };

    // ВЫЖИВАНИЕ — СВОЯ СТРАТЕГИЯ, А НЕ УГОЛОК БОЕВОЙ. `Survival` объявлена в списке ровно под
    // это, и разделение не косметическое: маска включает стратегии ПОРОЗНЬ, а «выключить бой,
    // оставить отдых» под общей крышей перестало бы работать.
    class SurvivalStrategy final : public Strategy
    {
    public:
        SurvivalStrategy() : Strategy(StrategyId::Survival) { }

        void DefaultBids(Ctx& ctx, BidSink& sink) const override
        {
            // ОТДЫХ ВПЕРЕДИ ДРАКИ, НО ПОЗАДИ СДАЧИ — порядок лестницы, слово в слово
            // (`Constellation.cpp:3056`): «сдать готовое можно и раненым, а вот идти за новой
            // целью — нет».
            // ПРИЗНАК ЗДЕСЬ ДЕШЁВЫЙ И ШИРОКИЙ, А РЕШАЕТ `Useful`. Условие ставки не должно быть
            // УЖЕ условия полезности: `Useful` фильтрует поставленное, непоставленное он не
            // спасёт. Первая редакция ставила по `NeedsRest`, то есть в полосе между порогами
            // ставки не было вовсе — и расширенная проверка не спрашивалась ни разу.
            //
            // Форма та же, что у квестовой стратегии: она ставит на каждого квестодателя в
            // обзоре, а разбирается `Useful`.
            if (ctx.World.NeedsRest()
                || (ctx.St && ctx.St->Running == ActionId::Rest))
                sink.Add(ActionId::Rest, REL_HIGH, Subject());
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

            // КЛЕТКА СТАВИТСЯ НЕЗАВИСИМО ОТ БОЯ, и спорить им не о чем. У лестницы клетка
            // достижима только когда бить некого; у движка это выходит само — существо,
            // невосприимчивое к игрокам, обход в боевые кандидаты не берёт, значит ставки на
            // бой по нему и нет.
            if (scan.CageSpawn)
                sink.Add(ActionId::OpenCageForTarget, REL_HIGH,
                         Subject::OfSpawn(uint32(scan.CageSpawn)));

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
        engine.Register(std::make_unique<RestAction>());
        engine.Register(std::make_unique<SurvivalStrategy>());
        engine.Register(std::make_unique<KillObjectiveAction>());
        engine.Register(std::make_unique<OpenCageForTargetAction>());
        engine.Register(std::make_unique<CombatStrategy>());
    }
}
