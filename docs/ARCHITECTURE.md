# Constellation — устройство / architecture

## Что такое спутник

Спутник — это **настоящий Player, ведомый сервером**: строка в базе персонажей, вошедшая
через сессию без сокета (WorldSession с пустым сокетом), которую двигает и действует код
модуля. Это не существо, переодетое игроком.

Почему это решение несущее:

- Всё, что ядро уже умеет для игроков — заклинания, ауры, предметы, движение, угроза,
  лут, группы, профессии — достаётся спутникам бесплатно. Имитация на существах
  переписывает это заново и всё равно выдаёт себя (не те пакеты, не та рамка, не может
  в группу, не может торговать).
- Настоящие клиенты видят спутника ровно как другого игрока — потому что для ядра он
  и есть другой игрок.
- Цена: одна WorldSession + один Player на спутника. Отсюда фаза замера нагрузки
  **до** масштабирования.

## Уроки интеграции, оплаченные стендом (девять кругов фазы 1–2)

1. Сессия строится **до** персонажа: конструктор Player разыменовывает её.
2. Сессии живут **вне** менеджера сессий: пустой сокет — `Update()` вернёт false и
   менеджер её пожнёт. Модуль сам зовёт `Update()` каждый тик и игнорирует false.
3. Перед каждым тиком — `ResetTimeOutTime(false)`: первая же строка `Update()` зовёт
   `CloseSocket()` по простою **без проверки на пустоту**.
4. После `HandlePlayerLoginOpcode` — сразу `HandleContinuePlayerLogin()`: настоящий вход
   продолжает второй (мировой) сокет клиента, которого у нас нет.
5. Внешность собирается **зеркалом проверки ядра**: опция входит, если её требование
   проходит `MeetsChrCustomizationReq`, вариант — если его требование проходит **против
   уже собранного набора** (требования вариантов зависят от других выбранных).
6. Учётка — настоящая связка Battle.net + игровая: без родителя валятся внешние ключи
   таблиц коллекций.
7. Фиксация сохранения персонажа — только асинхронная (её запросы помечены для
   асинхронного соединения); правда о создании берётся стадией «Сохраняется» — опросом
   строки в базе, а не доверием вызову.

## Инварианты

0. **Работа от ядра клиентскими действиями** (оператор, 2026-08-29). Игровые поступки
   спутника — только то, что мог бы отправить настоящий клиент: взятие квеста — это
   «клик» по квестодателю через опкоды сессии, не вызов внутренних API вроде
   `Player::AddQuest`. Бот, неотличимый от клиента на уровне протокола, ломается ровно
   там, где сломался бы игрок, — в этом его ценность как проверяющего мира. Транспортный
   уровень (вход без второго сокета, сброс простоя) — дозволенное исключение.

   **Уровень учётной записи — тоже не игровой поступок** (оператор, 2026-08-29): заведение
   персонажа и его привязка к учётке происходят до появления в мире и делаются серверной
   стороной, ровно как вход без сокета. Правило говорит об игре — о том, что спутник делает,
   уже находясь в мире.
1. **Ядро остаётся чистым.** Один охраняемый крюк; нужда в правке ядра — отдельный
   именованный коммит, никогда не «заодно».
2. **Выключен — значит инертен.**
3. **Свои таблицы, свой префикс** `constellation_*` в базе персонажей.
4. **Та же дисциплина отгрузки, что у ядра.**

## Фазы

См. ROADMAP.md — там таблица фаз с состоянием и ворота замера нагрузки.

---

<details>
<summary><strong>English</strong></summary>

<br>

### What a companion is

A companion is a **real Player driven by the server**: a character row in the characters
database, logged in through a socketless session, moved and acted by module code. It is
not a creature dressed as a player.

Why this is the load-bearing decision:

- Everything the core already implements for players — spells, auras, items, movement,
  threat, loot, groups, professions — works for companions for free. A creature-based
  imitation re-implements all of it and still looks wrong.
- Real clients see a companion exactly as another player, because to the core it *is*
  another player.
- Cost: one WorldSession + one Player per companion — hence the load-measurement phase
  gates any scaling.

### Integration lessons, paid for on the rig (nine cycles of phase 1–2)

1. The session is built **before** the character: Player's constructor dereferences it.
2. Sessions live **outside** the session manager: with a null socket `Update()` returns
   false and the manager reaps them. The module ticks `Update()` itself and ignores false.
3. `ResetTimeOutTime(false)` before every tick: `Update()`'s first statement calls
   `CloseSocket()` on idle **without a null check**.
4. `HandleContinuePlayerLogin()` right after `HandlePlayerLoginOpcode`: the real login is
   continued by the client's second (instance) socket, which we do not have.
5. Appearance is assembled by **mirroring the core's validator**: an option enters if its
   requirement passes `MeetsChrCustomizationReq`, a choice if its requirement passes
   **against the set built so far** (choice requirements depend on other selections).
6. The account is a real Battle.net + game pair: without the parent, the collection
   tables' foreign keys fail on every login.
7. Character-save commits are async by necessity (their statements are flagged
   async-connection-only); creation truth comes from a Saving stage that polls for the
   row instead of trusting the call.

### Invariants

0. **Act through the core as a client would** (operator, 2026-08-29). A companion's
   gameplay actions are only what a real client could send: taking a quest is a "click"
   on the questgiver via session opcodes, never an internal API call like
   `Player::AddQuest`. A bot indistinguishable from a client at the protocol level breaks
   exactly where a player would — that is its value as a world-checker. The transport
   layer (socketless login, idle-clock reset) is an allowed exception.

   **Account level is not a gameplay action either** (operator, 2026-08-29): creating a
   character and binding it to an account happen before it exists in the world and are done
   server-side, exactly like socketless login. The rule is about play — what a companion does
   once it is already in the world.
1. **The core stays clean.** One guarded hook; a genuine core change is a separate,
   named commit — never mixed in.
2. **Disabled means inert.**
3. **Own tables, own prefix** `constellation_*` in the characters database.
4. **The same shipping discipline as the core.**

### Phases

See ROADMAP.md for the phase table with states and the load-measurement gate.

</details>
