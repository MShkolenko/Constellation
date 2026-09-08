#!/bin/bash
# Приборная доска боевого. Шаг 1 плана миграции.
#
# ПОЧЕМУ РАЗНИЦА СНИМКОВ, А НЕ ОКНО ПО ВРЕМЕНИ.
#
# Первая редакция резала «последний час» через `awk '$0 >= "<дата>"'`. Проверка сырыми
# счётчиками показала, что окно содержало ВЕСЬ журнал (63645 строк из 63654), а предыдущий
# такой же час — ноль строк. Причина в самом журнале: `Appender.Server=2,2,0,...` — флаги
# нуль, и НИ ОДНА строка не начинается с даты. Меток времени в журнале нет вообще, поэтому
# лексикографическое сравнение с датой пропускало почти всё.
#
# Отсюда семь чисел были не скоростями, а суммами за весь подъём мира, делёнными на 114, и
# два замера подряд совпадали до сотых просто потому, что мерили одно и то же.
#
# Правильный способ без меток: считать НАКОПЛЕННЫЕ итоги и брать разницу с прошлым снимком,
# делённую на реально прошедшее время. Первый запуск только заводит базу.
#
#   bash realm_board.sh              # снимок и разница с прошлым
#   bash realm_board.sh --reset      # забыть прошлый снимок
set -u
STATE=/root/.realm_board.state
[ "${1:-}" = "--reset" ] && { rm -f "$STATE"; echo "база забыта"; exit 0; }

PID=$(pgrep -x worldserver)
L=$(readlink /proc/$PID/fd/3 2>/dev/null)
[ -f "$L" ] || L=/opt/wow/tc-1127/build/bin/RelWithDebInfo/bin/Server.log
BOTS="(SELECT a.id FROM auth_1127.account a JOIN auth_1127.battlenet_accounts b ON b.id=a.battlenet_account WHERE b.email LIKE 'CONSTELLATION%')"

NOW=$(date +%s)
UP=$(ps -o etimes= -p "$PID" | tr -d ' ')
NAMES=$(mysql -N -e "SELECT GROUP_CONCAT(c.name SEPARATOR '|') FROM characters_1127.characters c WHERE c.online=1 AND c.account IN $BOTS;")
SOULS=$(echo "$NAMES" | tr '|' '\n' | grep -c .)

# накопленные итоги по всему журналу
T_QUEST=$(grep -acE "($NAMES) сдал квест" "$L")
T_WIN=$(grep -acE "БОЙ ($NAMES).*ПОБЕДА" "$L")
T_DEATH=$(grep -acE "ГИБЕЛЬ ($NAMES)" "$L")
T_NOTGT=$(grep -aE "ГИБЕЛЬ ($NAMES)" "$L" | grep -ac 'цели не было')
T_FLEE=$(grep -acE "ОТХОД ($NAMES)" "$L")
T_IDLE=$(grep -acE "ПРОСТОЙ ($NAMES)" "$L")
T_STEP=$(grep -ac 'Constellation ШАГ' "$L")
T_EMPTY=$(grep -ac 'БОЙ-ПУСТОЙ' "$L")
T_LINES=$(grep -ac Constellation "$L")

echo "=== снимок ==="
echo "мир поднят: $((UP/60)) мин, душ онлайн: $SOULS, строк модуля всего: $T_LINES"
top -H -b -n 2 -d 1 -p "$PID" 2>/dev/null | awk '
  /^ *PID/ { ++hdr; next }
  hdr >= 2 && $1 ~ /^[0-9]+$/ { c = $9 + 0; if (c > max) { max = c; tid = $1 }; if (c > 80) ++hot; sum += c }
  END { printf "самый горячий поток %.0f%% (tid %s), выше 80%%: %d; сумма %.0f%% — для справки\n", max, tid, hot+0, sum }'
ps -o rss= -p "$PID" | awk '{printf "RSS %.1f ГиБ\n", $1/1048576}'
free -m | awk 'NR==2{printf "память хоста: %d/%d МиБ\n", $3, $2}'

if [ -f "$STATE" ]; then
  . "$STATE"
  DT=$((NOW - P_NOW))
  # перезапуск мира или ротация журнала: любой счётчик уменьшился — база недействительна
  if [ "$T_QUEST" -lt "$P_QUEST" ] || [ "$T_WIN" -lt "$P_WIN" ] || [ "$T_LINES" -lt "$P_LINES" ]; then
    echo; echo "!! счётчики уменьшились — мир перезапущен или журнал сменился; база заведена заново"
    DT=0
  fi
else
  DT=0
fi

if [ "${DT:-0}" -ge 60 ]; then
  H=$(echo "scale=6; $DT / 3600" | bc)
  CH=$(echo "scale=4; $SOULS * $H" | bc)
  echo
  echo "=== семь чисел: НА СПУТНИКА-ЧАС за $(echo "scale=1; $DT/60" | bc) мин ==="
  r () { echo "scale=2; ($1 - $2) / $CH" | bc; }
  printf "  сдано квестов:        %s   (прирост %s)\n" "$(r $T_QUEST $P_QUEST)"  "$((T_QUEST-P_QUEST))"
  printf "  побед:                %s   (прирост %s)\n" "$(r $T_WIN $P_WIN)"      "$((T_WIN-P_WIN))"
  printf "  гибелей:              %s   (прирост %s)\n" "$(r $T_DEATH $P_DEATH)"  "$((T_DEATH-P_DEATH))"
  printf "    из них без цели:    %s   (прирост %s)\n" "$(r $T_NOTGT $P_NOTGT)"  "$((T_NOTGT-P_NOTGT))"
  printf "  бегств:               %s   (прирост %s)\n" "$(r $T_FLEE $P_FLEE)"    "$((T_FLEE-P_FLEE))"
  printf "  простоев:             %s   (прирост %s)\n" "$(r $T_IDLE $P_IDLE)"    "$((T_IDLE-P_IDLE))"
  printf "  строк модуля в минуту: %s\n" "$(echo "scale=0; ($T_LINES - $P_LINES) * 60 / $DT" | bc)"
  echo "  ШАГ отказано за окно:  $((T_STEP-P_STEP))    БОЙ-ПУСТОЙ: $((T_EMPTY-P_EMPTY))"
else
  echo
  echo "=== база заведена; скорости будут при следующем запуске (нужно не меньше минуты) ==="
  echo "накоплено за весь подъём: квестов $T_QUEST, побед $T_WIN, гибелей $T_DEATH, простоев $T_IDLE"
fi

cat > "$STATE" <<EOF
P_NOW=$NOW
P_QUEST=$T_QUEST
P_WIN=$T_WIN
P_DEATH=$T_DEATH
P_NOTGT=$T_NOTGT
P_FLEE=$T_FLEE
P_IDLE=$T_IDLE
P_STEP=$T_STEP
P_EMPTY=$T_EMPTY
P_LINES=$T_LINES
EOF

echo
echo "=== настройки, влияющие на инвариант ==="
grep -E '^[[:space:]]*(InstantFlightPaths|AllFlightPaths)' \
  /srv/wow/tc-1127/build/bin/RelWithDebInfo/etc/worldserver.conf 2>/dev/null || echo "  не заданы"
