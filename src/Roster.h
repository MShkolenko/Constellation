/*
 * Copyright (C) 2026 MShkolenko <montekristo1995@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Constellation -- the roster: humans, and the one class we still want that they cannot be.
 *
 * CUT DOWN TO TEN on 2026-09-09. First to eleven (operator: keep only humans, the death knight
 * included, plus whichever ALLIANCE race fills in the classes they cannot take), then to ten the
 * same day -- "ДХ не берём совсем". It was every standard race in every class it could take: 122,
 * then 114 once the goblins were paused.
 *
 * DROPPING THE DEMON HUNTER CHANGED THE ANSWER, NOT JUST A LINE. The night elf was chosen because
 * it was the only Alliance race supplying TWO of the missing classes -- druid AND demon hunter.
 * Without the second it supplies exactly one, the same as the worgen.
 *
 * SO NO SINGLE RACE CAN EVEN COVER TWO, and that is data rather than a preference. A human takes
 * nine classes; shaman, druid and evoker are what remain wanted. Read from the realm's own
 * playercreateinfo: night elf and worgen bring druid; dwarf, draenei and Alliance pandaren bring
 * shaman; gnome brings nothing. Shaman and druid never come from the same Alliance race except
 * the Kul Tiran, and allied races are excluded (operator, 2026-08-29) because they have no
 * ordinary levelling start. Evoker exists only on the dracthyr, also allied. So it is druid OR
 * shaman from one race, or a SECOND race for both.
 *
 * NIGHT ELF RATHER THAN WORGEN, since both give the druid: the worgen start is Gilneas, and this
 * realm has a known and expensive problem there -- companions standing in zone 0, and the Gilneas
 * door already cost a day of reading. Teldrassil is an ordinary start.
 *
 * Only the MISSING class is taken from the night elf -- not a second warrior and a second mage.
 * Every class appears exactly once. Kaelith is not deleted: this table decides who is summoned,
 * and restoring his line puts him back in Mardum where he stands.
 *
 * The other 103 companions are not deleted. This table decides who is SUMMONED, not who exists;
 * their characters stay in the database and simply stop coming into the world.
 *
 * The race/class pairs are not invented here:
 * they were read from the realm's own world.playercreateinfo, which is what the
 * core consults when a client creates a character, so an impossible pair cannot
 * be in this table. Allied races are deliberately excluded (operator, 2026-08-29).
 *
 * Names are hand-picked per race and GENDER-MATCHED: the client treats sex as
 * body 1 / body 2, so nothing downstream will catch a male body carrying a female
 * name. Change a name and its gender together or not at all. No famous lore
 * figures -- those sit in the reserved names store and would bounce on
 * CheckPlayerName. 63 male, 59 female.
 *
 * Generated, then committed as data -- this header is the single source of truth.
 */

#ifndef CONSTELLATION_ROSTER_H
#define CONSTELLATION_ROSTER_H

#include "SharedDefines.h"

#include <array>

namespace Constellation
{
struct RosterEntry
{
    char const* Name;       // gender-matched, see header comment
    uint8 Race;
    uint8 Class;
    uint8 Sex;              // GENDER_MALE / GENDER_FEMALE -- must match Name
};

// 10: девять человеческих классов и друид (было 11, до сокращения 114). ЧИСЛО ЗДЕСЬ — НЕ
// УКРАШЕНИЕ: std::array с меньшим числом записей компилируется молча и добивает остаток
// нулями, то есть спутниками без имени.
inline constexpr std::array<RosterEntry, 10> Roster =
{{
    // человек — все девять классов, которые ему доступны, рыцарь смерти в том числе
    { "Garrick",    RACE_HUMAN,                 CLASS_WARRIOR,       GENDER_MALE },
    { "Aldric",     RACE_HUMAN,                 CLASS_PALADIN,       GENDER_MALE },
    { "Rowena",     RACE_HUMAN,                 CLASS_HUNTER,        GENDER_FEMALE },
    { "Cecily",     RACE_HUMAN,                 CLASS_ROGUE,         GENDER_FEMALE },
    { "Adeline",    RACE_HUMAN,                 CLASS_PRIEST,        GENDER_FEMALE },
    { "Corvin",     RACE_HUMAN,                 CLASS_DEATH_KNIGHT,  GENDER_MALE },
    { "Emrick",     RACE_HUMAN,                 CLASS_MAGE,          GENDER_MALE },
    { "Deverel",    RACE_HUMAN,                 CLASS_WARLOCK,       GENDER_MALE },
    { "Brienne",    RACE_HUMAN,                 CLASS_MONK,          GENDER_FEMALE },
    // ночной эльф — только друид. Охотника на демонов не берём вовсе (оператор, 2026-09-09)
    { "Sylwen",     RACE_NIGHTELF,              CLASS_DRUID,         GENDER_FEMALE },
}};
}

#endif
