# Дорожная карта / Roadmap

## Карта репозиториев

| Репозиторий | Видимость | Лицензия | Роль |
|---|---|---|---|
| `MShkolenko/AlgalonCore` | публичный | GPL-2.0 (унаследована, выбора нет) | форк ядра: замороженный 11.2.7 + проверенные бэкпорты + ОДИН охраняемый крюк модуля |
| `MShkolenko/TrinityCore` | публичный | GPL-2.0 (унаследована) | нетронутое зеркало апстрима для местных раскопок |
| `MShkolenko/Constellation` | **приватный** | заголовки GPL-2.0-or-later, полный текст при публикации | модуль спутников — целиком наша работа |
| `MShkolenko/homelab` | приватный | — | процесс, решения, инструменты; никогда не игровой код |

## Решение по лицензии

Модуль линкуется с TrinityCore (GPL-2.0): любое **распространяемое** производное обязано нести
GPL-совместимую лицензию. Внутри этого ограничения упоминание достигается средствами самой GPL —
лицензия запрещает удалять строки авторства, поэтому каждый файл несёт:

```
Copyright (C) 2026 Constellation Project (AlgalonCore)
```

Использующий код обязан сохранить эти строки и открыть свои изменения под той же лицензией.
Более жёсткое «упомяните нас в README/рекламе» несовместимо с GPL и невозможно при линковке
с ядром. Вежливая просьба об упоминании идёт в README как просьба, не как условие лицензии.

Пока репозиторий приватный, распространения нет и обязательства не срабатывают; заголовки
уже расставлены, так что открытие — это добавить COPYING, и всё.

## Фазы

| Фаза | Результат | Состояние |
|---|---|---|
| 0 | Каркас: регистрация, настройка, команда статуса | **готово, на боевом** |
| 1 | Серверное создание персонажей: 13 рас, имена по полу, внешность по умолчанию | **этот выпуск** |
| 2 | Присутствие: вход через сессии без сокета, ступенчатый старт, повтор, переживание перезапуска | **этот выпуск** |
| 3 | Следование: вступление в группу призвавшего, следование, телепорт-догон | следующая |
| 4 | Бой: режим помощника, ротация по классу | после 3 |
| 5 | Население + замер нагрузки (сессий на миллисекунду обновления мира) | ворота перед любым ростом |
| 6+ | Экипировка, торговцы, задания, аукцион | нераспределённый задел |

## Постоянные инварианты

1. Один охраняемый крюк в форке; всё остальное — в этом репозитории.
2. `Constellation.Enable = 0` → модуль инертен: ни сессий, ни таймеров, ни записей в базу.
3. Свои таблицы только под `constellation_*`; схема ядра не меняется никогда.
4. Отгрузка как у ядра: сборка на стенде → запуск на стенде → сверка DBErrors →
   проба на сервере на 8095 → подмена. Исключений для «мелочей» нет.
5. Имена — выверенные пары: пол и имя меняются вместе или никак.

---

<details>
<summary><strong>English</strong></summary>

<br>

### Repository map

| Repository | Visibility | License | Role |
|---|---|---|---|
| `MShkolenko/AlgalonCore` | public | GPL-2.0 (inherited, no choice) | the core fork: frozen 11.2.7 + audited back-ports + ONE guarded module hook |
| `MShkolenko/TrinityCore` | public | GPL-2.0 (inherited) | untouched upstream mirror, for local archaeology |
| `MShkolenko/Constellation` | **private** | GPL-2.0-or-later headers, full text at publication | the companion module — all our own work |
| `MShkolenko/homelab` | private | — | process, decisions, tooling; never gameplay code |

### Licensing decision

The module links against TrinityCore, which is GPL-2.0: any *distributed* derivative must
carry a GPL-2.0-compatible license. Within that constraint, attribution is achieved the
GPL's own way — the license forbids removing copyright notices, so every file carries:

```
Copyright (C) 2026 Constellation Project (AlgalonCore)
```

Anyone who uses the code must keep those lines and must publish their modifications under
the same license. A stronger "credit us in your README/advertising" clause would be
GPL-incompatible and cannot be used while linking the core. A polite request for a mention
goes in the README as a request, not a license term.

While the repository is private nothing is distributed and no obligation triggers; the
headers are in place so flipping to public is a two-line change (add COPYING, done).

### Phases

| Phase | Deliverable | State |
|---|---|---|
| 0 | Skeleton: registration, config, status command | **done, on the live realm** |
| 1 | Server-side character creation: 13 races, gender-matched names, default appearance | **this release** |
| 2 | Presence: socketless login, stagger, retry, survive restart | **this release** |
| 3 | Follow: join summoner's group, follow, catch-up teleport | next |
| 4 | Combat: assist mode, class-appropriate rotation | after 3 |
| 5 | Population scaling + load measurement (sessions per world-update ms) | gate before any growth |
| 6+ | Equipment, vendors, quests, auction house | unordered backlog |

### Standing invariants

1. One guarded hook in the fork; everything else in this repository.
2. `Constellation.Enable = 0` → the module is inert: no sessions, no timers, no DB writes.
3. Own tables under `constellation_*` only; core schema never altered.
4. Ship like the core ships: rig build → rig boot → DBErrors compare → server-side probe
   on 8095 → swap. No exceptions for "small" changes.
5. Names are curated pairs: gender and name change together or not at all.

</details>
