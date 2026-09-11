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
    // шагать. ЧЕТЫРЕ — ЧИСЛО ЛЕСТНИЦЫ, и на подходе (`:4088`), и в погоне (`:5731`); прежнее
    // действие ставило два «с запасом внутрь охвата» — своё число, и Кодекс назвал его самым
    // вероятным дефектом переноса: другая длина пути, другая частота застреваний.
    inline constexpr float MELEE_STOP_YARDS = 4.0f;

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

    // СТОРОЖА — ЧИСЛА ЛЕСТНИЦЫ, С ЕЁ ЖЕ ПРИЧИНАМИ (`case Behavior::Attacking`).
    inline constexpr uint32 NO_OWN_DAMAGE_MS   = 30000;   // тридцать, не пятнадцать: медленное оружие
                                                          // даёт четыре-пять ударов, из них 21 из 117 на
                                                          // ноль урона после брони (замер стенда)
    inline constexpr uint32 CAST_EVERY_MS      = 1500;    // очередь каста этой сборки ЗАМЕЩАЕТ запрос
    inline constexpr uint32 WANTED_EVERY_MS    = 1000;    // обход всего журнала — не на такте
    inline constexpr uint32 FIGHT_FUSE_MS      = 300000;  // предохранитель, не судья: две минуты
                                                          // обрывали бои, которые шли как надо
    inline constexpr uint32 FIGHT_GAP_MS       = 1000;    // разрыв наблюдения: срез недостоверен
    // ОТВОД — числа лестницы (`ApproachingTarget`, «ОТВОД»): дошли (2 яр.), затянулось (12 с),
    // застряли (3 с без сдвига на ярд), точка маршрута пройдена (1,5 яр.).
    inline constexpr float  KITE_ARRIVED_YARDS = 2.0f;
    inline constexpr uint32 KITE_MAX_MS        = 12000;
    inline constexpr uint32 KITE_STUCK_MS      = 3000;
    inline constexpr float  KITE_WAYPOINT_YARDS = 1.5f;
    inline constexpr uint32 KITE_GAVE_UP       = 0xFFFFFFFFu;

    // БОЙ — ОДИН МЕХАНИЗМ, ОДИН ВЛАДЕЛЕЦ ЦЕЛИ (решение 2026-09-11, утверждено Мастером).
    //
    // Замер, который это написал: пятнадцать минут движка вживую, «провёл 95,7 %» — и две
    // победы против пятидесяти девяти у лестницы, одиннадцать гибелей, девять из них «цели не
    // было», предметы заданий не собраны, спутник трижды погиб на одном пятне. Прежнее
    // действие было ТОЛЬКО замахом: дойти, повернуться, выбрать, ударить один раз. Всё, что
    // делает бой боем, у лестницы живёт в `Attacking` и шести помощниках — и движок шёл мимо.
    //
    // ФАЗЫ, А НЕ ОТДЕЛЬНЫЕ СТАВКИ (Кодекс, дизайн боя, п. 1): подход → вступление → удержание →
    // исход → лут. Одна ставка, один предмет, одно состояние `EngineState::Fight`, которое
    // переживает `Reset` и кончается только исходом.
    //
    // ВСТУПЛЕНИЕ ПОДТВЕРЖДАЕТ ЯДРО, а не факт отправки: как `TryAttack` у лестницы, после замаха
    // спрашиваем `GetVictim() == цель`, и только тогда снимаем базу телеметрии и докладываем
    // «вступил» — с этого момента гибель припишется этой цели, как у лестницы.
    //
    // ИСХОД ПО АТРИБУЦИИ ЯДРА (`:5564`): победа — убийств стало больше ЗА ЭТОТ БОЙ И последний
    // убитый — наш; одного счётчика мало (добили кого-то ещё), одного совпадения мало (существо
    // возрождается с тем же GUID). Победа докладывается в память опасности и лутится — со своего
    // убийства и только с него.
    //
    // ЧТО ЗДЕСЬ ЕЩЁ НЕ ПОРТИРОВАНО, НАЗВАНО: отвод (кайт) для дальников по маршруту мовера спиной
    // — следующим коммитом того же переноса, до включения `Combat` вживую; и дистанция
    // вступления для дальников (`EngageRangeAgainst`) читается, но сегодня равна нулю у всех:
    // умения выключены (`Cfg().Abilities`), и лестница тоже дерётся вплотную.
    class KillObjectiveAction final : public Action
    {
    public:
        KillObjectiveAction() : Action(ActionId::KillObjective) { }

        BackoffKind DeferKind() const override { return BackoffKind::CombatUnreachable; }

        bool Useful(Ctx& ctx, Bid const& bid) override
        {
            if (bid.About.What() != Subject::Kind::Unit)
                return false;
            ObjectGuid const victim = bid.About.Guid();
            if (victim.IsEmpty() || !ctx.St)
                return false;
            // ИДУЩИЙ БОЙ ПОЛЕЗЕН, ПОКА НЕ КОНЧИЛСЯ: обход пересчитывается раз в секунду и может
            // назвать другую цель, но брошенный на середине бой — это две цели у одного спутника
            // (сторож подмены у лестницы ровно про это). Свою цель бой доводит до исхода.
            if (ctx.St->Fight.Engaged && ctx.St->Fight.Victim == victim)
                return true;
            return Val<ValueId::Objectives>(ctx).Fight == victim;
        }

        bool Possible(Ctx& ctx, Bid const& bid) override
        {
            // Вступивший бой возможен, пока цель есть в мире — живая или уже труп: исход
            // разбирается внутри, иначе победа никогда не будет засчитана.
            if (ctx.St && ctx.St->Fight.Engaged && ctx.St->Fight.Victim == bid.About.Guid())
                return true;
            return ctx.World.CanSee(bid.About.Guid());
        }

        float Score(Ctx& ctx, Bid const& bid, float relevance) const override
        {
            // ИДУЩИЙ БОЙ НЕ ТОРГУЕТСЯ ЗА РАССТОЯНИЕ: цель могла отбежать, и наклон по ярдам
            // отдал бы такт походу по карте посреди боя.
            if (ctx.St && ctx.St->Fight.Engaged && ctx.St->Fight.Victim == bid.About.Guid())
                return relevance;
            if (std::optional<float> const d = ctx.World.DistanceTo(bid.About.Guid()))
                return relevance - *d * YARD_COST;
            return relevance;
        }

        bool Execute(Ctx& ctx, Bid const& bid) override
        {
            ObjectGuid const victim = bid.About.Guid();
            if (victim.IsEmpty() || !ctx.St)
                return false;
            EngineState::FightState& f = ctx.St->Fight;

            // НОВАЯ ЦЕЛЬ — НОВЫЙ БОЙ. Вступивший по другой цели бой сюда не попадает: `Useful`
            // держит его на своей цели до исхода. Невступивший — просто подход, его бросить
            // ничего не стоит.
            if (f.Victim != victim)
                Begin(ctx, f, victim, ctx.World.EntryOf(victim), ctx.NowMs);

            uint32 slice = ctx.NowMs - f.LastTickMs;
            if (f.LastTickMs == 0 || slice > FIGHT_GAP_MS)
                slice = 0;                          // первый такт или разрыв: срез недостоверен
            f.LastTickMs = ctx.NowMs;

            // ПОКА ОТВОДИМ — ЕЩЁ ПОДХОД, даже если замах уже принят: сторожа удержания у лестницы
            // живут в `Attacking`, куда она попадает только по окончании отвода.
            if (!f.Engaged || f.Kiting)
                return Engage(ctx, f, victim, slice);

            // ---- ИСХОД: ядро обнуляет `GetVictim()` в момент смерти цели -------------------
            ObjectGuid const cur = ctx.World.CurrentVictim();
            if (cur.IsEmpty() || !ctx.World.IsAliveUnit(victim))
                return Outcome(ctx, f, victim);

            // ---- СТОРОЖА, порядок лестницы --------------------------------------------------
            // ЦЕЛЬ ПОДМЕНИЛАСЬ ПОД НАМИ: весь учёт привязан к ОДНОЙ цели, с другой он
            // бессмыслен. Уходим и выбираем заново — но уже с новым отсчётом.
            if (cur != victim)
                return End(ctx, f, "цель подменилась", /*ban=*/false);

            BlowsSnapshot const now = ctx.Fight.Snapshot();
            if (now.Dealt > f.DealtHigh)
            {
                f.DealtHigh  = now.Dealt;
                f.NoDamageMs = 0;                   // НАШ урон в ЭТОМ бою есть — считаем заново
            }
            else
                f.NoDamageMs += slice;

            // УМЕНИЕ — ДОБАВКА К АВТОУДАРУ, А НЕ ЗАМЕНА ЕМУ, раз в полторы секунды.
            f.CastMs += slice;
            if (f.CastMs >= CAST_EVERY_MS)
            {
                f.CastMs = 0;
                CastThroughDoor(ctx, victim, f.Cast);
            }

            // ЦЕЛЬ НАБРАНА — БОЙ ОКОНЧЕН, ДАЖЕ ЕСЛИ ПРОТИВНИК ЖИВ (манекен, зачёт ударами).
            f.WantedCheckMs += slice;
            if (f.WantedCheckMs >= WANTED_EVERY_MS)
            {
                f.WantedCheckMs = 0;
                if (!ctx.World.StillWanted(f.VictimEntry))
                    return End(ctx, f, "цель задания набрана", /*ban=*/false);
            }

            if (f.NoDamageMs > NO_OWN_DAMAGE_MS)
                return End(ctx, f, "бью, а следа нет — не наша цель", /*ban=*/true);

            // ---- УДЕРЖАНИЕ: догнать или стоять лицом ---------------------------------------
            if (!ctx.World.InMeleeRange(victim))
            {
                std::optional<Position> const where = ctx.World.WhereIs(victim);
                std::optional<float> const d = ctx.World.DistanceTo(victim);
                if (!where || !d)
                    return Outcome(ctx, f, victim);     // исчез между проверками — разобрать исход
                float const dt = ctx.Act.SliceSeconds();
                bool const going = WalkTowards(ctx, *where, MELEE_STOP_YARDS, dt);
                bool const stalled = !going && *d > MELEE_STOP_YARDS;
                if (AdvanceWalk(ctx, bid.About, *d, uint32(dt * 1000.0f), stalled) != WalkVerdict::Going)
                    return End(ctx, f, "до цели в бою не дойти", /*ban=*/true);
            }
            else
            {
                // ДОШЛИ: сперва остановиться, потом повернуться. Порядок важен: пока догоняем,
                // поворот задаёт само движение; остановка, не сделанная при входе в досягаемость,
                // оставляла бы «иду вперёд» висеть, пока шагов уже нет.
                ctx.Act.StopMoving();
                ctx.Act.Face(victim);
            }

            f.FightMs += slice;
            if (f.FightMs > FIGHT_FUSE_MS)
                return End(ctx, f, "пять минут боя без исхода", /*ban=*/false);
            return true;
        }

        // ЧТО КОНЧАЕТ БОЙ, А ЧТО НЕТ — В ЭТОМ ВСЁ ПОСТАНОВЛЕНИЕ. `EnteredFromOutside` приходит с
        // каждой сменой эпохи у лестницы, а её предохранители идут под нашим боем; кончать бой по
        // ней — стирать его на середине замаха. Настоящие концы — смерть, карта, выход, роспуск,
        // выключение — докладываются как «кончил» и гасят состояние.
        void Cancel(Ctx& ctx, Subject const& about, CancelReason why) override
        {
            if (!ctx.St)
                return;
            EngineState::FightState& f = ctx.St->Fight;
            if (f.Victim.IsEmpty() || (about.What() == Subject::Kind::Unit && about.Guid() != f.Victim))
                return;
            switch (why)
            {
                case CancelReason::EnteredFromOutside:
                case CancelReason::Finished:
                    return;                         // бой продолжится следующим тактом
                default:
                    break;
            }
            if (f.Engaged)
            {
                Report(ctx, f, FightEvent::Ended, ReasonText(why));
                ctx.Act.AttackStop();               // и руки тоже: ядро продолжало бы автоатаку
            }
            f = EngineState::FightState{ .Loot = f.Loot };
        }

    private:
        static char const* ReasonText(CancelReason why)
        {
            switch (why)
            {
                case CancelReason::Died:           return "погиб";
                case CancelReason::MapChanged:     return "сменилась карта";
                case CancelReason::LoggedOut:      return "вышел";
                case CancelReason::Dismissed:      return "распущен";
                case CancelReason::EngineDisabled: return "движок выключен";
                case CancelReason::SubjectGone:    return "цель исчезла";
                case CancelReason::Failed:         return "не вышло";
                default:                           return "прервано";
            }
        }

        static void Begin(Ctx& ctx, EngineState::FightState& f, ObjectGuid victim, uint32 entry, uint32 nowMs)
        {
            LootCounters const keep = f.Loot;       // за всё время — не за бой
            f = EngineState::FightState{};
            f.Loot        = keep;
            f.Victim      = victim;
            f.VictimEntry = entry;
            f.LastTickMs  = nowMs;
            // С ЧЕМ ШЛИ В БОЙ — снимок обхода на момент выбора, как `EngageAssists`/`PackCenter`
            // у лестницы: обход пересчитается через секунду и уже про другую цель.
            ObjectiveScan const& scan = Val<ValueId::Objectives>(ctx);
            if (scan.Fight == victim)
            {
                f.Assists    = scan.Assists;
                f.PackCenter = scan.PackCenter;
                f.PackKnown  = scan.PackKnown;
            }
        }

        // ДОШЛИ ИЛИ ИДЁМ; ДОШЛИ — ВСТУПАЕМ, ровно то, что делает игрок мышью, в том же порядке.
        bool Engage(Ctx& ctx, EngineState::FightState& f, ObjectGuid victim, uint32 slice)
        {
            if (!f.EngageRangeKnown)
            {
                f.EngageRange      = ctx.World.EngageRangeAgainst(victim);
                f.EngageRangeKnown = true;
            }
            bool const closeEnough = ctx.World.CloseEnough(victim, f.EngageRange);

            // ---- ОТВОД: УТАЩИТЬ ЦЕЛЬ ОТ ЛАГЕРЯ И ТАМ ДОБИТЬ (лестница, `:3986-4076`) -----
            // Точку берём один раз — прочь от того места, где мы её зацепили; идём спиной,
            // лицом к цели, поэтому удары не прерываются. Условия — её: уводить разрешено,
            // у цели были заступники, мы уже в бою, и в этом бою ещё не пробовали.
            if (ctx.World.KiteYards() > 0.0f && f.Assists > 0 && ctx.World.IsInCombat()
                && !f.Kiting && f.KiteMs == 0 && ctx.St)
            {
                if (ctx.World.BuildKiteRoute(f.PackKnown, f.PackCenter, victim, ctx.World.KiteYards(),
                                             ctx.St->Move.Waypoints, &f.KiteTo))
                {
                    ctx.St->Move.WaypointIndex = ctx.St->Move.Waypoints.size() > 1 ? 1 : 0;
                    f.Kiting = true;
                    f.KiteMs = 1;
                    ctx.St->Move.LastX = ctx.World.Where().GetPositionX();
                    ctx.St->Move.LastY = ctx.World.Where().GetPositionY();
                    ctx.St->Move.StuckMs = 0;
                    // ОСОЗНАННОЕ РАСХОЖДЕНИЕ С ЛЕСТНИЦЕЙ: `Stalled` ставит только её шаг к цели,
                    // который во время отвода не зовётся, — у неё старое значение с подхода
                    // может оборвать новый отвод мгновенно (Кодекс, п. 6). Здесь он гасится.
                    ctx.St->Move.Stalled = false;
                }
                else
                    f.KiteMs = KITE_GAVE_UP;        // некуда пятиться — больше не пробуем в этом бою
            }
            if (f.Kiting && ctx.St)
            {
                MoveState& mv = ctx.St->Move;
                f.KiteMs += slice;
                // ЗАСТРЕВАНИЕ ВО ВРЕМЯ ОТВОДА СУДИМ ТЕМ ЖЕ, ЧЕМ И ОБЫЧНЫЙ ШАГ: сдвинулись ли.
                Position const here = ctx.World.Where();
                if (here.GetExactDist2d(mv.LastX, mv.LastY) > 1.0f)
                {
                    mv.LastX = here.GetPositionX(); mv.LastY = here.GetPositionY();
                    mv.StuckMs = 0;
                }
                else
                    mv.StuckMs += slice;
                float const left = here.GetExactDist2d(f.KiteTo.GetPositionX(), f.KiteTo.GetPositionY());
                // ДОШЛИ, ЗАСТРЯЛИ ИЛИ ЗАТЯНУЛОСЬ — ДЕРЁМСЯ ЗДЕСЬ. И ОБЯЗАТЕЛЬНО ОСТАНАВЛИВАЕМСЯ:
                // иначе сервер продолжает видеть «иду назад».
                if (left <= KITE_ARRIVED_YARDS || f.KiteMs > KITE_MAX_MS || mv.Stalled
                    || mv.StuckMs > KITE_STUCK_MS || mv.WaypointIndex >= mv.Waypoints.size())
                {
                    f.Kiting = false;
                    // КАК `StopMovingCore` (`:1333`): отказ значит, что для сервера мы всё ещё
                    // идём, и флаг сбрасывается только по принятой остановке.
                    if (ctx.Act.StopMoving())
                        mv.Moving = false;
                    mv.Waypoints.clear();           // мовер перестроит: пустой маршрут — его
                    mv.WaypointIndex = 0;           // первое условие пересчёта (`:1475`)
                }
                else
                {
                    // ДАЛЬНОБОЙНЫЙ ЧИТАЕТ СТОЯ: идёт каст — стоим и не мешаем себе.
                    if (ctx.World.IsCasting())
                    {
                        ctx.Act.StopMoving();
                        return true;
                    }
                    // ШАГ ПО ТОЧКАМ ПОСТРОИТЕЛЯ, А НЕ ПО ПРЯМОЙ, но спиной — лицом к цели.
                    Position const& wp = mv.Waypoints[mv.WaypointIndex];
                    if (here.GetExactDist2d(wp.GetPositionX(), wp.GetPositionY()) < KITE_WAYPOINT_YARDS)
                        ++mv.WaypointIndex;
                    else
                    {
                        Position next;
                        if (ctx.World.BackStepToward(wp, victim, ctx.Act.SliceSeconds(), &next))
                        {
                            ctx.Act.Step(next, MOVEMENTFLAG_BACKWARD);
                            mv.Moving = true;
                        }
                    }
                    // И БЬЁМ, ЕСЛИ ДОСТАЁМ, не переставая пятиться — как `TryAttack` в отводе.
                    // Лестница замахивается каждый такт и каждый раз снимает базу заново; здесь
                    // замах один, а повторяется он только если ядро потеряло состояние атаки —
                    // тогда `GetVictim()` уже не наша цель (Кодекс, п. 5: «остаётся ли автоатака»
                    // не утверждается, а проверяется у ядра).
                    if (closeEnough && (!f.Engaged || ctx.World.CurrentVictim() != victim))
                        Swing(ctx, f, victim);
                    return true;
                }
            }

            // ОТВОД КОНЧИЛСЯ, А ЗАМАХ УЖЕ ПРИНЯТ — второй раз не вступаем: повторная база сдвинула
            // бы отсчёт побед на середину боя (у лестницы `TryAttack` зовётся снова, и это её
            // причуда, а не правило).
            if (f.Engaged)
                return true;

            if (!closeEnough)
            {
                std::optional<Position> const where = ctx.World.WhereIs(victim);
                if (!where)
                    return false;                   // цель исчезла между выбором и шагом
                std::optional<float> const d = ctx.World.DistanceTo(victim);
                if (!d)
                    return false;
                float const dt = ctx.Act.SliceSeconds();
                // ДИСТАНЦИЯ ВСТУПЛЕНИЯ — ЛЕСТНИЦЫ (`:4088`): дальность заклинания у дальника при
                // включённых умениях, иначе четыре ярда. Сегодня умения выключены — ноль у всех.
                float const stopAt = f.EngageRange > 0.0f ? f.EngageRange : MELEE_STOP_YARDS;
                bool const going = WalkTowards(ctx, *where, stopAt, dt);
                // «ДОШЁЛ» — НЕ «ЗАСТРЯЛ»: приход решает ядро, а между его меркой и порогом
                // остановки есть зазор (разбор 2026-09-10 — три действия отсрочивали себя в
                // такт прихода).
                bool const stalled = !going && *d > stopAt;
                if (AdvanceWalk(ctx, Subject::OfUnit(victim), *d, uint32(dt * 1000.0f), stalled) != WalkVerdict::Going)
                {
                    Defer(ctx, BackoffKind::CombatUnreachable, Subject::OfUnit(victim), 0, COMBAT_UNREACHABLE_MS);
                    f = EngineState::FightState{ .Loot = f.Loot };
                    return false;
                }
                return true;
            }

            return Swing(ctx, f, victim);
        }

        // ВСТУПЛЕНИЕ: повернуться, выбрать, ударить — и спросить у ядра, приняло ли оно замах.
        bool Swing(Ctx& ctx, EngineState::FightState& f, ObjectGuid victim)
        {
            // ПОВОРОТ ПЕРВЫМ: `Unit::UpdateMeleeAttackingState` требует `HasInArc`.
            if (!ctx.Act.Face(victim))
                return false;
            if (!ctx.Act.SetSelection(victim))
                return false;
            if (!ctx.Act.AttackSwing(victim))
                return false;
            // ПРОВЕРЯЕМ ПОСЛЕДСТВИЕ, А НЕ ФАКТ ВЫЗОВА: сокета нет, ответа не будет. Ядро приняло
            // — оно и назвало нас атакующим. Не приняло — цель не наша, как у лестницы («удар
            // не принят ядром» → отказ по цели).
            if (ctx.World.CurrentVictim() != victim)
            {
                // УЖЕ ВСТУПИВШИЙ БОЙ НЕ СТИРАЕТСЯ повторным замахом из отвода: это ядро потеряло
                // состояние атаки, и что с этим делать, решит исход следующим тактом.
                if (f.Engaged)
                    return false;
                Defer(ctx, BackoffKind::CombatUnreachable, Subject::OfUnit(victim), 0, COMBAT_UNREACHABLE_MS);
                f = EngineState::FightState{ .Loot = f.Loot };
                return false;
            }
            if (f.Engaged)
                return true;                        // повторный замах: база и доклад уже есть
            // ОТСЕЧКА: всё, что насчитается дальше, относится ИМЕННО к этому бою — ОДИН РАЗ.
            f.Base      = ctx.Fight.Baseline();
            f.DealtHigh = f.Base.Dealt;
            f.Engaged   = true;
            f.CastMs    = CAST_EVERY_MS;            // первое решение об умении — сразу
            Report(ctx, f, FightEvent::Engaged, "вступил");
            return true;
        }

        // ПОБЕДУ СЧИТАЕТ ЯДРО, А НЕ Я. Два условия, и оба нужны.
        bool Outcome(Ctx& ctx, EngineState::FightState& f, ObjectGuid victim)
        {
            BlowsSnapshot const now = ctx.Fight.Snapshot();
            bool const won = now.Kills > f.Base.Kills && now.LastKilled == victim;
            if (won)
            {
                Report(ctx, f, FightEvent::Won, "ПОБЕДА");
                // ДОБЫЧА ТОЛЬКО СО СВОЕГО УБИЙСТВА: право проверит и ядро, но пакет, заведомо
                // обречённый на отказ, лучше не слать. Настройка — лестницы.
                if (ctx.World.LootAllowed())
                    LootThroughDoor(ctx, victim, f.Loot);
            }
            else
                Report(ctx, f, FightEvent::Ended,
                       ctx.World.IsAliveUnit(victim) ? "бой прекратился" : "цель мертва, но добили не мы");
            f = EngineState::FightState{ .Loot = f.Loot };
            return true;                            // такт был боем — что бы ни вышло
        }

        bool End(Ctx& ctx, EngineState::FightState& f, char const* why, bool ban)
        {
            Report(ctx, f, FightEvent::Ended, why);
            ctx.Act.AttackStop();
            if (ban)
                Defer(ctx, BackoffKind::CombatUnreachable, Subject::OfUnit(f.Victim), 0, COMBAT_UNREACHABLE_MS);
            f = EngineState::FightState{ .Loot = f.Loot };
            return !ban;
        }

        static void Report(Ctx& ctx, EngineState::FightState const& f, FightEvent what, char const* why)
        {
            FightOutcome ev;
            ev.What        = what;
            ev.Victim      = f.Victim;
            ev.VictimEntry = f.VictimEntry;
            ev.MapId       = ctx.World.MapId();
            Position const here = ctx.World.Where();
            ev.X = here.GetPositionX();
            ev.Y = here.GetPositionY();
            ev.Why = why;
            ctx.Fight.Report(ev);
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
            // ВСТУПИВШИЙ БОЙ СТАВИТСЯ ПЕРВЫМ И ИЗ СОСТОЯНИЯ, а не из обхода: обход
            // пересчитывается раз в секунду и вправе назвать другую цель, но у спутника одна
            // пара рук и один противник, которого ядро уже считает его жертвой. Без этой ставки
            // бой осиротел бы — `Useful` никто не спросит, состояние висело бы «вступил» вечно.
            if (ctx.St && ctx.St->Fight.Engaged && !ctx.St->Fight.Victim.IsEmpty())
            {
                sink.Add(ActionId::KillObjective, REL_HIGH, Subject::OfUnit(ctx.St->Fight.Victim));
                return;
            }
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
