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

    // Дальше этого сдача не рассматривается вовсе: не потому что дойти нельзя, а потому что
    // такая цель на порядок дороже всего остального в ветке, и её место — в походе по карте,
    // который придёт своим шагом.
    inline constexpr float TURNIN_MAX_YARDS = 600.0f;

    // §14 — НА СКОЛЬКО ОТКЛАДЫВАЕТСЯ СДАЧА, У КОТОРОЙ ПРИНИМАЮЩИЙ ПРОПАЛ. Пять секунд: за них
    // спутник успевает сделать шаг, а движок — заняться другим. Дольше держать нечего, условие
    // здесь меняется движением, а не временем.
    inline constexpr uint32 TURNIN_RETRY_MS = 5000;

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
        // ядро. Теперь отвечает `CanInteractWithQuestGiver`, а сорок ярдов лишь ограничивают
        // обход сетки — столько же, сколько берёт ветка `TurningIn` на той же работе.
        static constexpr float GIVER_SEARCH_YARDS = 40.0f;

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
            uint32 const quest = bid.About.Id();
            for (TurnInCandidate const& t : Val<ValueId::CompletedTurnIns>(ctx))
            {
                if (t.QuestId != quest)
                    continue;
                if (!t.EnderEntry)
                    return true;
                return ctx.World.NearestQuestGiverOfEntry(t.EnderEntry, GIVER_SEARCH_YARDS).has_value();
            }
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

            for (TurnInCandidate const& t : Val<ValueId::CompletedTurnIns>(ctx))
            {
                if (t.QuestId != quest)
                    continue;

                // САМОСДАЧА — ПУСТОЙ ПРИНИМАЮЩИЙ, и дверь выводит это сама (ClientAct.h:83):
                // «сдать самому себе» и «это скриптовая сдача» — одно и то же условие, поэтому
                // оно не предлагается параметром, который вызывающий мог бы опровергнуть.
                if (!t.EnderEntry)
                    return ctx.Act.CompleteQuest(ObjectGuid::Empty, quest);

                // ДОЙТИ ДО НЕГО — ОТДЕЛЬНАЯ РАБОТА, И ОНА НЕ ЗДЕСЬ. Пока принимающий не в
                // радиусе разговора, эта ставка ничего не исполняет; дорогу даст своё действие
                // на следующем шаге переноса. Возвращаем false честно: движок толкнёт
                // альтернативы, а не сделает вид, что сдал.
                //
                // lazy: сдача без дороги. Как появится действие «идти к принимающему»
                // (план, шаг 14), здесь останется только сама сдача, а движение уйдёт в
                // Continuers этой же ставки.
                // ТЕПЕРЬ ЭТО ДОКАЗАТЕЛЬСТВО, А НЕ ДОГАДКА: поиск отдаёт только тех, кому
                // ядро само разрешило сдавать. Проверка всё равно повторяется здесь, а не
                // берётся из `Possible`: между выбором и исполнением проходит время, и
                // принимающий успевает умереть, взлететь или уйти.
                std::optional<ObjectGuid> const ender =
                    ctx.World.NearestQuestGiverOfEntry(t.EnderEntry, GIVER_SEARCH_YARDS);
                if (!ender)
                {
                    // §14 — И ВОТ ЗДЕСЬ ПЕТЛЯ, РАДИ КОТОРОЙ ЗАВЕДЕНА ТАБЛИЦА. Альтернатив у
                    // этого действия нет, поэтому ставка вернулась бы чуть ниже и была бы
                    // выбрана СЛЕДУЮЩИМ ЖЕ тактом — и так, пока принимающий не появится.
                    // Пять секунд это пауза, а не наказание: спутник в это время идёт.
                    Defer(ctx, BackoffKind::Unreachable, bid.About, 0, TURNIN_RETRY_MS);
                    return false;
                }
                return ctx.Act.CompleteQuest(*ender, quest);
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
            if (giver.IsEmpty())
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
            for (TurnInCandidate const& t : Val<ValueId::CompletedTurnIns>(ctx))
            {
                if (t.Dist > TURNIN_MAX_YARDS)
                    continue;
                sink.Add(ActionId::TurnInQuest, REL_HIGH, Subject::OfQuest(t.QuestId));
            }

            // ВЗЯТИЕ НИЖЕ СДАЧИ, И ЭТО НЕ ВКУСОВЩИНА. Сдача закрывает уже сделанную работу и
            // освобождает слот журнала; взятие только добавляет работы. При обеих доступных
            // выигрывает сдача — ровно так же поступил бы игрок с полным журналом.
            // lazy: ставка на КАЖДОГО квестодателя в обзоре, а `Useful`/`Score` ищут его в том
            // же списке линейно — на такте это квадратично по числу видимых квестодателей.
            // Кодекс назвал предел: на восьми спутниках это ничто, на 122 в плотном хабе нужен
            // замер, а возможно и указатель вместо перебора. Очередь ставок ограничена 32, и
            // перебор считается полем `сброшено` в строке решения — по нему и станет видно.
            for (GiverInSight const& g : Val<ValueId::GiversInSight>(ctx))
                sink.Add(ActionId::TakeQuestNearby, REL_NORMAL, Subject::OfUnit(g.Guid));
        }
    };
}

namespace Constellation::Ai
{
    void RegisterQuestActions(Engine& engine)
    {
        engine.Register(std::make_unique<TurnInQuestAction>());
        engine.Register(std::make_unique<TakeQuestNearbyAction>());
        engine.Register(std::make_unique<QuestsStrategy>());
    }
}
