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
 * Constellation -- the roster: EVERY standard race in EVERY class it can take.
 *
 * 122 companions over 13 races. The race/class pairs are not invented here:
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

inline constexpr std::array<RosterEntry, 122> Roster =
{{
    // человек
    { "Garrick",   RACE_HUMAN,                CLASS_WARRIOR,       GENDER_MALE },
    { "Aldric",    RACE_HUMAN,                CLASS_PALADIN,       GENDER_MALE },
    { "Rowena",    RACE_HUMAN,                CLASS_HUNTER,        GENDER_FEMALE },
    { "Cecily",    RACE_HUMAN,                CLASS_ROGUE,         GENDER_FEMALE },
    { "Adeline",   RACE_HUMAN,                CLASS_PRIEST,        GENDER_FEMALE },
    { "Corvin",    RACE_HUMAN,                CLASS_DEATH_KNIGHT,  GENDER_MALE },
    { "Emrick",    RACE_HUMAN,                CLASS_MAGE,          GENDER_MALE },
    { "Deverel",   RACE_HUMAN,                CLASS_WARLOCK,       GENDER_MALE },
    { "Brienne",   RACE_HUMAN,                CLASS_MONK,          GENDER_FEMALE },
    // орк
    { "Karguk",    RACE_ORC,                  CLASS_WARRIOR,       GENDER_MALE },
    { "Durok",     RACE_ORC,                  CLASS_HUNTER,        GENDER_MALE },
    { "Mazzok",    RACE_ORC,                  CLASS_ROGUE,         GENDER_MALE },
    { "Shakti",    RACE_ORC,                  CLASS_PRIEST,        GENDER_FEMALE },
    { "Grommara",  RACE_ORC,                  CLASS_DEATH_KNIGHT,  GENDER_FEMALE },
    { "Urzula",    RACE_ORC,                  CLASS_SHAMAN,        GENDER_FEMALE },
    { "Thragan",   RACE_ORC,                  CLASS_MAGE,          GENDER_MALE },
    { "Gorrum",    RACE_ORC,                  CLASS_WARLOCK,       GENDER_MALE },
    { "Kaltha",    RACE_ORC,                  CLASS_MONK,          GENDER_FEMALE },
    // дворф
    { "Thoradin",  RACE_DWARF,                CLASS_WARRIOR,       GENDER_MALE },
    { "Balrik",    RACE_DWARF,                CLASS_PALADIN,       GENDER_MALE },
    { "Brandir",   RACE_DWARF,                CLASS_HUNTER,        GENDER_MALE },
    { "Gerda",     RACE_DWARF,                CLASS_ROGUE,         GENDER_FEMALE },
    { "Hilda",     RACE_DWARF,                CLASS_PRIEST,        GENDER_FEMALE },
    { "Morgrim",   RACE_DWARF,                CLASS_DEATH_KNIGHT,  GENDER_MALE },
    { "Thyra",     RACE_DWARF,                CLASS_SHAMAN,        GENDER_FEMALE },
    { "Brenna",    RACE_DWARF,                CLASS_MAGE,          GENDER_FEMALE },
    { "Durgan",    RACE_DWARF,                CLASS_WARLOCK,       GENDER_MALE },
    { "Ferla",     RACE_DWARF,                CLASS_MONK,          GENDER_FEMALE },
    // ночной эльф
    { "Theron",    RACE_NIGHTELF,             CLASS_WARRIOR,       GENDER_MALE },
    { "Faelan",    RACE_NIGHTELF,             CLASS_HUNTER,        GENDER_MALE },
    { "Nyressa",   RACE_NIGHTELF,             CLASS_ROGUE,         GENDER_FEMALE },
    { "Aelira",    RACE_NIGHTELF,             CLASS_PRIEST,        GENDER_FEMALE },
    { "Ilyndir",   RACE_NIGHTELF,             CLASS_DEATH_KNIGHT,  GENDER_MALE },
    { "Shaeryn",   RACE_NIGHTELF,             CLASS_MAGE,          GENDER_FEMALE },
    { "Vaelthas",  RACE_NIGHTELF,             CLASS_WARLOCK,       GENDER_MALE },
    { "Liriel",    RACE_NIGHTELF,             CLASS_MONK,          GENDER_FEMALE },
    { "Sylwen",    RACE_NIGHTELF,             CLASS_DRUID,         GENDER_FEMALE },
    { "Kaelith",   RACE_NIGHTELF,             CLASS_DEMON_HUNTER,  GENDER_MALE },
    // нежить
    { "Vorlin",    RACE_UNDEAD_PLAYER,        CLASS_WARRIOR,       GENDER_MALE },
    { "Grimwald",  RACE_UNDEAD_PLAYER,        CLASS_HUNTER,        GENDER_MALE },
    { "Yssara",    RACE_UNDEAD_PLAYER,        CLASS_ROGUE,         GENDER_FEMALE },
    { "Nadira",    RACE_UNDEAD_PLAYER,        CLASS_PRIEST,        GENDER_FEMALE },
    { "Corvath",   RACE_UNDEAD_PLAYER,        CLASS_DEATH_KNIGHT,  GENDER_MALE },
    { "Elmira",    RACE_UNDEAD_PLAYER,        CLASS_MAGE,          GENDER_FEMALE },
    { "Morwenna",  RACE_UNDEAD_PLAYER,        CLASS_WARLOCK,       GENDER_FEMALE },
    { "Ashkar",    RACE_UNDEAD_PLAYER,        CLASS_MONK,          GENDER_MALE },
    // таурен
    { "Mato",      RACE_TAUREN,               CLASS_WARRIOR,       GENDER_MALE },
    { "Takoda",    RACE_TAUREN,               CLASS_PALADIN,       GENDER_MALE },
    { "Chayton",   RACE_TAUREN,               CLASS_HUNTER,        GENDER_MALE },
    { "Winona",    RACE_TAUREN,               CLASS_ROGUE,         GENDER_FEMALE },
    { "Anpaytoo",  RACE_TAUREN,               CLASS_PRIEST,        GENDER_FEMALE },
    { "Nashoba",   RACE_TAUREN,               CLASS_DEATH_KNIGHT,  GENDER_MALE },
    { "Ohanzee",   RACE_TAUREN,               CLASS_SHAMAN,        GENDER_MALE },
    { "Kimimela",  RACE_TAUREN,               CLASS_MAGE,          GENDER_FEMALE },
    { "Wachiwi",   RACE_TAUREN,               CLASS_WARLOCK,       GENDER_FEMALE },
    { "Hototo",    RACE_TAUREN,               CLASS_MONK,          GENDER_MALE },
    { "Ayasha",    RACE_TAUREN,               CLASS_DRUID,         GENDER_FEMALE },
    // гном
    { "Nobbin",    RACE_GNOME,                CLASS_WARRIOR,       GENDER_MALE },
    { "Fizwick",   RACE_GNOME,                CLASS_HUNTER,        GENDER_MALE },
    { "Pimpi",     RACE_GNOME,                CLASS_ROGUE,         GENDER_FEMALE },
    { "Wenna",     RACE_GNOME,                CLASS_PRIEST,        GENDER_FEMALE },
    { "Snargle",   RACE_GNOME,                CLASS_DEATH_KNIGHT,  GENDER_MALE },
    { "Tizzine",   RACE_GNOME,                CLASS_MAGE,          GENDER_FEMALE },
    { "Bimble",    RACE_GNOME,                CLASS_WARLOCK,       GENDER_FEMALE },
    { "Klanko",    RACE_GNOME,                CLASS_MONK,          GENDER_MALE },
    // тролль
    { "Zalko",     RACE_TROLL,                CLASS_WARRIOR,       GENDER_MALE },
    { "Jubaka",    RACE_TROLL,                CLASS_HUNTER,        GENDER_MALE },
    { "Tayana",    RACE_TROLL,                CLASS_ROGUE,         GENDER_FEMALE },
    { "Zulwara",   RACE_TROLL,                CLASS_PRIEST,        GENDER_FEMALE },
    { "Mahiki",    RACE_TROLL,                CLASS_DEATH_KNIGHT,  GENDER_MALE },
    { "Nakuru",    RACE_TROLL,                CLASS_SHAMAN,        GENDER_MALE },
    { "Sennja",    RACE_TROLL,                CLASS_MAGE,          GENDER_FEMALE },
    { "Voljara",   RACE_TROLL,                CLASS_WARLOCK,       GENDER_FEMALE },
    { "Bumbu",     RACE_TROLL,                CLASS_MONK,          GENDER_MALE },
    { "Yalanda",   RACE_TROLL,                CLASS_DRUID,         GENDER_FEMALE },
    // гоблин
    { "Grizzik",   RACE_GOBLIN,               CLASS_WARRIOR,       GENDER_MALE },
    { "Sprocket",  RACE_GOBLIN,               CLASS_HUNTER,        GENDER_MALE },
    { "Razlo",     RACE_GOBLIN,               CLASS_ROGUE,         GENDER_MALE },
    { "Nixxa",     RACE_GOBLIN,               CLASS_PRIEST,        GENDER_FEMALE },
    { "Zibby",     RACE_GOBLIN,               CLASS_DEATH_KNIGHT,  GENDER_FEMALE },
    { "Krezzo",    RACE_GOBLIN,               CLASS_SHAMAN,        GENDER_MALE },
    { "Vexa",      RACE_GOBLIN,               CLASS_MAGE,          GENDER_FEMALE },
    { "Fizzle",    RACE_GOBLIN,               CLASS_WARLOCK,       GENDER_MALE },
    { "Twizzy",    RACE_GOBLIN,               CLASS_MONK,          GENDER_FEMALE },
    // эльф крови
    { "Kaelor",    RACE_BLOODELF,             CLASS_WARRIOR,       GENDER_MALE },
    { "Theronis",  RACE_BLOODELF,             CLASS_PALADIN,       GENDER_MALE },
    { "Aravel",    RACE_BLOODELF,             CLASS_HUNTER,        GENDER_FEMALE },
    { "Sylvara",   RACE_BLOODELF,             CLASS_ROGUE,         GENDER_FEMALE },
    { "Elenwe",    RACE_BLOODELF,             CLASS_PRIEST,        GENDER_FEMALE },
    { "Valethar",  RACE_BLOODELF,             CLASS_DEATH_KNIGHT,  GENDER_MALE },
    { "Lyresa",    RACE_BLOODELF,             CLASS_MAGE,          GENDER_FEMALE },
    { "Naeryn",    RACE_BLOODELF,             CLASS_WARLOCK,       GENDER_FEMALE },
    { "Belorin",   RACE_BLOODELF,             CLASS_MONK,          GENDER_MALE },
    { "Ithaeril",  RACE_BLOODELF,             CLASS_DEMON_HUNTER,  GENDER_MALE },
    // дреней
    { "Nuroth",    RACE_DRAENEI,              CLASS_WARRIOR,       GENDER_MALE },
    { "Vaandor",   RACE_DRAENEI,              CLASS_PALADIN,       GENDER_MALE },
    { "Ishala",    RACE_DRAENEI,              CLASS_HUNTER,        GENDER_FEMALE },
    { "Ohana",     RACE_DRAENEI,              CLASS_ROGUE,         GENDER_FEMALE },
    { "Yrel",      RACE_DRAENEI,              CLASS_PRIEST,        GENDER_FEMALE },
    { "Maraad",    RACE_DRAENEI,              CLASS_DEATH_KNIGHT,  GENDER_MALE },
    { "Naariel",   RACE_DRAENEI,              CLASS_SHAMAN,        GENDER_FEMALE },
    { "Zurael",    RACE_DRAENEI,              CLASS_MAGE,          GENDER_MALE },
    { "Talandra",  RACE_DRAENEI,              CLASS_WARLOCK,       GENDER_FEMALE },
    { "Ekkorn",    RACE_DRAENEI,              CLASS_MONK,          GENDER_MALE },
    // ворген
    { "Hallow",    RACE_WORGEN,               CLASS_WARRIOR,       GENDER_MALE },
    { "Ashgrove",  RACE_WORGEN,               CLASS_HUNTER,        GENDER_MALE },
    { "Greyric",   RACE_WORGEN,               CLASS_ROGUE,         GENDER_MALE },
    { "Elswyth",   RACE_WORGEN,               CLASS_PRIEST,        GENDER_FEMALE },
    { "Marrow",    RACE_WORGEN,               CLASS_DEATH_KNIGHT,  GENDER_MALE },
    { "Bramwyn",   RACE_WORGEN,               CLASS_MAGE,          GENDER_FEMALE },
    { "Thornwick", RACE_WORGEN,               CLASS_WARLOCK,       GENDER_MALE },
    { "Ivelle",    RACE_WORGEN,               CLASS_MONK,          GENDER_FEMALE },
    { "Ravenna",   RACE_WORGEN,               CLASS_DRUID,         GENDER_FEMALE },
    // пандарен
    { "Baoshen",   RACE_PANDAREN_ALLIANCE,    CLASS_WARRIOR,       GENDER_MALE },
    { "Wenjun",    RACE_PANDAREN_ALLIANCE,    CLASS_HUNTER,        GENDER_MALE },
    { "Meilin",    RACE_PANDAREN_ALLIANCE,    CLASS_ROGUE,         GENDER_FEMALE },
    { "Liuwei",    RACE_PANDAREN_ALLIANCE,    CLASS_PRIEST,        GENDER_MALE },
    { "Xinyi",     RACE_PANDAREN_ALLIANCE,    CLASS_DEATH_KNIGHT,  GENDER_FEMALE },
    { "Jinhua",    RACE_PANDAREN_ALLIANCE,    CLASS_SHAMAN,        GENDER_FEMALE },
    { "Shenlong",  RACE_PANDAREN_ALLIANCE,    CLASS_MAGE,          GENDER_MALE },
    { "Yunmei",    RACE_PANDAREN_ALLIANCE,    CLASS_WARLOCK,       GENDER_FEMALE },
    { "Taozi",     RACE_PANDAREN_ALLIANCE,    CLASS_MONK,          GENDER_FEMALE },
}};
}

#endif
