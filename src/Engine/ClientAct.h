/*
 * Constellation — the one door through which an action may touch the world.
 *
 * Contract: homelab/.agent/design/constellation-engine/engine-spec-v1.md §6, as amended by v2
 * §6′ and v3. Three Codex passes, the last one BUILD THE FIRST CUT AS WRITTEN.
 *
 * INVARIANT 0, MADE STRUCTURAL. Until now "companions act only through client-equivalent
 * opcodes" was a rule people follow. Here it is a type: an Action receives a ClientAct& and has
 * no other way to affect the world. Reading core state stays free — a real client learns the
 * same facts through SMSG — but WRITING has exactly one door, and Player::TeleportTo, AddQuest,
 * RewardQuest and KilledMonsterCredit are not on it and will not be added. If a behaviour cannot
 * be expressed here, that is a finding to report, not a reason to widen the door.
 *
 * Every method names the opcode it sends. The bodies are MOVED from Constellation.cpp, not
 * invented: each field name below was read at the call site it came from, because writing a
 * packet's fields from memory is how a build cycle gets wasted.
 *
 * The audit `F:\core\constellation\audit_writes.py` is the gate: it enumerates every non-read
 * call on a world object, and anything outside ClientAct.cpp is a finding. Its measured baseline
 * today is 41 opcode dispatches, 44 CMSG packets and exactly three internal writes —
 * SetFacingToObject twice, DurabilityRepairAll once, and CombatStop once, the last being the
 * only one in the whole module with no client packet at all.
 *
 * Copyright (C) 2026 Constellation contributors. Licensed under the GNU AGPL v3 — see COPYING.
 */
#ifndef CONSTELLATION_ENGINE_CLIENTACT_H
#define CONSTELLATION_ENGINE_CLIENTACT_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"
#include <set>

class Player;
class WorldSession;

namespace Constellation::Ai
{
    // ЛУТ — СЛОВАРЬ ДВЕРИ, потому что лут это протокол из четырёх отправок, а не одна.
    //
    // Замер 2026-09-11, движок вживую пятнадцать минут: его бой не лутил — предметы заданий не
    // собирались, квесты не закрывались, а занятость при этом читалась «провёл 95,7 %». Исход
    // боя у лестницы живёт в `LootFromCorpse`/`TakeOpenLoot`, которые шлют прямо в сессию.
    // Теперь их тело одно (`LootFromCorpseCore`), а КТО ШЛЁТ — параметр: лестница над сессией,
    // движок над этой дверью. Что брать и сколько влезет — политика, и она в теле, не здесь.
    //
    // ОДНА ЗАЯВКА — ГУИД ОБЪЕКТА ЛУТА И НОМЕР В ЕГО СПИСКЕ, ровно то, что несёт
    // `WorldPackets::Loot::LootRequest`; пакет вмещает сто (`Array<LootRequest, 100>`), и это
    // предел самого пакета, не выдуманный.
    struct LootPick
    {
        ObjectGuid Object;
        uint8      LootListId = 0;
    };
    inline constexpr uint32 LOOT_PICK_CAP = 100;

    // ЧЕТЫРЕ ОТПРАВКИ ЧЕРЕЗ УКАЗАТЕЛИ С КОНТЕКСТОМ, не `std::function` — та аллоцирует, а лут
    // хоть и не на такте, но той же дисциплины. Собраны в структуру с именованными полями:
    // `Open` и `Release` одной сигнатуры, и позиционно их можно перепутать так, что компилятор
    // не заметит, — по имени нельзя.
    using LootOpenFn    = bool (*)(void* user, ObjectGuid unit);
    using LootMoneyFn   = bool (*)(void* user);
    using LootItemsFn   = bool (*)(void* user, LootPick const* picks, uint32 count);
    using LootReleaseFn = bool (*)(void* user, ObjectGuid unit);
    struct LootSender
    {
        LootOpenFn    Open    = nullptr;
        LootMoneyFn   Money   = nullptr;
        LootItemsFn   Items   = nullptr;
        LootReleaseFn Release = nullptr;
        void*         User    = nullptr;
    };

    // СЧЁТ ЗА ВСЁ ВРЕМЯ — у лестницы это были пять полей `Companion`, у движка будут поля
    // `EngineState`; тело пишет в одну структуру, не зная чью.
    struct LootCounters
    {
        uint32 Opened = 0;      // открыли трупов
        uint32 Items  = 0;      // взяли предметов (лёгших, не запрошенных)
        uint32 Money  = 0;      // взяли денег (в медяках)
        uint32 TooFar = 0;      // не дотянулись — мера нужды в ходьбе
        uint32 Denied = 0;      // ядро не дало (чужой лут, розыгрыш, пустой вид)
    };

    // КАСТ — ТА ЖЕ ФОРМА, ЧТО У ЛУТА: тело одно (`CastAtTargetCore`), кто шлёт — параметр. Две
    // отправки: сам запрос каста и остановка перед читаемым заклинанием (у лестницы это её
    // мовер, у движка — дверь `StopMoving`). Выбор заклинания, откаты и «стоя ли читаем» —
    // политика, она в теле и читает игрока; дверь только строит и шлёт.
    using CastSpellFn = bool (*)(void* user, uint32 spellId, ObjectGuid target);
    using CastStopFn  = bool (*)(void* user);
    struct CastSender
    {
        CastSpellFn Cast = nullptr;
        CastStopFn  Stop = nullptr;
        void*       User = nullptr;
    };

    // ПАМЯТЬ КАСТА — семь полей, которые у лестницы лежали в `Companion` порознь. `LastSpell` —
    // ротации нужно «что реально ушло»; `WasCasting` читает ИСХОД боя (`CastsDiedUnder`), потому
    // он здесь, а сам счётчик исхода — нет. `SpellsLogged` — прибор «каждая пара спутник +
    // заклинание один раз за всё время»; выделяет память, как и у лестницы, на решении о
    // касте раз в полторы секунды, не на такте.
    struct CastMemory
    {
        uint32 LastSpell     = 0;       // что именно произносили — иначе выбор не проверить
        uint32 CastsTried    = 0;       // за этот бой: попыток произнести
        uint32 CastsWent     = 0;       //               и сколько ушло (по следу в ядре)
        uint32 CastsBusy     = 0;       // не просили: уже читаем или не истёк общий откат
        bool   CastFailNoted = false;   // величины момента без следа — по разу на бой
        bool   WasCasting    = false;   // читали ли на прошлом такте (для счётчика исхода)
        std::set<uint32> SpellsLogged;  // о каком выборе уже написали — по разу за всё время
    };

    class ClientAct
    {
    public:
        // `muted` closes the door outright: every write below returns false and is counted.
        //
        // §10 — THE SHADOW NEEDS THIS TO BE STRUCTURAL. It was "safe" only because the engine
        // happens not to call `Execute` in shadow — but `Ctx` hands a mutable `ClientAct` to
        // every virtual, `Trigger::Check` included, and `Reset` calls `Action::Cancel`, which is
        // an observable act. "Nobody calls it" is a property of today's code; a muted door is a
        // property of the type.
        ClientAct(Player* self, WorldSession* session, bool muted = false)
            : _self(self), _session(session), _muted(muted) { }

        bool Muted() const { return _muted; }
        uint32 Refused() const { return _refused; }

        // NO ACCESSOR FOR THE PLAYER, AND THE REASON IS SHARP.
        //
        // This class first exposed `Player const* Me()`, on the assumption that const-correctness
        // carried the rule. The review showed it does not: `Player.h:2214` declares
        // `WorldSession* GetSession() const { return m_session; }` — a const method handing back
        // a MUTABLE session. So `Me()` was a straight route from an action to every packet
        // handler in the core, and the door this class exists to be was standing open.
        //
        // Identity and state reach an action through Ctx's read-only views instead (§6′).
        //
        // EVERY write method's first statement is `if (!Usable()) return false;`, so muting here
        // closes all sixteen at once — and closes the seventeenth before it is written, which is
        // the only way a rule like this survives new code.
        bool Usable() const
        {
            if (_muted)
            {
                ++_refused;
                return false;
            }
            return _self && _session;
        }

        // -- talking -----------------------------------------------------------------------
        bool QuestGiverHello(ObjectGuid giver);                     // CMSG_QUEST_GIVER_HELLO
        // ВТОРОЕ ПРИВЕТСТВИЕ, И ОНО НЕ ДУБЛЬ ПЕРВОГО. `HandleQuestgiverHelloOpcode` выходит ДО
    // `PrepareQuestMenu`, если у существа есть свой обработчик приветствия, — тогда меню не
    // строится вовсе и первый опкод возвращает пустоту (замер: «предложил пунктов 0» у всех,
    // кто дошёл до Milly Osworth). Живой клиент шлёт этот; шлём и мы.
    // Имя опкода в ЭТОМ ядре — `CMSG_TALK_TO_GOSSIP`, проверено по таблице обработчиков.
    bool GossipHello(ObjectGuid unit);                          // CMSG_TALK_TO_GOSSIP
    bool GossipSelect(ObjectGuid unit, uint32 menuId, uint32 optionId); // CMSG_GOSSIP_SELECT_OPTION

        // -- quests ------------------------------------------------------------------------
        bool AcceptQuest(ObjectGuid giver, uint32 questId);         // CMSG_QUEST_GIVER_ACCEPT_QUEST
        // An empty ender means "hand in to myself", and THAT is what makes it a script turn-in.
        // The call site couples the two — `FromScript = (ender == nullptr)` — so the coupling is
        // derived here rather than offered as a parameter a caller could contradict.
        bool CompleteQuest(ObjectGuid ender, uint32 questId);       // CMSG_QUEST_GIVER_COMPLETE_QUEST

        // -- combat ------------------------------------------------------------------------
        bool SetSelection(ObjectGuid target);                       // CMSG_SET_SELECTION
        // ПОВЕРНУТЬСЯ К ЦЕЛИ — ОТДЕЛЬНАЯ ДВЕРЬ, А НЕ ШАГ НУЛЕВОЙ ДЛИНЫ.
        //
        // У позиции есть ориентация, и соблазн послать поворот через `Step` очевиден. Он неверен:
        // обработчик ЗАМЕЩАЕТ весь `MovementInfo` присланным, а `Step` строит его с нуля — то есть
        // стёр бы транспорт, падение, прыжок и тангаж. Здесь состояние копируется целиком и
        // меняются только поворот и время, как это делает лестница.
        //
        // Без поворота ядро отвергает каждый замах: `Unit::UpdateMeleeAttackingState` требует
        // `HasInArc`. Возвращает ИСТИНУ, когда поворачиваться уже не нужно, — это успех, а не
        // отказ, и путать их значило бы не ударить ровно тогда, когда всё в порядке.
        bool Face(ObjectGuid target);                               // CMSG_MOVE_SET_FACING
        bool AttackSwing(ObjectGuid victim);                        // CMSG_ATTACK_SWING
        bool AttackStop();                                          // CMSG_ATTACK_STOP

        // -- loot: four opcodes, one protocol (open -> money -> items -> release). Bodies moved
        //    from the ladder's LootFromCorpse/TakeOpenLoot; the protocol itself is
        //    `LootFromCorpseCore` and it reaches these through a `LootSender`.
        bool LootUnit(ObjectGuid unit);                             // CMSG_LOOT_UNIT
        bool LootMoney();                                           // CMSG_LOOT_MONEY
        bool LootItems(LootPick const* picks, uint32 count);        // CMSG_LOOT_ITEM
        bool LootRelease(ObjectGuid unit);                          // CMSG_LOOT_RELEASE

        // -- spells: builds the request the way the client does and sends it. Body moved from
        //    the ladder's CastAtTarget; WHAT to cast is policy and stays in `CastAtTargetCore`.
        bool CastSpell(uint32 spellId, ObjectGuid target);          // CMSG_CAST_SPELL

        // -- the two remaining raw writes of the ladder's `Talking` (`Constellation.cpp:5280`,
        //    `:5346`). The click is one guid. The item's TARGET is the spell's contract, not
        //    «always the creature»: explicit unit, a point on the ground, or none (an area
        //    around oneself) — the door carries the three, the policy picks one.
        struct UseItemTarget
        {
            enum Kind : uint8 { None, Unit, Dest } What = None;
            ObjectGuid Guid;                                        // Unit
            Position   Where;                                       // Dest
        };
        bool SpellClick(ObjectGuid unit);                           // CMSG_SPELL_CLICK
        bool UseItem(uint8 bag, uint8 slot, ObjectGuid item, uint32 spellId,
                     UseItemTarget const& target);                  // CMSG_USE_ITEM

        // -- the world ---------------------------------------------------------------------
        bool UseGameObject(ObjectGuid go);                          // CMSG_GAME_OBJ_USE
        bool EnterAreaTrigger(int32 areaTriggerId);                 // CMSG_AREA_TRIGGER

        // -- trade -------------------------------------------------------------------------
        bool ListInventory(ObjectGuid vendor);                      // CMSG_LIST_INVENTORY
        bool SellItem(ObjectGuid vendor, ObjectGuid item, uint32 amount);   // CMSG_SELL_ITEM
        bool RepairAll(ObjectGuid npc);                             // CMSG_REPAIR_ITEM

        // -- travel. §7 and the operator's ruling of 2026-09-07: «ходим ногами», flights are
        //    ordinary work, and there is NO teleport here because a client cannot send one.
        bool ActivateTaxi(ObjectGuid master, uint32 node);          // CMSG_ACTIVATE_TAXI
        bool EnableTaxiNode(ObjectGuid master);                     // CMSG_ENABLE_TAXI_NODE
        // Found by reading BindAtInn (Constellation.cpp:9359) for the innkeeper trigger: the
        // sixteenth method, and the hearthstone is worthless without it.
        bool BinderActivate(ObjectGuid innkeeper);                  // CMSG_BINDER_ACTIVATE

        // -- movement ----------------------------------------------------------------------
        //
        // This was first left out with the excuse that "the module moves through the motion
        // master, not a packet". The review checked and that was wrong: `SendMove`
        // (Constellation.cpp:11598) builds a MovementInfo and dispatches CMSG_MOVE_HEARTBEAT
        // through the real handler, exactly as a client does. The excuse was mine.
        //
        // Then it was added as `Move(Position, flags)` and the review caught the worse mistake:
        // an arbitrary position is a TELEPORT. Every one of the module's six senders supplies a
        // position within one step of the current one — `go = std::min(step, legLen)`
        // (Constellation.cpp:1554) — and the operator's ruling of 2026-09-07 is «ходим ногами».
        // A door that accepts any position hands back the very thing the door exists to forbid.
        //
        // So it is a STEP, and the bound is enforced here rather than trusted to callers.
        // A refused step is a defect to find, like a swept reservation in §10⁗, not a
        // condition to tolerate — it means an action tried to move somewhere it could not walk.
        bool Step(Position const& next, uint32 movementFlags);      // CMSG_MOVE_HEARTBEAT

        // ОСТАНОВИТЬСЯ. Отдельная дверь, потому что перестать слать шаги — НЕ значит встать:
        // сервер продолжает вести туда, куда его отправили в последний раз. Лестница шлёт этот
        // же опкод первым делом, входя в отдых.
        //
        // Бюджет шага она не тратит и не может: остановка не покрывает расстояния.
        bool StopMoving();                                          // CMSG_MOVE_STOP

        // How far this companion could walk in one slice, by the speed of the movement it is
        // actually doing. `MOVE_RUN` alone was wrong: a swimming companion is capped by
        // `MOVE_SWIM` and a flying one by `MOVE_FLIGHT`, and using the run speed for all three
        // both over- and under-permits depending on the state.
        float MaxStepYards() const;

        // ДЛИНА ТАКТА, ИЗМЕРЕННАЯ ДВЕРЬЮ. Двигателю нужен `dt` в секундах, и второго измерения
        // того же самого заводить нельзя: два счётчика одного расходятся, а разошедшись, дают
        // спутника, который считает свою скорость иначе, чем дверь считает его предел шага.
        float SliceSeconds() const { return _sliceSeconds; }

        // §11 and the review: bounding ONE step is not bounding movement. A hundred legal steps
        // in a single tick is a teleport spelled slowly. The engine calls this once per tick and
        // the budget is what a companion could actually have covered in it.
        // `sliceMs` — СКОЛЬКО ВРЕМЕНИ ПРОШЛО с прошлого такта ЭТОГО спутника. Ноль значит
        // «первый такт, мерить не с чем», и дверь возьмёт заявленную частоту модуля.
        // `refusedSink` — КУДА СКЛАДЫВАТЬ ОТКАЗЫ ШАГА. Своего счётчика у двери нет намеренно:
        // два счётчика одного и того же расходятся, и один из них оказывается тем, который
        // никто не заполняет (Кодекс, пункт 4). Здесь он ровно один, и живёт он там, где его
        // читают, — в состоянии спутника.
        void ResetTick(uint32 sliceMs, uint32* refusedSink);

        // NOT YET HERE, and deliberately listed so the gap is visible rather than discovered:
        //   UseItem     — CMSG_USE_ITEM needs the cast id built at the call site; moved when
        //                 the hearthstone action is written.
        //   ChooseReward— CMSG_QUEST_GIVER_CHOOSE_REWARD carries a LootItemType and an index
        //                 chosen by the gear rules; moved with the turn-in action.
        //   Loot*       — four opcodes that always travel together; moved as one group.

    private:
        Player*       _self;
        WorldSession* _session;
        float         _stepBudgetYards = 0.0f;   // spent by Step, refilled by ResetTick
        bool          _tickOpen        = false;  // no movement before the engine opens the tick
        float         _sliceSeconds    = 0.0f;   // ИЗМЕРЕННАЯ длина такта, в секундах
        uint32*       _refusedSink     = nullptr; // счётчик отказов шага — в состоянии спутника
        bool          _muted           = false;  // §10 — тень: дверь закрыта наглухо
        mutable uint32 _refused        = 0;      // сколько раз в неё постучали при этом
    };
}

#endif // CONSTELLATION_ENGINE_CLIENTACT_H
