/*
 * Constellation — what an action is allowed to know, and how it is handed the world.
 *
 * Contract: homelab/.agent/design/constellation-engine/engine-spec-v2.md §6′ ("Ctx does NOT hand
 * an action a mutable Player* or Companion& — otherwise the type proves nothing").
 *
 * WHY A READ FACADE AND NOT A `Player const*`. The obvious design is to pass a const pointer and
 * let const-correctness carry the rule. It does not: `Player.h:2214` declares
 * `WorldSession* GetSession() const` — a const method returning a MUTABLE session — so a const
 * pointer is a straight route to every packet handler in the core. That is not a hypothetical;
 * it is how the first version of ClientAct was still wide open while claiming to be a door.
 *
 * So reads get a facade too. WorldView holds the Player privately and answers questions; it never
 * hands the object out. The set of questions grows on demand, and that is deliberate — each one
 * added is a decision someone made, not a capability that leaked in.
 *
 * Copyright (C) 2026 Constellation contributors. Licensed under the GNU AGPL v3 — see COPYING.
 */
#ifndef CONSTELLATION_ENGINE_CONTEXT_H
#define CONSTELLATION_ENGINE_CONTEXT_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "QuestDef.h"
#include <optional>

class Player;

namespace Constellation::Ai
{
    class ClientAct;
    struct EngineState;      // §13 — только вперёд: знать его здесь значило бы цикл включений


    // -----------------------------------------------------------------------------------------
    // §12 — ЧТО МИР ОТДАЁТ ОБХОДАМИ. Ни одного указателя в мир: значение кэшируется на
    // секунду, а `Creature*`, проживший секунду, — это висячий указатель, ждущий выгрузки
    // клетки. Гуид разрешается заново в момент использования — точно так же, как это делает
    // настоящий клиент, который тоже держит гуид, а не адрес.
    // -----------------------------------------------------------------------------------------

    // Квест в моём журнале, готовый к сдаче, и место, куда его нести.
    struct TurnInCandidate
    {
        uint32   QuestId    = 0;
        // 0 = сдать САМОМУ СЕБЕ. Это НЕ «принимающий не найден», а собственное правило
        // ядра: `HandleQuestgiverCompleteQuest` пропускает самосдачу только при
        // `QUEST_FLAGS_AUTO_COMPLETE` (Constellation.cpp:7557-7561). Догадка «нет строки
        // в creature_questender — значит самосдача» ошибается вчетверо: 19 868 против 4 733.
        uint32   EnderEntry = 0;
        Position Where;
        float    Dist       = 0.0f;
        bool     FromTable  = false;    // позиция из указателя точек, а не от живого существа
    };

    // Квестодатель, которого ядро сейчас считает выдающим мне что-то прямо сейчас.
    struct GiverInSight
    {
        ObjectGuid Guid;
        uint32     Entry   = 0;
        float      Dist    = 0.0f;      // по плоскости, от меня
        bool       Visible = false;     // IsWithinLOSInMap — НЕ фильтр, а сведение
    };

    // §31 — СОСТОЯНИЕ ХОДЬБЫ. Шестнадцать полей, которые лестница копила по одному после
    // каждого случая, когда спутник вставал: маршрут ядра и место в нём, отступы вбок, отсрочка
    // построителя пути, обнаружение примерзания. Комментарии переехали ДОСЛОВНО — в них причина,
    // а не описание.
    //
    // ЗАЧЕМ ОТДЕЛЬНЫЙ ТИП: движку предстоит ходить ТЕМ ЖЕ механизмом, а не похожим. Написанный
    // заново он дал бы спутника, который ходит в стены, — ровно то, из-за чего каждое из этих
    // полей и появилось.
    struct MoveState
    {
        bool Moving = false;                // мы САМИ считаем, идём ли: флаги ядра могут быть нормализованы
        std::vector<Position> Waypoints;    // маршрут, построенный ядром
        size_t WaypointIndex = 0;
        float PathTargetX = 0.0f, PathTargetY = 0.0f;
        uint32 LastPathType = 0;            // тип последнего отказа построителя — для строки «не подойти»
        uint32 FrozenMs = 0;                // сколько стоим под запретом движения
        bool FrozenNoted = false;           // и сказали ли об этом хоть раз
        float LastX = 0.0f, LastY = 0.0f;   // где мы были — чтобы заметить, что не идём
        uint32 StuckMs = 0;                 // сколько стоим, хотя собирались идти
        uint32 UnstickTries = 0;            // сколько раз отступали вбок подряд
        uint32 NoPathMs = 0;                // сколько ещё не трогать построитель маршрута
        uint8 NoPathFails = 0;              // подряд идущих отказов — отступ растёт с ними
        bool RawTarget = false;             // боковая точка не далась — идём на самого NPC
        bool Stalled = false;               // отступать больше некуда — решает автомат
    };

    // КУДА ИДТИ И ЗА ЧЕМ. Ответ выбора по указателю карты — один, а не список: маршрут
    // ходячего NPC разрешается уже для ВЫБРАННОГО, и лестница тоже выбирает одного.
    // `Where` — не точка спавна, а место, к которому идти: у патрулирующего это ближайший
    // узел его маршрута на нашем ярусе.
    struct SeekTarget
    {
        uint32              Entry   = 0;
        ObjectGuid::LowType SpawnId = 0;
        Position            Where;
        uint32              QuestId = 0;
        bool                Found   = false;
    };

    // Квестодатель из указателя карты, загружен он сейчас или нет.
    struct GiverOnMap
    {
        uint32              Entry   = 0;
        ObjectGuid::LowType SpawnId = 0;
        Position            Where;
        float               Dist    = 0.0f;
    };

    // Посетители — указатели на функцию с контекстом, а не `std::function`: второе аллоцирует,
    // а весь смысл поправки 9 плана в том, чтобы путь пересчёта не выделял памяти вовсе.
    using TurnInVisitor     = void (*)(void* user, TurnInCandidate const& t);
    using GiverSightVisitor = void (*)(void* user, GiverInSight const& g);
    // ПАМЯТЬ ОБ ОТКАЗАХ ПРИХОДИТ ОТ ВЫЗЫВАЮЩЕГО, потому что принадлежит ему, а не миру.
    // Лестница передаёт свой набор, движок — свою таблицу отсрочек. `nullptr` значит
    // «памяти нет», и это НЕ безобидно: без неё квест, который не взялся, будет выбран
    // снова следующим тактом, и так навсегда.
    using QuestRefusedFn = bool (*)(void const* user, uint32 questId);

    // К ЭТОЙ ТОЧКЕ ПОКА НЕ ХОДИМ — вторая половина памяти механизма, рядом с первой.
    using SpawnBackoffFn = bool (*)(void const* user, uint32 spawnId);

    // ПАМЯТЬ МЕХАНИЗМА ПРИ ВЫБОРЕ, КУДА ИДТИ — одним типом, а не пятью параметрами подряд.
    //
    // Граница здесь та же, что и в трёх предыдущих лифтах: что разрешает МИР — общее, что этот
    // механизм уже пробовал и не смог — его собственное. У лестницы это наборы внутри спутника,
    // у движка — таблица отсрочек. Тип нужен потому, что параметров стало пять: подряд идущие
    // указатели одного вида — приглашение перепутать их местами на месте вызова, и компилятор
    // такую перестановку не заметит.
    struct SeekMemory
    {
        QuestRefusedFn Refused     = nullptr;   // этот КВЕСТ пробовали и не вышло
        void const*    RefusedUser = nullptr;
        // ВНИМАНИЕ: `nullptr` ЗДЕСЬ — НЕ «памяти нет», А «ЗАЩИТЫ НЕТ». Без него выбор будет
        // возвращать одну и ту же точку, к которой уже не дошли, — столько раз, сколько его
        // спросят. Три остальных нуля безобидны, этот нет (Кодекс, пункт 2). Движку сюда
        // передавать свою таблицу отсрочек, а не оставлять значение по умолчанию.
        SpawnBackoffFn BackedOff   = nullptr;   // к этой ТОЧКЕ пока не ходим
        void const*    BackoffUser = nullptr;

        // Одноразовая строка «за потолком N точек, ближайшая в M ярдах». `nullptr` значит «не
        // писать»: она нужна оператору, чтобы решать про потолок, и у движка своего решения
        // об этом нет.
        bool*          DiagOnce    = nullptr;
    };

    using GiverIndexVisitor = void (*)(void* user, GiverOnMap const& g);

    // Everything an action may ask about the world, and nothing else.
    class WorldView
    {
    public:
        explicit WorldView(Player const* self) : _self(self) { }

        // -- identity ----------------------------------------------------------------------
        ObjectGuid  Guid() const;
        char const* Name() const;
        uint8       Level() const;
        uint8       Class() const;
        uint8       Race() const;

        // -- where ------------------------------------------------------------------------
        Position Where() const;
        uint32   MapId() const;
        uint32   ZoneId() const;
        uint32   AreaId() const;
        float    DistanceTo(Position const& pos) const;
        float    DistanceTo2d(Position const& pos) const;
        // NOT a float: "not found" is not "right here". An unresolvable guid used to come back
        // as 0.0f, which satisfies every range and arrival check a caller could write.
        std::optional<float> DistanceTo(ObjectGuid guid) const;
        bool     CanSee(ObjectGuid guid) const;

        // -- condition ---------------------------------------------------------------------
        bool  IsAlive() const;
        bool  IsInCombat() const;
        bool  IsMounted() const;
        bool  IsFlying() const;
        bool  IsInWater() const;
        float HealthPct() const;
        bool  HasAttackers() const;

        // -- the quest log -----------------------------------------------------------------
        QuestStatus StatusOf(uint32 questId) const;
        bool        IsRewarded(uint32 questId) const;
        uint8       FreeQuestSlots() const;
        // TWO DIFFERENT QUESTIONS, and conflating them is a defect. Eligibility is what a
        // giver's menu is built from; permission additionally needs log space and bag space.
        bool        IsEligibleFor(uint32 questId) const;   // CanTakeQuest — would it be offered
        bool        MayAccept(uint32 questId) const;       // + SatisfyQuestLog + CanAddQuest

        // -- the bags ----------------------------------------------------------------------
        uint32 FreeBagSlots() const;

        // -- обходы мира (§12) ---------------------------------------------------------
        //
        // Здесь стояло «anything that scans the world … those are Values with intervals».
        // Это остаётся верным и теперь выполнено, а не обещано: обход — здесь, частота —
        // у `Value`. Пер-тактовый обход сетки однажды увёл мировой поток на 99 % ядра
        // (Constellation.cpp:2786), и именно поэтому звать их напрямую из действия нельзя.
        //
        // ВСЁ ЛИЧНОЕ ОСТАЁТСЯ СНАРУЖИ. Чёрные списки, отсрочки и предпочтения живут на
        // `Companion`, а `Value::Calculate` их не видит и не должно видеть: иначе кэш
        // протухал бы от того, что у кого-то сменилась отсрочка. Фильтрует действие.

        // Журнал заданий: всё готовое к сдаче, с местом, куда нести. Сетку не обходит.
        void ForEachCompletedTurnIn(TurnInVisitor visit, void* user) const;

        // ОДИН обход сетки. Отсеивает мёртвых и тех, у кого ядро не видит ЧТО ПРЕДЛОЖИТЬ
        // прямо сейчас. Маска — та же, что у `NearestQuestGiver` (Constellation.cpp:11566-11574), и
        // `Future` из неё сознательно исключён: спутник дорастёт и вернётся сам. Проверка
        // «не None» вместо маски — главная ошибка всей этой ветки: 56 спутников из 122 шли к
        // ПРИНИМАЮЩЕМУ их же текущего квеста и брали там ноль.
        void ForEachQuestGiverInRange(float range, GiverSightVisitor visit, void* user) const;

        // Указатель карты, построенный ОДИН раз при загрузке (`Manager::_givers`,
        // Constellation.cpp:7786-7797). Сетку не трогает вовсе — это чтение таблицы, и именно
        // поэтому ярус «на карту» не понадобился: таблица уже общая для всех.
        void ForEachGiverOnMap(float maxDist, GiverIndexVisitor visit, void* user) const;

        // МОГУ ЛИ Я ГОВОРИТЬ ИМЕННО С ЭТИМ. Тот же предикат ядра, что и у сдачи, только по
        // гуиду: `CanInteractWithQuestGiver` знает и расстояние, и флаг квестодателя, и смерть,
        // и полёт. Своей мерки близости здесь нет и не будет — она уже стоила 916 кругов.
        bool CanTalkTo(ObjectGuid unit) const;

        // ЧТО ВЗЯТЬ ИЗ МЕНЮ, КОТОРОЕ ЯДРО ТОЛЬКО ЧТО СОБРАЛО. Ноль значит «нечего».
        //
        // Зовёт ТУ ЖЕ политику, что и лестница (`PickFromQuestMenu`), поэтому выбор совпадает
        // по построению, а не по совпадению правил, написанных дважды. Памяти об отказах у
        // движка своей нет: она в таблице отсрочек и проверяется на уровне ставки, до вызова.
        //
        // ПОРЯДОК ОБЯЗАТЕЛЕН: меню действительно только сразу после приветствия и только для
        // того существа, которому оно послано. Это состояние прошлого разговора, а не запрос,
        // поэтому читатель не берёт гуид — брать его значило бы обещать выборку, которой нет.
        uint32 BestQuestOffered(QuestRefusedFn refused, void const* user) const;

        // КУДА ИДТИ ЗА СЛЕДУЮЩИМ КВЕСТОМ. Зовёт ту же функцию, что и лестница, со всей её
        // начинкой: потолок дальности, ближний порог, фракция, кэш по ВИДУ существа, проверка
        // яруса и разрешение маршрута патрулирующего. Память передаётся вызывающим.
        bool GiverToWalkTo(SeekMemory const& mem, SeekTarget* out) const;

        // ПРИНИМАЮЩИЙ ЭТОГО ВИДА, ДО КОТОРОГО ЯДРО РАЗРЕШАЕТ ДОТЯНУТЬСЯ.
        //
        // Значения отдают ВИД и точку (указатель карты знает только их), а дверь требует
        // гуид — как и клиент, который шлёт гуид того, по кому щёлкнул. Перевод одного в
        // другое делается в момент действия, а не кэшируется: `Creature*`, проживший секунду,
        // — это висячий указатель, ждущий выгрузки клетки.
        //
        // `searchDist` — ГРАНИЦА ОБХОДА, А НЕ РЕШЕНИЕ О БЛИЗОСТИ. Своя мерка здесь уже стоила
        // 916 кругов и ноль сдач (Constellation.cpp:3984-3998): по плоскости 5.8 ярда, в
        // пространстве 10.7, ядру нужно 5. Поэтому «достаточно ли близко» отвечает
        // `CanInteractWithQuestGiver`, а этот радиус лишь ограничивает обход сетки.
        std::optional<ObjectGuid> NearestQuestGiverOfEntry(uint32 entry, float searchDist) const;

        // NOT HERE, and not by omission:
        //   Player const* / Player& — see the header comment; this is the whole point.
        //   А mutable anything. Фасад читает; пишет только `ClientAct`.

    private:
        Player const* _self;
    };

    // -----------------------------------------------------------------------------------------
    // ЧТО ДВИЖОК ПРОСИТ У МОДУЛЯ, А НЕ ЗНАЕТ САМ.
    //
    // Указатели карты — `_givers`, `_spawns` — живут на `Manager` внутри `Constellation.cpp`,
    // который движку не виден и виден быть не должен: зависимость туда превратила бы
    // движок в часть модуля. Поэтому движок ОБЪЯВЛЯЕТ, что ему нужно, а модуль это
    // определяет — тот же шов, только в другую сторону, и ровно две функции шириной.
    // -----------------------------------------------------------------------------------------

    // Ближайшая известная точка появления этого вида на этой карте. false = не знаем
    // ни одной, что НЕ то же, что «его нет в мире»: призываемых в таблице точек нет.
    bool NearestSpawnOf(uint32 mapId, uint32 entry, Position const& from,
                        Position* outWhere, float* outDist);

    // Обход указателя квестодателей этой карты в пределах `maxDist` от `from`.
    void VisitGiverIndex(uint32 mapId, Position const& from, float maxDist,
                         GiverIndexVisitor visit, void* user);

    // Радиусы — НАСТРОЙКА МОДУЛЯ, И У НЕЁ ОДИН ВЛАДЕЛЕЦ. Движок не читает `Cfg()` —
    // он её не видит, и правильно: собственная копия числа разошлась бы с конфигом при
    // первом же `.reload config`, и никто бы не заметил.
    struct EngineTuning
    {
        float QuestGiverRange = 0.0f;   // обзор вокруг себя
        float GiverSeekRange  = 0.0f;
        // Потолок на одну дорогу. Читается у конфига каждый раз, как и радиусы: `.reload config`
        // меняет его на живом мире.
        uint32 WalkCapMs      = 900000;   // как далеко готовы идти по указателю
    };
    EngineTuning Tuning();

    // Политика выбора квеста из меню — одна на лестницу и на движок. Переходник, а не копия.
    uint32 BestQuestInMenu(Player const* self, QuestRefusedFn refused, void const* user);

    // Тот же приём: одна реализация в модуле, переходник для движка.
    bool FindGiverToWalkTo(Player const* self, SeekMemory const& mem, SeekTarget* out);

    // §6′ — what an action receives. One timestamp for the whole tick so two values cannot
    // disagree about "now"; one read facade; one write door; nothing else.
    struct Ctx
    {
        WorldView const& World;
        ClientAct&       Act;
        uint32           NowMs;

        // §13 — ОТКУДА ДЕЙСТВИЕ ЧИТАЕТ ЗНАЧЕНИЕ.
        //
        // На шаге 12 значения написали и проверили в `Seal()`, но не провели к читателю:
        // `Value<T>::Get` требует слот, слот лежит в `EngineState`, а `DefaultBids(Ctx&, BidSink&)`
        // и `Score(Ctx&, ...)` видят только `Ctx`. Механизм был, провода не было; всплыло при
        // первом же чтении, то есть ровно тогда, когда это вообще могло всплыть.
        //
        // УКАЗАТЕЛЬ, А НЕ ССЫЛКА: ноль значит «значения недоступны», и читающая
        // функция обязана это проверить, а не разыменовать.
        EngineState*     St = nullptr;
    };
}

#endif // CONSTELLATION_ENGINE_CONTEXT_H
