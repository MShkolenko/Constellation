# -*- coding: utf-8 -*-
"""Чем модуль пишет в мир — ПЕРЕЧИСЛЕНИЕМ, а не по белому списку.

Первая редакция этого скрипта искала записи по списку подозрительных методов, который
я составил сам, и отрапортовала «мимо клиента: 0». Кодекс тут же показал
`self->CombatStop(true)` — метода не было в моём списке. Ошибка та же, что утром с
playerbots: обобщение по выборке, которую сам и выбрал.

Теперь скрипт перечисляет ВСЕ вызовы методов на игроке/цели и делит их на три ведра:
клиентский путь, заведомо читающие, и всё остальное — которое надо смотреть глазами.
"""
import io
import os
import re
from collections import Counter, defaultdict

ROOT = r"F:\core\constellation\src"

# ЛЮБОЙ Handle* на сессии — клиентский путь. Первая редакция требовала суффикс `Opcode` и
# недосчитывала восемь штатных обработчиков (HandleRepopRequest, HandleSpellClick,
# HandleReclaimCorpse, HandleSpiritHealerActivate, HandleQuestgiverCompleteQuest, три Move*Ack):
# счётчик переноса был занижен, а сами они попали в «подозрительные».
OPCODE = re.compile(r"(?:GetSession\(\)|Session)\s*->\s*(Handle\w+)\s*\(")
PACKET = re.compile(r"WorldPacket\s+\w+\s*\(\s*(CMSG_\w+)")

# любой вызов метода на игроке/существе — без предположений о том, что он делает
CALL = re.compile(r"\b(self|bot|player|me|target|victim|owner|who|giver|cr|creature|unit|go|obj)"
                  r"\s*->\s*([A-Z]\w*)\s*\(")

# и на СЕССИИ — всё, что не Handle*Opcode. Пропуск, найденный чтением LearnTaxiNode:
# `c.Session->SendLearnNewTaxiNode(cr)` — прямой внутренний вызов, помеченный «зонд», которого
# перечисление по мировым объектам не видело вовсе. Сессия — второй вход в ядро.
SESSION_CALL = re.compile(r"\bSession\s*->\s*(?!Handle)([A-Z]\w*)\s*\(")

# заведомо читающие префиксы — их отсеиваем, остальное показываем
READ_PREFIX = ("Get", "Is", "Has", "Can", "Find", "Count", "Compute", "Calculate",
               "To", "Satisfy", "Should", "Need", "Contains", "Exists", "At", "Size",
               "Nearest", "Select", "Query", "Print", "Dump")
READ_EXACT = {"GetGUID", "GetName", "GetEntry", "GetMap", "GetMapId", "GetZoneId",
              "GetAreaId", "GetLevel", "GetClass", "GetRace", "GetSession", "GetTypeId"}

op, pk = Counter(), Counter()
other = Counter()
where = defaultdict(list)

for dirpath, _, files in os.walk(ROOT):
    for fn in sorted(files):
        if not fn.endswith((".cpp", ".h")):
            continue
        p = os.path.join(dirpath, fn)
        with io.open(p, "r", encoding="utf-8", errors="replace") as f:
            for n, line in enumerate(f, 1):
                s = line.split("//")[0]
                for m in OPCODE.finditer(s):
                    op[m.group(1)] += 1
                for m in PACKET.finditer(s):
                    pk[m.group(1)] += 1
                for m in CALL.finditer(s):
                    meth = m.group(2)
                    if meth in READ_EXACT or meth.startswith(READ_PREFIX):
                        continue
                    if meth.startswith("Handle") and meth.endswith("Opcode"):
                        continue
                    other[meth] += 1
                    where[meth].append("%s:%d" % (fn, n))
                for m in SESSION_CALL.finditer(s):
                    meth = "Session->" + m.group(1)
                    if m.group(1) in READ_EXACT or m.group(1).startswith(READ_PREFIX):
                        continue
                    other[meth] += 1
                    where[meth].append("%s:%d" % (fn, n))

print("=== клиентский путь ===")
print("Handle*Opcode: %d вызовов, %d разных" % (sum(op.values()), len(op)))
print("WorldPacket(CMSG_*): %d, %d разных" % (sum(pk.values()), len(pk)))

# ------------------------------------------------------------------------------------------
# СЧЁТЧИК ПЕРЕНОСА, и он обязан дойти до нуля.
#
# Кодекс: «все шесть отправителей движения по-прежнему минуют ClientAct через сырой обработчик
# на Constellation.cpp:11598». Это не дефект двери — это состояние незаконченного переноса: по
# §2 старое тело удаляется тем же коммитом, что переключает Owns для его ветки. Но обещание
# нельзя проверить, а число можно. Пока оно не ноль, дверь не единственная.
# ------------------------------------------------------------------------------------------
outside = 0
outside_where = []
for dirpath, _, files in os.walk(ROOT):
    for fn in sorted(files):
        if not fn.endswith((".cpp", ".h")) or fn == "ClientAct.cpp":
            continue
        p = os.path.join(dirpath, fn)
        with io.open(p, "r", encoding="utf-8", errors="replace") as f:
            for n, line in enumerate(f, 1):
                s = line.split("//")[0]
                if OPCODE.search(s):
                    outside += 1
                    outside_where.append("%s:%d" % (fn, n))

print()
print("=== отправок мимо двери (счётчик переноса; цель — 0) ===")
print("осталось: %d" % outside)
print("  " + ", ".join(outside_where[:8]) + (" …" if len(outside_where) > 8 else ""))

print()
print("=== ВСЁ остальное, что зовётся на игроке/существе (не Get/Is/Has/Can/...) ===")
print("вызовов %d, разных методов %d" % (sum(other.values()), len(other)))
print()
for k, v in sorted(other.items(), key=lambda x: (-x[1], x[0])):
    print("  %-34s %4d   %s" % (k, v, ", ".join(where[k][:2])))
