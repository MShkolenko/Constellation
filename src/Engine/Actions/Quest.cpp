/*
 * Constellation — the first action, and the strategy that bids for it.
 *
 * Contract: migration plan step 13 — the Idle branch's actions, one at a time. This is the first
 * thing in the engine that both READS a value and PRICES itself, so it is also the first proof
 * that steps 11′ and 12 wired up correctly.
 *
 * WHAT RUNS TODAY, AND WHAT DOES NOT. `Owns()` is false for every mode, so the seam never calls
 * this. The SHADOW does: it ticks, bids, scores and writes «ТЕНЬ <имя>: выбрал бы X» — and never
 * executes, because shadow mode skips `Execute` and never opens the movement tick. So the whole
 * of this file's observable effect today is one log line per changed decision, which is exactly
 * the measurement the shadow exists for.
 *
 * `Execute` IS WRITTEN ANYWAY, AND WRITTEN FOR REAL. A stub returning false would be a landmine:
 * at the flip it would fail on the first tick and push alternatives forever, and nobody would
 * remember why. It sends the same packet the ladder sends (CMSG_QUEST_GIVER_COMPLETE_QUEST,
 * through the door), so the flip changes who decides, not what is done.
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

    // §4.3′ — РАССТОЯНИЕ ВХОДИТ В ЦЕНУ, А НЕ В ОТБОР. Ярд стоит немного, но стоит: между двумя
    // одинаково готовыми сдачами выигрывает ближняя, а между близкой ерундой и далёкой важностью
    // по-прежнему важность. Число маленькое намеренно — это наклон, а не запрет.
    inline constexpr float YARD_COST = 0.01f;

    // ПОТОЛОКА РАССТОЯНИЯ У СДАЧИ БОЛЬШЕ НЕТ, И ЭТО НЕ ПОСЛАБЛЕНИЕ.
    //
    // Здесь стояло 600 ярдов с оговоркой, что дальняя сдача «на порядок дороже всего остального в
    // ветке, и её место — в походе, который придёт своим шагом». Прибор показал, чем это обошлось:
    // «готовых 2, за потолком 2 (ближайший 622 ярдов при потолке 600)» — двадцать два ярда, и
    // сдача не рассматривалась вовсе, потому что обещанный поход так и не был написан.
    //
    // Заменять число большим числом нельзя: ступенька в произвольном месте промахнётся снова.
    // Работу потолка делает ЦЕНА, и делает её согласованно с остальными ставками: `REL_HIGH`
    // минус ярды даёт на 622 ярдах 13.8 — выше взятия рядом (10) и много выше похода по карте, а
    // на двух тысячах ярдов ноль, то есть ниже всего. Ровно то, что потолок пытался выразить
    // ступенькой, но уже наклоном — и у лестницы потолка тоже нет, она просто доходит.
    //
    // ПРИХОД — 25 ЯРДОВ ДО ТОЧКИ ПОЯВЛЕНИЯ, число лестницы (`Constellation.cpp:3699`), и оно
    // работает в паре с обзором: ходячий принимающий стоит не там, где его точка. Обзор
    // (`GIVER_SEARCH_YARDS`) спрашивается КАЖДЫЙ такт по дороге, поэтому сдача случается сразу,
    // как только ядро увидело принимающего, а не по формальному приходу.
    inline constexpr float TURNIN_ARRIVED_YARDS = 25.0f;

    // ДОРОГА К ПРИНИМАЮЩЕМУ НЕ ИДЁТ — ДЕСЯТЬ МИНУТ, столько же, сколько у похода к квестодателю
    // и у недостижимой боевой цели. Одно число на все три случая потому, что случай один:
    // «туда не дойти сейчас».
    inline constexpr uint32 TURNIN_UNREACHABLE_MS = 600000;
    // Сдача не прошла у принимающего / самому себе — сроки лестницы (`:4207`, `:4144`).
    inline constexpr uint32 TURNIN_FAILED_MS      = 60000;
    inline constexpr uint32 TURNIN_SELF_FAILED_MS = 300000;

    // НА СКОЛЬКО ЗАБЫТЬ КВЕСТОДАТЕЛЯ, У КОТОРОГО МЕНЮ ПУСТОЕ. Десять минут — не моё число:
    // ровно столько держит `GiverUnreachable` у лестницы (`Constellation.cpp`, ветка выбора из
    // меню), и по той же причине — «уровень растёт, предыдущие квесты закрываются, меню
    // меняется». Запрет по особи, не по виду.
    inline constexpr uint32 GIVER_EMPTY_MS = 600000;

    // НА СКОЛЬКО ЗАБЫТЬ КВЕСТ, КОТОРЫЙ НЕ ВЗЯЛСЯ. У лестницы такой запрет ВЕЧНЫЙ
    // (`QuestRefused.insert`), и это осознанная разница, а не упущение: её набор растёт без
    // предела, наша таблица ограничена шестнадцатью записями на спутника. Мы меняем вечность на
    // ограниченность — час достаточно долог, чтобы петля не крутилась, и достаточно короток,
    // чтобы запись не вытеснила ту, что нужнее.
    inline constexpr uint32 QUEST_REFUSED_MS = 3600000;

    // ПРИШЛИ. Двадцать пять ярдов по плоскости — число лестницы (`Constellation.cpp:3699`), и
    // мерка тоже её: до ТОЧКИ СПАВНА, а не до NPC. Ходячий стоит не там, где его точка, и
    // добирает эти метры уже обзор.
    inline constexpr float SEEK_ARRIVED_YARDS = 25.0f;

    // НА СКОЛЬКО ЗАБЫТЬ ТОЧКУ ПОСЛЕ ПРИХОДА. Десять минут, и лестница ставит их ТОЖЕ ПРИ
    // УДАЧНОМ приходе (`:3700`) — причину нашёл Кодекс и она записана там же: иначе у
    // пришедшего впустую тут же начинается поход к соседней точке, цепочка походов вместо дела.
    inline constexpr uint32 SEEK_VISITED_MS = 600000;

    // И СТОЛЬКО ЖЕ — ТОЧКЕ, ДО КОТОРОЙ НЕ ДОШЛИ. Столько держит `GiverUnreachable` у лестницы.
    inline constexpr uint32 SEEK_UNREACHABLE_MS = 600000;

    // ПОХОД К МЕСТУ ЗАДАНИЯ — числа лестницы, `case Behavior::Travelling` (`:3896-3961`).
    // «Пришёл» — `TravelStop + 2` (порог отдаёт политика: 10 ярдов у точки, 3 у триггера);
    // «пришёл — целей нет», «место стало смертельным» и «не дойти» — все три откладывают ЭТОТ
    // квест на десять минут, остальным дорога открыта.
    inline constexpr float  TRAVEL_ARRIVED_SLACK   = 2.0f;
    inline constexpr uint32 TRAVEL_VISITED_MS      = 600000;
    inline constexpr uint32 TRAVEL_UNREACHABLE_MS  = 600000;

    // ОТПРАВЩИКА ЗДЕСЬ НЕТ, И ЭТО ГЛАВНОЕ В ЭТОМ ШВЕ. Он один на весь движок и живёт в
    // `Engine.cpp`, где его нельзя подменить: `WalkTowards` не принимает его параметром. Первая
    // версия принимала — и Кодекс показал, что тогда любое действие вправе передать свой способ
    // записи, а комментарий об инварианте 0 доказывает не больше, чем обещает.

    // ПАМЯТЬ ОБ ОТКАЗАХ ДВИЖКА — ТА ЖЕ ТАБЛИЦА ОТСРОЧЕК, только ключ здесь КВЕСТ, а не
    // квестодатель: не взялся конкретный квест, а не «у этого NPC нечего брать».
    //
    // Без неё выбор движка расходился бы с выбором лестницы ровно на отказных квестах — и хуже
    // того, при флипе давал бы петлю: действие вернуло бы ложь, ставка возникла бы следующим
    // тактом, политика выбрала бы тот же квест. Нашёл Кодекс; я успел написать в сообщении
    // коммита, что выбор совпадает «по построению», и это было неправдой.
    bool QuestRefusedHere(void const* user, uint32 questId)
    {
        Ctx const* ctx = static_cast<Ctx const*>(user);
        if (!ctx || !ctx->St)
            return false;
        BackoffKey key;
        key.Kind  = BackoffKind::CoreRefused;
        key.About = Subject::OfQuest(questId);
        return Engine::Deferred(*ctx->St, key, ctx->NowMs);
    }

    class TurnInQuestAction final : public Action
    {
    public:
        TurnInQuestAction() : Action(ActionId::TurnInQuest) { }

        // lazy: обход не кэширован. `Possible` и `Execute` обходят сетку каждый сам, и
        // Кодекс посчитал цену: до 32 обходов в секунду на восьми спутниках в худшем случае,
        // на пятнадцати путь догоняет лестницу, на 122 — ~488 в секунду, то самое число, от
        // которого лестница ушла в кэш («искать редко, помнить найденное», Constellation.cpp:
        // 3999-4003). Сегодня цена ноль: ставка ставится только при готовом к сдаче квесте, за
        // восемь минут такой был у одного спутника, `Update time diff` — 1 мс.
        // ПОРОГ, ЗА КОТОРЫМ ЭТО ПЕРЕСТАЁТ БЫТЬ ДОПУСТИМЫМ: пятнадцать спутников.
        // ПУТЬ НАВЕРХ: разрешённый гуид кладётся в `TurnInCandidate` при пересчёте значения —
        // у него уже есть свой интервал и свой слот на спутника. Делается на шаге 14, где у
        // этого гуида появляется второй потребитель (дорога к принимающему); сделанный здесь
        // и сейчас, кэш назавтра станет второй копией правила.
        //
        // ГРАНИЦА ОБХОДА, А НЕ ПОРОГ БЛИЗОСТИ. Здесь стояло 8 ярдов — придуманное число,
        // которое решало, возможна ли сдача. Ровно за это лестница заплатила 916 кругами и
        // нулём сдач (Constellation.cpp:3984-3998), и её вывод записан там же: порог знает
        // ядро. Отвечает `CanInteractWithQuestGiver`, а это число лишь ограничивает обход сетки.
        //
        // ШЕСТЬДЕСЯТ, А НЕ СОРОК, И ЭТО ИСПРАВЛЕНИЕ СЛОЖЕНИЯ ЧИСЕЛ ИЗ РАЗНЫХ ВЕТОК. Сорок брались
        // у `TurningIn`, которая доходит до САМОГО NPC и запаса не требует. Теперь положение
        // другое — идём к ТОЧКЕ ПОЯВЛЕНИЯ и находим по обзору, — а для этого положения у
        // лестницы своя пара: приход 25 и обзор 60 (`Constellation.cpp:3699`, `:3713`). Взяв
        // приход из одной пары и обзор из другой, я оставил дыру: встав в 25 ярдах от точки,
        // спутник может быть в 65 от ходячего принимающего и не увидеть его вовсе (Кодекс).
        // Пара берётся целиком.
        static constexpr float GIVER_SEARCH_YARDS = 60.0f;

        // §14 — ПОД КАКИМ ВИДОМ ЗАПРЕТА ХОДИТ ЭТО ДЕЙСТВИЕ. Движок фильтрует по паре
        // {этот вид, предмет ставки} до `Useful`, то есть по ТОЧНОМУ ключу: запрет «до этого
        // принимающего не дотянуться» не должен мешать взять у того же NPC новый квест, когда
        // такое действие появится.
        BackoffKind DeferKind() const override { return BackoffKind::Unreachable; }

        // §3.3 — БЕСПОЛЕЗНО И НЕВОЗМОЖНО — РАЗНЫЕ ВОПРОСЫ С РАЗНЫМ ВОССТАНОВЛЕНИЕМ.
        // Здесь именно «бесполезно»: квест уже не готов к сдаче, ставку надо просто выбросить.
        bool Useful(Ctx& ctx, Bid const& bid) override
        {
            // ВИД СПРАШИВАЕТСЯ ЯВНО. У `Subject` нет `Quest()`: нагрузка читается одним `Id()`,
            // а вид — через `What()`, и прочитанное не тем видом возвращает пустоту. Пара
            // заставляет назвать вид, вместо того чтобы угадать его по имени поля.
            if (bid.About.What() != Subject::Kind::Quest)
                return false;
            uint32 const quest = bid.About.Id();
            if (!quest)
                return false;
            return ctx.World.StatusOf(quest) == QUEST_STATUS_COMPLETE;
        }

        // §3.3 — А ЗДЕСЬ «НЕВОЗМОЖНО СЕЙЧАС», И ЭТО ДРУГОЕ ВОССТАНОВЛЕНИЕ.
        //
        // Квест годен к сдаче, а принимающего рядом нет. Выбросить ставку (`Useful`) было бы
        // неверно: работа не отпала, отпала только возможность сделать её В ЭТУ СЕКУНДУ.
        // Движок на `Possible == false` толкает альтернативы и возвращает ставку чуть ниже
        // (`Engine.cpp:590-597`), то есть предлагает заняться другим и вернуться — ровно то,
        // что нужно. Разницу назвал Кодекс, и она не терминологическая.
        //
        // Самосдача возможна всегда: принимающий — сам спутник.
        bool Possible(Ctx& ctx, Bid const& bid) override
        {
            // ТЕПЕРЬ «ВОЗМОЖНО» ЗНАЧИТ «ЕСТЬ КУДА ИДТИ», А НЕ «ПРИНИМАЮЩИЙ УЖЕ РЯДОМ».
            //
            // Раньше здесь спрашивался обзор, и отсутствие принимающего в сорока ярдах делало
            // ставку невозможной — то есть работа откладывалась ровно тогда, когда её надо было
            // начать: пойти. Значение отдаёт только тех кандидатов, у которых нашлась точка
            // появления принимающего, значит идти есть куда всегда, а самосдача возможна и
            // подавно.
            uint32 const quest = bid.About.Id();
            for (TurnInCandidate const& t : Val<ValueId::CompletedTurnIns>(ctx))
                if (t.QuestId == quest)
                    return true;
            return false;
        }

        // ЦЕНА СЧИТАЕТСЯ ПО ТОМУ ЖЕ СПИСКУ, ПО КОТОРОМУ СТАВИЛАСЬ СТАВКА, и это не лишний
        // проход: значение кэшировано, а второй раз за такт оно не пересчитывается по
        // построению (`Value<T>::Get` смотрит на отметку времени).
        // `const` — НЕ УКРАШЕНИЕ: контракт `Score` «чист относительно такта», и константность
        // держит его типом, а не обещанием. `Ctx&` остаётся изменяемым только потому, что чтение
        // значения может его пересчитать.
        float Score(Ctx& ctx, Bid const& bid, float relevance) const override
        {
            uint32 const quest = bid.About.Id();
            for (TurnInCandidate const& t : Val<ValueId::CompletedTurnIns>(ctx))
                if (t.QuestId == quest)
                    return relevance - t.Dist * YARD_COST;
            return relevance;
        }

        bool Execute(Ctx& ctx, Bid const& bid) override
        {
            if (bid.About.What() != Subject::Kind::Quest)
                return false;
            uint32 const quest = bid.About.Id();
            if (!quest)
                return false;
            // БЕЗ СОСТОЯНИЯ ЭТО ДЕЙСТВИЕ НЕ ИСПОЛНЯЕТСЯ ВОВСЕ, И ПРОВЕРКА СТОИТ ЗДЕСЬ, А НЕ У
            // ходьбы. Там она возвращала бы ложь, не поставив отсрочки, — то есть возвращала бы
            // петлю, ради которой отсрочка и заведена. Сегодня туда не дойти (без состояния
            // значение отдаёт пустой буфер), но «сегодня не дойти» — свойство нынешнего кода, а
            // не подписи (Кодекс).
            if (!ctx.St)
                return false;

            for (TurnInCandidate const& t : Val<ValueId::CompletedTurnIns>(ctx))
            {
                if (t.QuestId != quest)
                    continue;

                // САМОСДАЧА — ПУСТОЙ ПРИНИМАЮЩИЙ, и дверь выводит это сама (ClientAct.h:83):
                // «сдать самому себе» и «это скриптовая сдача» — одно и то же условие, поэтому
                // оно не предлагается параметром, который вызывающий мог бы опровергнуть.
                // ТРИ ПАКЕТА, А НЕ ОДИН. Первая редакция слала только `CompleteQuest` — он лишь
                // открывает окно награды, награждает `ChooseReward`; квест не сдавался, спутник
                // стоял у принимающего и перевыбирал сдачу весь живой час (53 решения, 25 сдач —
                // все лестницы). Тело сдачи теперь общее (`TurnInCore`), истина — ответ ядра.
                //
                // НЕ ВЫШЛО — ОТЛОЖИТЬ ЭТОТ КВЕСТ, а не пробовать снова четыре раза в секунду:
                // у лестницы минута (`:4144` пять минут самосдаче, `:4207` минута у принимающего),
                // у движка тот же ключ — квест — и тот же срок (Кодекс, проход 1). Вид отсрочки —
                // Unreachable: это ключ ЭТОГО действия (DeferKind), по нему предварительный
                // фильтр снимает ставку до Useful; «ядро не наградило» и «не дойти» для сдачи —
                // одно и то же: этот квест сейчас не сдать.
                if (!t.EnderEntry)
                {
                    if (TurnInThroughDoor(ctx, ObjectGuid::Empty, quest))
                        return true;
                    Defer(ctx, BackoffKind::Unreachable, bid.About, 0, TURNIN_SELF_FAILED_MS);
                    return false;
                }

                // ВИДНО ЛИ ЕГО ПРЯМО СЕЙЧАС. Поиск отдаёт только тех, кому ядро само
                // разрешило сдавать, и спрашивается он ЗДЕСЬ, а не берётся из `Possible`: между
                // выбором и исполнением проходит время, и принимающий успевает умереть,
                // взлететь или уйти.
                if (std::optional<ObjectGuid> const ender =
                        ctx.World.NearestQuestGiverOfEntry(t.EnderEntry, GIVER_SEARCH_YARDS))
                {
                    if (TurnInThroughDoor(ctx, *ender, quest))
                        return true;
                    Defer(ctx, BackoffKind::Unreachable, bid.About, 0, TURNIN_FAILED_MS);
                    return false;
                }

                // НЕ ВИДНО — ЗНАЧИТ ИДЁМ. Это и есть та «отдельная работа», которую прежний
                // комментарий обещал вынести в своё действие: она не понадобилась. Форма та же,
                // что у похода к квестодателю и у боя, — дойти и сделать, — и части те же.
                float const dt = ctx.Act.SliceSeconds();
                bool const going = WalkTowards(ctx, t.Where, TURNIN_ARRIVED_YARDS, dt);

                // РАССТОЯНИЕ БЕРЁМ ТЕКУЩЕЕ, А НЕ `t.Dist`: кэшированное посчитано при пересчёте
                // значения и по дороге устаревает, а `AdvanceWalk` судит именно о приближении.
                float const d = ctx.World.DistanceTo2d(t.Where);
                // «Дошёл» — не «застрял»: приход решает обзор (`NearestQuestGiverOfEntry`),
                // а ходьба останавливается по расстоянию, и между ними зазор.
                uint32 const sliceMs = uint32(dt * 1000.0f);
                bool const stalled = !going && d > TURNIN_ARRIVED_YARDS;

                // ПРОГРЕСС КЛЮЧУЕТСЯ ПРИНИМАЮЩИМ, А НЕ КВЕСТОМ: он про ДОРОГУ. При пересчёте
                // значение вправе выбрать другого принимающего того же квеста, и счётчик
                // застревания пришёл бы из прошлой дороги, оборвав новую не начавшись — тот же
                // дефект, что был найден у `WalkProgress` в день её написания.
                //
                // ОДИН СЛУЧАЙ ОСТАЁТСЯ НЕРАЗЛИЧИМЫМ, и он назван, а не умолчан: две точки
                // появления ОДНОГО вида. Значение берёт ближайшую, так что смена возможна при
                // движении спутника; цена ошибки — один перезапуск счётчика, и заводить ради неё
                // предмет-координату дороже.
                Subject const road = Subject::OfSpecies(t.EnderEntry);
                if (AdvanceWalk(ctx, road, d, sliceMs, stalled) != WalkVerdict::Going)
                {
                    // Дорога не идёт — отставляем КВЕСТ, а не принимающего: у сдачи ключ и есть
                    // сам квест, и лестница держит свой `TurnInBackoff` тоже по квесту.
                    Defer(ctx, BackoffKind::Unreachable, bid.About, 0, TURNIN_UNREACHABLE_MS);
                    return false;
                }
                return true;
            }
            return false;
        }

    };

    // ВЗЯТЬ КВЕСТ У ТОГО, КТО УЖЕ РЯДОМ.
    //
    // Выбор КАКОЙ ИМЕННО квест здесь не написан и написан не будет: он поднят в одну функцию,
    // которую зовёт и лестница (`PickFromQuestMenu`, через `WorldView::BestQuestOffered`).
    // Поэтому движок и лестница выбирают одинаково ПО ПОСТРОЕНИЮ, а не потому что два списка
    // правил совпали. Ровно из-за этого действие и не было написано утром.
    // ЖУРНАЛ ПОЛОН — ЭТО ВСЕ СЛОТЫ, А НЕ ПЕРВЫЕ ТРИ, и полон он по потолку МОДУЛЯ, не ядра:
    // `QuestTick` лестницы (`Constellation.cpp:7290`) не берёт квесты при `used >= MaxQuests`
    // (по умолчанию 10 при 25 у ядра). `MayAccept` спрашивает ядро и знает только 25.
    //
    // Замер 2026-09-12: 16 из 66 переходов «лестница идёт к месту задания» — движок «взять
    // квест рядом». Спутник со всеми слотами занят проходит мимо квестодателя, а движок ставил
    // на него каждый такт REL_NORMAL — выше похода — и уводил бы к нему, чтобы упереться в
    // отказ ядра и записать пустоту квестодателю, которым он не был.
    inline bool QuestLogFull(Ctx& ctx)
    {
        uint32 const used = ctx.World.QuestSlotsUsed();
        return used >= MAX_QUEST_LOG_SIZE || used >= Tuning().MaxQuests;
    }

    class TakeQuestNearbyAction final : public Action
    {
    public:
        TakeQuestNearbyAction() : Action(ActionId::TakeQuestNearby) { }

        // §14 — ПУСТОЙ КВЕСТОДАТЕЛЬ ЗАПОМИНАЕТСЯ. Без этого он выбирался бы каждый такт вечно:
        // знак над головой ставится по `CanSeeStartQuest`, а меню собирается по `CanTakeQuest`,
        // поэтому знак БЫВАЕТ при пустом меню. Это и есть та петля, ради которой заведена
        // таблица отсрочек.
        BackoffKind DeferKind() const override { return BackoffKind::NothingOffered; }

        bool Useful(Ctx& ctx, Bid const& bid) override
        {
            if (bid.About.What() != Subject::Kind::Unit)
                return false;
            ObjectGuid const giver = bid.About.Guid();
            if (giver.IsEmpty() || QuestLogFull(ctx))
                return false;
            // ВСЁ ЕЩЁ В ОБЗОРЕ И ВСЁ ЕЩЁ СО ЗНАКОМ. Значение фильтрует по флагу квестодателя и
            // по статусу диалога; ушёл из обзора или знак погас — ставка больше не нужна.
            for (GiverInSight const& g : Val<ValueId::GiversInSight>(ctx))
                if (g.Guid == giver)
                    return true;
            return false;
        }

        // ВОЗМОЖНО СЕЙЧАС — ЭТО ВОПРОС К ЯДРУ, А НЕ К РАССТОЯНИЮ. Тот же предикат, что у сдачи.
        bool Possible(Ctx& ctx, Bid const& bid) override
        {
            return ctx.World.CanTalkTo(bid.About.Guid());
        }

        // Ближе — дешевле, тем же наклоном, что у сдачи. Расстояние в значении по плоскости, и
        // это допустимо: здесь оно ЦЕНА, а не решение. Решает ядро в `Possible`.
        float Score(Ctx& ctx, Bid const& bid, float relevance) const override
        {
            ObjectGuid const giver = bid.About.Guid();
            for (GiverInSight const& g : Val<ValueId::GiversInSight>(ctx))
                if (g.Guid == giver)
                    return relevance - g.Dist * YARD_COST;
            return relevance;
        }

        bool Execute(Ctx& ctx, Bid const& bid) override
        {
            ObjectGuid const giver = bid.About.Guid();
            if (giver.IsEmpty() || !ctx.World.CanTalkTo(giver))
                return false;

            // ПРИВЕТСТВИЕ — ЭТО НЕ ВЕЖЛИВОСТЬ, А СПОСОБ УЗНАТЬ МЕНЮ. Ядро строит его в ответ на
            // этот опкод; до него читать нечего.
            if (!ctx.Act.QuestGiverHello(giver))
                return false;

            uint32 quest = ctx.World.BestQuestOffered(&QuestRefusedHere, &ctx);
            if (!quest)
            {
                // ВТОРОЙ ОПКОД, КАК У ЖИВОГО КЛИЕНТА. Существо со своим `OnGossipHello` выходит
                // ДО `PrepareQuestMenu`, и после первого приветствия меню пусто не потому, что
                // предложить нечего, а потому, что его не собирали.
                if (!ctx.Act.GossipHello(giver))
                    return false;
                quest = ctx.World.BestQuestOffered(&QuestRefusedHere, &ctx);
            }

            if (!quest)
            {
                // Предлагать действительно нечего — забываем его на десять минут.
                Defer(ctx, BackoffKind::NothingOffered, bid.About, 0, GIVER_EMPTY_MS);
                return false;
            }
            if (!ctx.Act.AcceptQuest(giver, quest))
                return false;

            // ОТПРАВЛЕННЫЙ ПАКЕТ — ЕЩЁ НЕ ВЗЯТЫЙ КВЕСТ, и дверь честно возвращает только
            // «отправлено». Лестница спрашивает статус после отправки и при неудаче ЗАПОМИНАЕТ
            // квест: так ведут себя требующие подтверждения — сопровождение, общие, — их приём
            // отдельный опкод, до которого дело не дошло (`Constellation.cpp:7556-7574`).
            //
            // Без этой проверки движок счёл бы успехом отправку и снял бы запрет, которого не
            // заслужил (Кодекс, пункт 7).
            if (ctx.World.StatusOf(quest) == QUEST_STATUS_NONE)
            {
                Defer(ctx, BackoffKind::CoreRefused, Subject::OfQuest(quest), 0, QUEST_REFUSED_MS);
                return false;
            }

            // ВЗЯЛИ КВЕСТ — «КУДА ИДТИ ЗА КВЕСТОМ» МОГЛО СТАТЬ НЕПРАВДОЙ. Ответ кэшируется на
            // пять минут, и это цена перебора карты; но взятие — ровно то событие, после
            // которого светофор у выбранной точки меняется. Одна строка вместо пяти минут
            // похода к тому, что уже не нужно (Кодекс, второй проход по шву).
            //
            // ОСТАЛЬНЫЕ СЛУЧАИ ОСТАЮТСЯ, И ЭТО НЕ РЕГРЕСС: лестница запоминает
            // `SeekEntry/SeekSpawn/SeekPos` в момент решения и до прихода не перепроверяет их
            // вовсе. Движок при этом строго лучше — ставка переоценивается каждый такт, так что
            // устаревший поход проигрывает любой настоящей работе, чего режим `SeekingGiver` не
            // умел.
            if (ctx.St)
                ctx.St->Values.GiverToSeek.Invalidate();
            return true;
        }
    };

    // §13 — ПОХОД К КВЕСТОДАТЕЛЮ ПО КАРТЕ. Первое действие движка, которое ХОДИТ.
    //
    // ОГЛЯДЫВАНИЕ НА ХОДУ ЗДЕСЬ НЕ НАПИСАНО, И ЭТО НЕ ПРОПУСК. У лестницы на него уходит
    // отдельная ветка (`Constellation.cpp:3720-3760`): на ходу переспросить обзор и сменить цель
    // на ближнюю, с оговоркой «и не оглядываемся на хвосте». В очереди ставок то же самое
    // происходит само: `TakeQuestNearby` ставит REL_NORMAL на КАЖДОГО квестодателя в обзоре, а
    // поход стоит REL_BACKGROUND минус ярды — значит любой увиденный по дороге перебивает поход
    // тем же тактом, а на хвосте перебивать уже некого. Двадцать строк ветки заменяются
    // порядком двух ставок.
    class SeekGiverByMapAction final : public Action
    {
    public:
        SeekGiverByMapAction() : Action(ActionId::SeekGiverByMap) { }

        // §14 — ПО ВИДУ «УЖЕ СХОДИЛ», потому что именно его ставит это действие. Второй вид,
        // «не дойти», ловится в `Useful` через общую память о точке: `DeferKind` возвращает
        // один, а причин у точки две.
        //
        // РАЗДВОЕНИЕ ЗДЕСЬ НАМЕРЕННОЕ, И ВОТ ЧТО ДЕЛАЕТ ЕГО БЕЗОПАСНЫМ. Предварительный фильтр
        // движка — СОКРАЩЕНИЕ: он снимает ставку до `Useful` и экономит вызов. Решает `Useful`,
        // и он спрашивает `SpawnBackedOffByEngine`, то есть ОБА вида. Значит худшее, что даёт
        // непойманный фильтром `Unreachable`, — лишняя ставка, дожившая до собственной проверки
        // и там отброшенная. Кодекс назвал это латентным; латентно оно ровно до тех пор, пока
        // не написано, кто здесь власть, а кто скорость.
        BackoffKind DeferKind() const override { return BackoffKind::Visited; }

        bool Useful(Ctx& ctx, Bid const& bid) override
        {
            if (bid.About.What() != Subject::Kind::Spawn)
                return false;
            SeekTarget const& t = Val<ValueId::GiverToSeek>(ctx);
            if (!t.Found || uint32(t.SpawnId) != bid.About.Id())
                return false;       // ответ пересчитался и ведёт уже в другое место
            return !SpawnBackedOffByEngine(&ctx, uint32(t.SpawnId));
        }

        // ХОДЬБА ВОЗМОЖНА, ПОКА ЕСТЬ ГДЕ ХРАНИТЬ ЕЁ СОСТОЯНИЕ. Маршрут ядра, место в нём и
        // отступы вбок живут в `EngineState`; без него двигатель начинал бы путь заново каждый
        // такт и никуда бы не пришёл.
        bool Possible(Ctx& ctx, Bid const&) override { return ctx.St != nullptr; }

        // Ближе — дешевле, тем же наклоном, что у сдачи и взятия. Наклон здесь важнее, чем там:
        // поход стоит сотни ярдов, и дальний обязан проигрывать всему, что рядом.
        float Score(Ctx& ctx, Bid const& bid, float relevance) const override
        {
            SeekTarget const& t = Val<ValueId::GiverToSeek>(ctx);
            if (!t.Found || uint32(t.SpawnId) != bid.About.Id())
                return relevance;
            return relevance - ctx.World.DistanceTo2d(t.Where) * YARD_COST;
        }

        bool Execute(Ctx& ctx, Bid const& bid) override
        {
            if (!ctx.St)
                return false;
            SeekTarget const& t = Val<ValueId::GiverToSeek>(ctx);
            if (!t.Found || uint32(t.SpawnId) != bid.About.Id())
                return false;

            float const d = ctx.World.DistanceTo2d(t.Where);
            if (d <= SEEK_ARRIVED_YARDS)
            {
                // ДОШЛИ — И ИМЕННО ЗДЕСЬ СТАВИТСЯ СРОК. Не в неудаче: у лестницы он стоит при
                // удачном приходе тоже, и по её же причине.
                Defer(ctx, BackoffKind::Visited, bid.About, 0, SEEK_VISITED_MS);
                return true;
            }

            float const dt = ctx.Act.SliceSeconds();
            bool const going = WalkTowards(ctx, t.Where, SEEK_ARRIVED_YARDS, dt);

            // ПРИБЛИЖАЕМСЯ ЛИ — ВОПРОС ОТДЕЛЬНЫЙ ОТ «СДЕЛАН ЛИ ШАГ». Двигатель отвечает про шаг,
            // `AdvanceWalk` — про дорогу: расстояние не падает, или идём слишком долго. Отказ
            // двери в шаге — это и есть отсутствие прогресса, отдельной ветки под него нет.
            uint32 const sliceMs = uint32(dt * 1000.0f);
            if (AdvanceWalk(ctx, bid.About, d, sliceMs, !going) != WalkVerdict::Going)
            {
                Defer(ctx, BackoffKind::Unreachable, bid.About, 0, SEEK_UNREACHABLE_MS);
                return false;
            }
            return true;
        }
    };

    // ПОХОД К МЕСТУ ЗАДАНИЯ. Замер 2026-09-11: 15 из 135 расхождений в узле «стою» — лестница
    // «иду к месту задания», движок «идти к квестодателю по карте», потому что этого действия
    // у него не было и он шёл за НОВЫМ квестом вместо цели текущего.
    //
    // ЧЕГО ЗДЕСЬ НЕТ, НАЗВАНО, А НЕ ПРОПУЩЕНО:
    //   * «цель показалась» (`:3900`) — порядок ставок: `KillObjective` стоит REL_HIGH, поход
    //     REL_BACKGROUND, любой увиденный по дороге перебивает поход тем же тактом;
    //   * камень и полёт к далёкой точке (`:3344-3350`) — транспорт движка не написан (0014);
    //   * `BrokenForFight` (`:3338`) — движок не смотрит на состояние экипировки нигде, и обход
    //     целей тоже; ставить проверку одному походу значило бы врать про остальные;
    //   * `TravelCooldownMs = 120000` (`:3947`) — общая пауза походам после неудачи. У движка
    //     неудача откладывает КВЕСТ, а не походы вообще: это другое поведение, и оно измеряется
    //     тенью, как всё, чем очередь отличается от `switch`.
    class TravelToObjectiveAction final : public Action
    {
    public:
        TravelToObjectiveAction() : Action(ActionId::TravelToObjective) { }

        BackoffKind DeferKind() const override { return BackoffKind::Visited; }

        bool Useful(Ctx& ctx, Bid const& bid) override
        {
            if (bid.About.What() != Subject::Kind::Quest)
                return false;
            TravelSpot const& t = Val<ValueId::ObjectiveSpot>(ctx);
            if (!t.Worth || t.QuestId != bid.About.Id())
                return false;       // ответ пересчитался и ведёт уже к другому квесту
            return !QuestTravelBackedOffByEngine(&ctx, t.QuestId);
        }

        bool Possible(Ctx& ctx, Bid const&) override { return ctx.St != nullptr; }

        float Score(Ctx& ctx, Bid const& bid, float relevance) const override
        {
            TravelSpot const& t = Val<ValueId::ObjectiveSpot>(ctx);
            if (!t.Worth || t.QuestId != bid.About.Id())
                return relevance;
            return relevance - ctx.World.DistanceTo2d(t.Where) * YARD_COST;
        }

        bool Execute(Ctx& ctx, Bid const& bid) override
        {
            if (!ctx.St)
                return false;
            TravelSpot const& t = Val<ValueId::ObjectiveSpot>(ctx);
            if (!t.Worth || t.QuestId != bid.About.Id())
                return false;

            // МЕСТО СТАЛО СМЕРТЕЛЬНЫМ, ПОКА МЫ ШЛИ — ПОВОРАЧИВАЕМ (`:3922`). Выбор точки делается
            // на пересчёте, а гибель по дороге меняет ответ между пересчётами.
            if (ctx.Danger.DeadlyToTravelTo(t.Where.GetPositionX(), t.Where.GetPositionY()))
            {
                Defer(ctx, BackoffKind::Visited, bid.About, 0, TRAVEL_VISITED_MS);
                ctx.St->Values.ObjectiveSpot.Invalidate();
                return false;
            }

            float const d = ctx.World.DistanceTo2d(t.Where);
            if (d <= t.Stop + TRAVEL_ARRIVED_SLACK)
            {
                // ПРИШЛИ, А ЦЕЛЕЙ НЕТ — ВЫХОД, а не стояние до срока (`:3907`): будь цель в
                // обзоре, ставка боя уже перебила бы поход. Область пуста — выбита или её
                // наполняет скрипт волнами; откладываем этот квест, пересчёт даст следующий.
                Defer(ctx, BackoffKind::Visited, bid.About, 0, TRAVEL_VISITED_MS);
                ctx.St->Values.ObjectiveSpot.Invalidate();
                return true;
            }

            float const dt = ctx.Act.SliceSeconds();
            bool const going = WalkTowards(ctx, t.Where, t.Stop, dt);
            uint32 const sliceMs = uint32(dt * 1000.0f);
            if (AdvanceWalk(ctx, bid.About, d, sliceMs, !going) != WalkVerdict::Going)
            {
                // «до места задания не дойти» / «полминуты без приближения» / «в пути слишком
                // долго» — три исхода лестницы, один ключ: сам квест (`:3953`).
                Defer(ctx, BackoffKind::Unreachable, bid.About, 0, TRAVEL_UNREACHABLE_MS);
                ctx.St->Values.ObjectiveSpot.Invalidate();
                return false;
            }
            return true;
        }
    };

    // §9 — СТРАТЕГИЯ СТАВИТ КАЖДЫЙ ТАКТ, а не по событию. Так задумано: ставка живёт один такт,
    // и «есть что сдать» — это состояние, а не происшествие. Триггер понадобился бы, если бы
    // нужно было поймать МОМЕНТ; здесь нужно постоянное присутствие в очереди.
    class QuestsStrategy final : public Strategy
    {
    public:
        QuestsStrategy() : Strategy(StrategyId::Quests) { }

        void DefaultBids(Ctx& ctx, BidSink& sink) const override
        {
            // ГОТОВОЕ СДАТЬ — ВЫШЕ ОБЫЧНОГО. Оно закрывает работу, которая уже сделана, и
            // освобождает слот журнала; отложить его значит носить законченное.
            // БЕЗ ОТСЕВА ПО РАССТОЯНИЮ: ставится всё готовое, а порядок решает цена ниже.
            // Прибор, который это выяснил, ушёл вместе с потолком — он и был написан до ответа.
            // СДАЧА УСТУПАЕТ ОТДЫХУ ТОЛЬКО ПОСЛЕ ПОДЪЁМА. В `Idle` лестница сдаёт ПРЕЖДЕ отдыха
            // (`FindTurnIn` `:3125`, «надо перевести дух» `:3148`) — там ничья с отдыхом решается
            // как и была; а после воскрешения она держит `Recovering` по событию и не сдаёт, пока
            // не восстановится (постановление Мастера 2026-09-12, п. 3).
            // ТЕМ ЖЕ ВОПРОСОМ, ЧТО И БОЙ: `RestWanted` читает и запрет на отдых, который ставит его
            // потолок, — иначе после «отдых не помогает, иду как есть» бой открылся бы, а сдача нет
            // (Кодекс, второй проход).
            bool const restingAfterRevive = ctx.St && ctx.St->RestAfterRevive && RestWanted(ctx);
            if (!restingAfterRevive)
                for (TurnInCandidate const& t : Val<ValueId::CompletedTurnIns>(ctx))
                    sink.Add(ActionId::TurnInQuest, REL_HIGH, Subject::OfQuest(t.QuestId));

            // ВЗЯТИЕ НИЖЕ СДАЧИ, И ЭТО НЕ ВКУСОВЩИНА. Сдача закрывает уже сделанную работу и
            // освобождает слот журнала; взятие только добавляет работы. При обеих доступных
            // выигрывает сдача — ровно так же поступил бы игрок с полным журналом.
            // lazy: ставка на КАЖДОГО квестодателя в обзоре, а `Useful`/`Score` ищут его в том
            // же списке линейно — на такте это квадратично по числу видимых квестодателей.
            // Кодекс назвал предел: на восьми спутниках это ничто, на 122 в плотном хабе нужен
            // замер, а возможно и указатель вместо перебора. Очередь ставок ограничена 32, и
            // перебор считается полем `сброшено` в строке решения — по нему и станет видно.
            if (!QuestLogFull(ctx))
                for (GiverInSight const& g : Val<ValueId::GiversInSight>(ctx))
                    sink.Add(ActionId::TakeQuestNearby, REL_NORMAL, Subject::OfUnit(g.Guid));

            // ПОХОД ПО КАРТЕ — САМОЕ НИЖНЕЕ, ЧТО МОЖНО ДЕЛАТЬ ПО КВЕСТАМ, и у лестницы он ровно
            // там же: последняя ветка `Idle`, куда доходит тот, у кого нет ни готового к сдаче,
            // ни цели, ни собеседника. REL_BACKGROUND значит «когда больше нечем заняться»; всё
            // остальное в этой стратегии стоит выше по построению, а не по проверке условий.
            // ПОХОД К МЕСТУ ЗАДАНИЯ ВЫШЕ ПОХОДА ЗА НОВЫМ КВЕСТОМ — ОТНОШЕНИЕМ, А НЕ ЧИСЛОМ.
            // У лестницы это `if (FindObjectiveSpot) … else FindGiverByMap` (`:3340`, `:3383`):
            // пока есть куда идти за целью текущего квеста, карту за новым она не перебирает
            // вовсе. Две ставки на одном REL_BACKGROUND минус ярды решал бы ближний, и ближний
            // квестодатель уводил бы от цели — ровно те 15 расхождений, ради которых это
            // действие написано.
            // НАЧАТЫЙ ПОХОД ПО КАРТЕ ИДЁТ ДО ПРИХОДА — раньше любых ворот и раньше похода к месту
            // задания: у лестницы `SeekingGiver` не прерывается ни взятым по дороге квестом, ни
            // появившимся местом задания (`:3380-3394`); ставится по `Running`, как отдых (Кодекс,
            // проходы 1–2). Его конец — приход или отказ самого действия.
            if (ctx.St && ctx.St->Running == ActionId::SeekGiverByMap)
            {
                SeekTarget const& seek = Val<ValueId::GiverToSeek>(ctx);
                if (seek.Found)
                {
                    sink.Add(ActionId::SeekGiverByMap, REL_BACKGROUND,
                             Subject::OfSpawn(uint32(seek.SpawnId)));
                    return;
                }
            }

            TravelSpot const& spot = Val<ValueId::ObjectiveSpot>(ctx);
            if (spot.Worth)
                sink.Add(ActionId::TravelToObjective, REL_BACKGROUND, Subject::OfQuest(spot.QuestId));
            // ПО КАРТЕ ЗА НОВЫМ КВЕСТОМ — ТОЛЬКО КОГДА НЕЗАКРЫТЫХ ЦЕЛЕЙ НЕТ ВОВСЕ. Ворота лестницы
            // (`Constellation.cpp:3357-3358`: `!unmetNow && TalkCandidate.IsEmpty()`), которые
            // разбор 2026-09-12 оставил на суд живого окна как «стояние хуже похода». Окно
            // рассудило числом: за час вживую 51 решение «по карте», боёв вчетверо меньше
            // (63 против ~300), сдач втрое, гибелей больше — состав ушёл через зоны за новыми
            // квестами, бросив цели рядом. Лестница с этими воротами бьёт то, что рядом, и
            // стоит лишь тогда, когда бить и правда некого. Отношение переносится как есть;
            // счётчик — значение с интервалом обхода лестницы. Ворота — на НАЧАЛО похода.
            else if (Val<ValueId::UnmetObjectives>(ctx) == 0
                     && Val<ValueId::Objectives>(ctx).Talk.IsEmpty())
            {
                SeekTarget const& seek = Val<ValueId::GiverToSeek>(ctx);
                if (seek.Found)
                    sink.Add(ActionId::SeekGiverByMap, REL_BACKGROUND,
                             Subject::OfSpawn(uint32(seek.SpawnId)));
            }
        }
    };
}

namespace Constellation::Ai
{
    void RegisterQuestActions(Engine& engine)
    {
        engine.Register(std::make_unique<TurnInQuestAction>());
        engine.Register(std::make_unique<TakeQuestNearbyAction>());
        engine.Register(std::make_unique<SeekGiverByMapAction>());
        engine.Register(std::make_unique<TravelToObjectiveAction>());
        engine.Register(std::make_unique<QuestsStrategy>());
    }
}
