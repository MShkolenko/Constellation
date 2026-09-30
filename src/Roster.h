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
 * Constellation -- the roster: eight humans (Northshire) and nine trolls (Echo Isles), and two more
 * groups of the same 17 born at level 10 and level 20 (Group and Level fields).
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

#include <iterator>

namespace Constellation
{
struct RosterEntry
{
    char const* Name;       // gender-matched, see header comment
    uint8 Race;
    uint8 Class;
    uint8 Sex;              // GENDER_MALE / GENDER_FEMALE -- must match Name
    uint16 Spec = 0;        // ChrSpecialization asked for at level 10; 0 = the first damage spec
    uint8 Level = 1;        // born at this level: 1, 10 or 20
    uint8 Group = 0;        // 0 = newborns, 1 = level 10, 2 = level 20; keys the account and the landing hubs
    uint8 Hub = 0;          // which of the group's three landing hubs (0..2); ignored for group 0
};

// LANDING HUBS of the level groups (operator 2026-09-30, teleport research in
// .agent/tmp/levelgroups-teleport.md): an innkeeper entry per hub, resolved to its spawn when the
// character is created. Each group is spread over three hubs so nobody waits behind seven others at
// one quest giver. Level 10 hubs have a zone floor at most 10, level 20 hubs at most 10 as well
// (ceiling 30, so mobs are not grey); groups 1 and 2 never share a hub.
struct LandingHub { uint8 Group; bool Horde; uint32 InnEntry; };
inline constexpr LandingHub LandingHubs[] =
{
    { 1, false, 6727  }, { 1, false, 6790  }, { 1, false, 6734  },   // Lakeshire, Darkshire, Thelsamar
    { 1, true,  3934  }, { 1, true,  41892 }, { 1, true,  2388  },   // Crossroads, Sun Rock Retreat, Tarren Mill
    { 2, false, 6807  }, { 2, false, 6738  }, { 2, false, 43420 },   // Booty Bay, Astranaar, Auberdine
    { 2, true,  14731 }, { 2, true,  5814  }, { 2, true,  43872 },   // Revantusk, Grom'gol, Dessina (Desolace)
};

// 51: три группы (уровни 1, 10, 20) по восемь людей и девять троллей, без рыцаря смерти. Массив
// C с выводимым размером: std::array с меньшим числом записей компилировался молча и добивал остаток
// нулями, то есть спутниками без имени. Порядок - порядок запуска: сначала группа 0 (как всегда),
// потом 10-й уровень, потом 20-й (`Constellation.MaxActive` режет хвост).
//
// ТРОЛЛИ ЦЕЛИКОМ (оператор, 29.09): «выпускай троллей ... троллей целиком». Это единственная
// не союзная раса с обоими недостающими людям классами, друидом и шаманом. Все девять
// начинают на одном месте (Echo Isles, карта 1, -1171 -5263 по playercreateinfo), поэтому
// правило «одна раса - одна стартовая зона» держится внутри каждой команды. Строки взяты из
// ростера до сокращения 09.09: персонажи с этими именами уже есть в базе и вернутся, где стоят.
// СПЕК ЗАДАН У КАЖДОГО (оператор, 29.09: «делай все»; аудит механик 29.09): «первый урон по
// порядку» делал всех магов Тайными, и сосульки, над которыми работали сутки, не доставались
// никому. Спеки разнесены между людьми и троллями, чтобы каждая механика шла вживую хоть у кого-то.
inline constexpr RosterEntry Roster[] =
{
    // восемь классов человека, которые начинают в Северной Долине
    { "Garrick",    RACE_HUMAN,                 CLASS_WARRIOR,       GENDER_MALE, 71 },
    { "Aldric",     RACE_HUMAN,                 CLASS_PALADIN,       GENDER_MALE, 70 },
    { "Rowena",     RACE_HUMAN,                 CLASS_HUNTER,        GENDER_FEMALE, 253 },
    { "Cecily",     RACE_HUMAN,                 CLASS_ROGUE,         GENDER_FEMALE, 260 },
    { "Adeline",    RACE_HUMAN,                 CLASS_PRIEST,        GENDER_FEMALE, 258 },
    { "Emrick",     RACE_HUMAN,                 CLASS_MAGE,          GENDER_MALE, 64 },
    { "Deverel",    RACE_HUMAN,                 CLASS_WARLOCK,       GENDER_MALE, 265 },
    { "Brienne",    RACE_HUMAN,                 CLASS_MONK,          GENDER_FEMALE, 269 },
    // девять классов тролля, которые начинают на Echo Isles
    { "Zalko",      RACE_TROLL,                 CLASS_WARRIOR,       GENDER_MALE, 72 },
    { "Jubaka",     RACE_TROLL,                 CLASS_HUNTER,        GENDER_MALE, 254 },
    { "Tayana",     RACE_TROLL,                 CLASS_ROGUE,         GENDER_FEMALE, 259 },
    { "Zulwara",    RACE_TROLL,                 CLASS_PRIEST,        GENDER_FEMALE, 258 },
    { "Nakuru",     RACE_TROLL,                 CLASS_SHAMAN,        GENDER_MALE, 262 },
    { "Sennja",     RACE_TROLL,                 CLASS_MAGE,          GENDER_FEMALE, 63 },
    { "Voljara",    RACE_TROLL,                 CLASS_WARLOCK,       GENDER_FEMALE, 267 },
    { "Bumbu",      RACE_TROLL,                 CLASS_MONK,          GENDER_MALE, 269 },
    { "Yalanda",    RACE_TROLL,                 CLASS_DRUID,         GENDER_FEMALE, 103 },

    // GROUP 1, level 10, humans (Lakeshire, Darkshire, Thelsamar)
    { "Edmund",     RACE_HUMAN,      CLASS_WARRIOR,      GENDER_MALE,    71, 10, 1, 0 },
    { "Harold",     RACE_HUMAN,      CLASS_PALADIN,      GENDER_MALE,    70, 10, 1, 1 },
    { "Matilda",    RACE_HUMAN,      CLASS_HUNTER,       GENDER_FEMALE, 253, 10, 1, 2 },
    { "Philippa",   RACE_HUMAN,      CLASS_ROGUE,        GENDER_FEMALE, 260, 10, 1, 0 },
    { "Eleanor",    RACE_HUMAN,      CLASS_PRIEST,       GENDER_FEMALE, 258, 10, 1, 1 },
    { "Percival",   RACE_HUMAN,      CLASS_MAGE,         GENDER_MALE,    64, 10, 1, 2 },
    { "Osric",      RACE_HUMAN,      CLASS_WARLOCK,      GENDER_MALE,   265, 10, 1, 0 },
    { "Rosalind",   RACE_HUMAN,      CLASS_MONK,         GENDER_FEMALE, 269, 10, 1, 1 },
    // GROUP 1, level 10, trolls (Crossroads, Sun Rock Retreat, Tarren Mill)
    { "Rokhan",     RACE_TROLL,      CLASS_WARRIOR,      GENDER_MALE,    72, 10, 1, 0 },
    { "Kalaya",     RACE_TROLL,      CLASS_HUNTER,       GENDER_FEMALE, 254, 10, 1, 1 },
    { "Vuldak",     RACE_TROLL,      CLASS_ROGUE,        GENDER_MALE,   259, 10, 1, 2 },
    { "Mazira",     RACE_TROLL,      CLASS_PRIEST,       GENDER_FEMALE, 258, 10, 1, 0 },
    { "Mahanu",     RACE_TROLL,      CLASS_SHAMAN,       GENDER_MALE,   262, 10, 1, 1 },
    { "Nisani",     RACE_TROLL,      CLASS_MAGE,         GENDER_FEMALE,  63, 10, 1, 2 },
    { "Torkal",     RACE_TROLL,      CLASS_WARLOCK,      GENDER_MALE,   267, 10, 1, 0 },
    { "Jindu",      RACE_TROLL,      CLASS_MONK,         GENDER_MALE,   269, 10, 1, 1 },
    { "Sanjari",    RACE_TROLL,      CLASS_DRUID,        GENDER_FEMALE, 103, 10, 1, 2 },
    // GROUP 2, level 20, humans (Booty Bay, Astranaar, Auberdine)
    { "Reginald",   RACE_HUMAN,      CLASS_WARRIOR,      GENDER_MALE,    71, 20, 2, 0 },
    { "Wilfred",    RACE_HUMAN,      CLASS_PALADIN,      GENDER_MALE,    70, 20, 2, 1 },
    { "Beatrix",    RACE_HUMAN,      CLASS_HUNTER,       GENDER_FEMALE, 253, 20, 2, 2 },
    { "Winifred",   RACE_HUMAN,      CLASS_ROGUE,        GENDER_FEMALE, 260, 20, 2, 0 },
    { "Cordelia",   RACE_HUMAN,      CLASS_PRIEST,       GENDER_FEMALE, 258, 20, 2, 1 },
    { "Ambrose",    RACE_HUMAN,      CLASS_MAGE,         GENDER_MALE,    64, 20, 2, 2 },
    { "Leofric",    RACE_HUMAN,      CLASS_WARLOCK,      GENDER_MALE,   265, 20, 2, 0 },
    { "Gwendolyn",  RACE_HUMAN,      CLASS_MONK,         GENDER_FEMALE, 269, 20, 2, 1 },
    // GROUP 2, level 20, trolls (Revantusk, Grom'gol, Dessina)
    { "Hakamba",    RACE_TROLL,      CLASS_WARRIOR,      GENDER_MALE,    72, 20, 2, 0 },
    { "Zahira",     RACE_TROLL,      CLASS_HUNTER,       GENDER_FEMALE, 254, 20, 2, 1 },
    { "Darazu",     RACE_TROLL,      CLASS_ROGUE,        GENDER_MALE,   259, 20, 2, 2 },
    { "Loraya",     RACE_TROLL,      CLASS_PRIEST,       GENDER_FEMALE, 258, 20, 2, 0 },
    { "Kazuru",     RACE_TROLL,      CLASS_SHAMAN,       GENDER_MALE,   262, 20, 2, 1 },
    { "Tamala",     RACE_TROLL,      CLASS_MAGE,         GENDER_FEMALE,  63, 20, 2, 2 },
    { "Vonjo",      RACE_TROLL,      CLASS_WARLOCK,      GENDER_MALE,   267, 20, 2, 0 },
    { "Nalakai",    RACE_TROLL,      CLASS_MONK,         GENDER_MALE,   269, 20, 2, 1 },
    { "Utaya",      RACE_TROLL,      CLASS_DRUID,        GENDER_FEMALE, 103, 20, 2, 2 },
};
}

#endif
