# -*- coding: utf-8 -*-
"""Аудит таймеров Companion: объявлены — тикают — читаются.

Круг 79 нарвался на поля, которые читают, но никто не пишет. Прежде чем заменять
ручную бухгалтерию объявленным реестром, надо знать, что там сейчас на самом деле:
сколько таймеров, все ли тикают, все ли читаются. Иначе замена спрячет дефект,
вместо того чтобы его показать.
"""
import io
import re
import sys
from collections import Counter

SRC = r"F:\core\constellation\src\Constellation.cpp"

with io.open(SRC, "r", encoding="utf-8") as f:
    lines = f.read().split("\n")

# 1. границы struct Companion
start = next(i for i, l in enumerate(lines) if l.startswith("struct Companion"))
depth = 0
end = None
for i in range(start, len(lines)):
    depth += lines[i].count("{") - lines[i].count("}")
    if depth == 0 and i > start:
        end = i
        break
body = lines[start:end + 1]
print("struct Companion: строки %d..%d (%d строк)" % (start + 1, end + 1, end - start + 1))

# 2. поля-таймеры (uint32 ...Ms) и поля-карты задержек
timer_re = re.compile(r"^\s*(?:uint32|int32|uint64)\s+(\w*Ms)\s*(?:=|;)")
map_re = re.compile(r"^\s*std::(?:map|unordered_map)<[^>]*>\s+(\w+)\s*;")
timers = [m.group(1) for l in body for m in [timer_re.match(l)] if m]
maps = [m.group(1) for l in body for m in [map_re.match(l)] if m]
print("полей-таймеров (*Ms): %d" % len(timers))
print("полей-карт:           %d" % len(maps))

whole = "\n".join(lines)

def uses(name):
    """Сколько раз поле вообще упоминается вне объявления."""
    return len(re.findall(r"\b" + re.escape(name) + r"\b", whole)) - 1

# 3. тикает ли: ищем уменьшение вида c.X -= / c.X = std::max / Tick(c.X ...)
def ticks(name):
    pats = [
        r"c\.%s\s*(?:-=|=\s*[^;]*-\s*)" % re.escape(name),
        r"Tick\w*\(\s*c\.%s\b" % re.escape(name),
        r"\b%s\s*(?:-=)" % re.escape(name),
    ]
    return any(re.search(p, whole) for p in pats)

dead_read, never_ticked, ok = [], [], []
for t in timers:
    n = uses(t)
    if n == 0:
        dead_read.append((t, n))
    elif not ticks(t):
        never_ticked.append((t, n))
    else:
        ok.append((t, n))

print()
print("=== таймеры, которые НИКТО не уменьшает (%d) ===" % len(never_ticked))
for t, n in sorted(never_ticked, key=lambda x: -x[1]):
    print("  %-28s упоминаний %d" % (t, n))

print()
print("=== таймеры, объявленные и не упомянутые нигде (%d) ===" % len(dead_read))
for t, n in dead_read:
    print("  %s" % t)

print()
print("=== карты, объявленные и не упомянутые нигде ===")
dead_maps = [m for m in maps if uses(m) == 0]
print("  " + (", ".join(dead_maps) if dead_maps else "нет"))

print()
print("=== карты, которые читают, но не пишут ===")
for m in maps:
    if uses(m) == 0:
        continue
    written = re.search(r"c\.%s\s*\[|c\.%s\.(?:insert|emplace|erase|clear)" % (re.escape(m), re.escape(m)), whole)
    read = re.search(r"c\.%s\.(?:find|count|empty|size|begin)" % re.escape(m), whole)
    if read and not written:
        print("  %-28s читают, но не пишут" % m)

print()
print("итого: тикают %d, не тикают %d, мёртвых %d" % (len(ok), len(never_ticked), len(dead_read)))
