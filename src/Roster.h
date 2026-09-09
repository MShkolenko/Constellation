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
 * Constellation -- the roster: eight humans, one starting zone.
 *
 * CUT TO EIGHT on 2026-09-09, and the reason is not arithmetic. The operator: "только люди на
 * стартовой локации, без ДК / налаживаешь модуль по ним. остальное это частности / основные
 * классы у них есть / не будем распылять твое внимание". It was every standard race in every
 * class it could take -- 122, then 114 once the goblins were paused, then 11, then 10.
 *
 * THE DEATH KNIGHT GOES FOR HIS START, NOT HIS CLASS. He begins in Acherus on map 609, a tiered
 * citadel whose navmesh does not even load in full on this realm, while everyone else begins in
 * Northshire. One companion in nine living in a different world makes every measurement a blend
 * of two different problems.
 *
 * WHAT THE CUT BUYS: one race, one starting zone, one levelling path. Eight companions walk the
 * same quests to the same givers, so any difference between them is a difference of CLASS -- not
 * of map, phase or starting chain. Until now every number mixed thirteen different beginnings.
 *
 * Nobody is deleted. This table decides who is summoned; Corvin, Sylwen and the other 106 of the
 * 114 that were provisioned stay in the characters database and simply stop coming into the
 * world. Restoring a line brings that companion back where it stands.
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

// 8: люди без рыцаря смерти (было 10, до сокращения 114). ЧИСЛО ЗДЕСЬ — НЕ
// УКРАШЕНИЕ: std::array с меньшим числом записей компилируется молча и добивает остаток
// нулями, то есть спутниками без имени.
inline constexpr std::array<RosterEntry, 8> Roster =
{{
    // восемь классов человека, которые начинают в Северной Долине
    { "Garrick",    RACE_HUMAN,                 CLASS_WARRIOR,       GENDER_MALE },
    { "Aldric",     RACE_HUMAN,                 CLASS_PALADIN,       GENDER_MALE },
    { "Rowena",     RACE_HUMAN,                 CLASS_HUNTER,        GENDER_FEMALE },
    { "Cecily",     RACE_HUMAN,                 CLASS_ROGUE,         GENDER_FEMALE },
    { "Adeline",    RACE_HUMAN,                 CLASS_PRIEST,        GENDER_FEMALE },
    { "Emrick",     RACE_HUMAN,                 CLASS_MAGE,          GENDER_MALE },
    { "Deverel",    RACE_HUMAN,                 CLASS_WARLOCK,       GENDER_MALE },
    { "Brienne",    RACE_HUMAN,                 CLASS_MONK,          GENDER_FEMALE },
}};
}

#endif
