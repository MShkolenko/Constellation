#!/bin/bash
# Приборная доска боевого: замороженная база для миграции на движок.
#
# Шаг 1 плана. Правило 3: сравнение всегда с контрольной когортой В ТОТ ЖЕ ЧАС, никогда
# со «вчера». Поэтому скрипт считает не абсолюты, а нормированные на спутника-час числа
# за окно, и умеет делить состав на две когорты по списку имён.
#
#   bash realm_board.sh [минут_окна] [имя,имя,...]
#
# Второй аргумент — канарейка; если задан, всё считается дважды: канарейка и контроль.
# Без него — весь состав одной группой (это и есть база до движка).
#
# Поправка v2, пункт 16: три метрики бегства заведены ЗДЕСЬ, до флипа, потому что вся
# машина бегства живёт внутри тела Idle, которое стирается, а неудача бегства — это смерть.
set -u
WIN=${1:-60}
CANARY=${2:-}

PID=$(pgrep -x worldserver)
L=$(readlink /proc/$PID/fd/3 2>/dev/null)
[ -f "$L" ] || L=/opt/wow/tc-1127/build/bin/RelWithDebInfo/bin/Server.log
BOTS="(SELECT a.id FROM auth_1127.account a JOIN auth_1127.battlenet_accounts b ON b.id=a.battlenet_account WHERE b.email LIKE 'CONSTELLATION%')"

echo "=== окно $WIN мин, журнал $L ==="
UP=$(ps -o etimes= -p "$PID" | tr -d ' ')
echo "мир поднят: $((UP/60)) мин"
[ "$UP" -lt $((WIN*60)) ] && echo "ВНИМАНИЕ: мир моложе окна — числа неполные"

# окно по времени: берём хвост журнала за WIN минут по отметке времени в начале строки
SINCE=$(date -d "-$WIN minutes" '+%Y-%m-%d %H:%M:%S')
TMP=$(mktemp)
awk -v s="$SINCE" '$0 >= s' "$L" > "$TMP" 2>/dev/null || tail -n 400000 "$L" > "$TMP"
echo "строк в окне: $(wc -l < "$TMP")"

echo
echo "=== безопасность (общая, не по когортам) ==="
printf "строк модуля в минуту:      %s\n" "$(( $(grep -ac Constellation "$TMP") / WIN ))"
# НАГРУЗКА МЕРЯЕТСЯ ПО САМОМУ ГОРЯЧЕМУ ПОТОКУ, А НЕ СУММОЙ (оператор, 2026-09-07).
#
# Здесь стояло `ps -o %cpu`, то есть сумма по всем потокам: «128 %» не отличает мир на
# сорока процентах от мира, упёршегося в сто. А упирается именно мировой поток — он один
# тикает автомат, и именно он однажды ушёл с 45 % на 84 %, после чего модуль выдавал одну
# строку в десять секунд. Стоп-таблица плана оперирует потоком, а доска печатала сумму,
# то есть не могла накормить собственные ворота.
#
# Первый проход top -H — среднее с запуска, поэтому берётся второй.
top -H -b -n 2 -d 1 -p "$PID" 2>/dev/null | awk -v np="$(nproc --all)" '
  /^ *PID/ { ++hdr; next }
  hdr >= 2 && $1 ~ /^[0-9]+$/ {
    c = $9 + 0; name = $NF
    if (c > max) { max = c; who = name; tid = $1 }
    if (c > 80) ++hot
    sum += c
  }
  END {
    printf "worldserver: самый горячий поток %.0f%% (%s, tid %s), потоков выше 80%%: %d\n",
           max, who, tid, hot + 0
    printf "             сумма по потокам %.0f%% из %d00%% машины — для справки, не для ворот\n",
           sum, np
  }'
ps -o rss= -p "$PID" | awk '{printf "             RSS %.1f ГиБ\n", $1/1048576}'
free -m | awk 'NR==2{printf "память хоста: %d/%d МиБ\n", $3, $2}'
printf "ШАГ отказано:               %s\n" "$(grep -ac 'Constellation ШАГ' "$TMP")"
printf "БОЙ-ПУСТОЙ:                 %s\n" "$(grep -ac 'БОЙ-ПУСТОЙ' "$TMP")"

board () {          # $1 — ярлык, $2 — regex имён (пусто = все боты)
  local tag="$1" rx="$2" names n
  if [ -z "$rx" ]; then
    names=$(mysql -N -e "SELECT GROUP_CONCAT(c.name SEPARATOR '|') FROM characters_1127.characters c WHERE c.online=1 AND c.account IN $BOTS;")
  else
    names="$rx"
  fi
  n=$(echo "$names" | tr '|' '\n' | grep -c .)
  [ "$n" -eq 0 ] && { echo "  $tag: пусто"; return; }
  local ch=$(echo "scale=3; $n * $WIN / 60" | bc)   # спутник-часов в окне

  local kills flees fleedeath turnins deaths notarget idle
  kills=$(grep -acE "БОЙ ($names).*ПОБЕДА" "$TMP")
  flees=$(grep -acE "ОТХОД ($names)" "$TMP")
  fleedeath=$(grep -aE "($names)" "$TMP" | grep -acE 'ОТХОД.*полторы минуты')
  turnins=$(grep -acE "($names) сдал квест" "$TMP")
  deaths=$(grep -acE "ГИБЕЛЬ ($names)" "$TMP")
  notarget=$(grep -aE "ГИБЕЛЬ ($names)" "$TMP" | grep -ac 'цели не было')
  idle=$(grep -acE "ПРОСТОЙ ($names)" "$TMP")

  echo "  $tag: душ $n, спутник-часов $ch"
  printf "    сдано квестов / сп-час:   %s\n" "$(echo "scale=2; $turnins / $ch" | bc)"
  printf "    побед / сп-час:           %s\n" "$(echo "scale=2; $kills / $ch" | bc)"
  printf "    гибелей / сп-час:         %s\n" "$(echo "scale=2; $deaths / $ch" | bc)"
  printf "      из них без цели:        %s\n" "$notarget"
  printf "    бегств / сп-час:          %s\n" "$(echo "scale=2; $flees / $ch" | bc)"
  printf "      кончившихся отказом:    %s\n" "$fleedeath"
  printf "    простоев / сп-час:        %s\n" "$(echo "scale=2; $idle / $ch" | bc)"
}

echo
echo "=== семь чисел ==="
if [ -n "$CANARY" ]; then
  RX=$(echo "$CANARY" | tr ',' '|')
  board "КАНАРЕЙКА" "$RX"
  ALL=$(mysql -N -e "SELECT GROUP_CONCAT(c.name SEPARATOR '|') FROM characters_1127.characters c WHERE c.online=1 AND c.account IN $BOTS;")
  CTRL=$(echo "$ALL" | tr '|' '\n' | grep -vxF -f <(echo "$RX" | tr '|' '\n') | paste -sd'|')
  board "КОНТРОЛЬ" "$CTRL"
else
  board "ВЕСЬ СОСТАВ" ""
fi

echo
echo "=== настройки, влияющие на инвариант (поправка v2, пункт 2) ==="
C=/srv/wow/tc-1127/build/bin/RelWithDebInfo/etc/worldserver.conf
grep -E '^[[:space:]]*(InstantFlightPaths|AllFlightPaths)' "$C" 2>/dev/null || echo "  не заданы — по умолчанию 0"

rm -f "$TMP"
