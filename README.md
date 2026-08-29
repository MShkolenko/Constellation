# Constellation

Серверный модуль спутников для AlgalonCore (11.2.7). Constellation населяет мир
спутниками — «живыми созвездиями», — реализованными целиком на стороне сервера:
без автоматизации клиента, без внедрённых аддонов, без чужого кода.

Это независимая реализация с нуля. Модуль линкуется со скриптовым API ядра — и больше
ни с чем.

## Устройство

```
src/    исходники модуля (подхватываются сборкой ядра как подкаталог Custom)
conf/   constellation.conf.dist — документированные ключи настройки
docs/   ARCHITECTURE.md — устройство и фазы; ROADMAP.md — дорожная карта
tools/  integrate.sh — подключает модуль к чекауту ядра
```

## Подключение (одна ссылка + один охраняемый крюк)

Сборка ядра уже компилирует всё, что лежит в `src/server/scripts/Custom/`.
Подключение — это:

1. `src/server/scripts/Custom/Constellation` → символическая ссылка на `src/` этого
   репозитория.
2. Форк несёт один коммит на шесть строк в `Custom/custom_script_loader.cpp`:
   крюк обёрнут в `#if __has_include("Constellation/Registration.h")`, поэтому без
   модуля ядро собирается байт в байт как прежде.

`tools/integrate.sh <каталог-исходников-ядра>` делает шаг 1 и проверяет шаг 2.

## Лицензия

GPL-2.0-or-later — см. [COPYING](COPYING): модуль линкуется с ядром TrinityCore (GPL-2.0).
Строки авторства в файлах сохраняются при любом использовании — этого требует лицензия.
Если модуль вам пригодился, будем рады ссылке на репозиторий — это просьба, не условие.

## Состояние

Фаза 0 (каркас) — на боевом сервере. Фазы 1–2 (состав по расам, вход через сессии
без сокета) — в испытаниях. План — в `docs/ROADMAP.md`.

---

<details>
<summary><strong>English</strong></summary>

<br>

Server-side companion module for AlgalonCore (11.2.7). Constellation populates the realm
with server-driven companions — "living constellations" — implemented entirely on the
server: no client automation, no injected addons, no third-party code.

This is an independent, from-scratch implementation. It links against the core's script
API and nothing else.

### Layout

```
src/    module sources (picked up by the core build as a Custom script subtree)
conf/   constellation.conf.dist — documented configuration keys
docs/   ARCHITECTURE.md — design and phases; ROADMAP.md — the roadmap
tools/  integrate.sh — wires the module into a core checkout
```

### Integration (one symlink + one guarded hook)

The core build already compiles every source under `src/server/scripts/Custom/`.
Integration is:

1. `src/server/scripts/Custom/Constellation` → symlink to this repo's `src/`.
2. The fork carries a single 6-line commit in `Custom/custom_script_loader.cpp`:
   the hook is wrapped in `#if __has_include("Constellation/Registration.h")`,
   so the core builds byte-identically whether or not the module is checked out.

`tools/integrate.sh <core-src-dir>` performs step 1 and verifies step 2.

### License

GPL-2.0-or-later — see [COPYING](COPYING): the module links against TrinityCore (GPL-2.0).
Copyright notices in the files must be preserved by any use — the license requires it.
If the module serves you, a link back to the repository is appreciated — a request, not a term.

### Status

Phase 0 (skeleton) is on the live realm. Phases 1–2 (per-race roster, socketless-session
login) are in testing. The plan lives in `docs/ROADMAP.md`.

</details>
