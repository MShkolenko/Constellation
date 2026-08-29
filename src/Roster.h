/*
 * Copyright (C) 2026 Constellation Project (AlgalonCore)
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
 * Constellation — the fixed first roster: one companion per playable race.
 *
 * Names are hand-picked and GENDER-MATCHED: the client treats sex as body 1 /
 * body 2, so nothing downstream will catch a male body carrying a female name.
 * The pairing below is the single source of truth — change name and gender
 * together or not at all. No famous lore figures: those sit in the reserved
 * names store and would bounce on CheckPlayerName.
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
    uint8 Sex;              // GENDER_MALE / GENDER_FEMALE — must match Name
};

// 13 classic races, lore-fitting class each, 7 female / 6 male.
inline constexpr std::array<RosterEntry, 13> Roster =
{{
    // Alliance
    { "Aldric",   RACE_HUMAN,              CLASS_PALADIN, GENDER_MALE   },
    { "Brandir",  RACE_DWARF,              CLASS_HUNTER,  GENDER_MALE   },
    { "Sylwen",   RACE_NIGHTELF,           CLASS_DRUID,   GENDER_FEMALE },
    { "Tizzine",  RACE_GNOME,              CLASS_MAGE,    GENDER_FEMALE },
    { "Naariel",  RACE_DRAENEI,            CLASS_SHAMAN,  GENDER_FEMALE },
    { "Greyric",  RACE_WORGEN,             CLASS_ROGUE,   GENDER_MALE   },
    { "Meilin",   RACE_PANDAREN_ALLIANCE,  CLASS_MONK,    GENDER_FEMALE },
    // Horde
    { "Karguk",   RACE_ORC,                CLASS_WARRIOR, GENDER_MALE   },
    { "Morwenna", RACE_UNDEAD_PLAYER,      CLASS_WARLOCK, GENDER_FEMALE },
    { "Ohanzee",  RACE_TAUREN,             CLASS_SHAMAN,  GENDER_MALE   },
    { "Zulwara",  RACE_TROLL,              CLASS_PRIEST,  GENDER_FEMALE },
    { "Lyresa",   RACE_BLOODELF,           CLASS_MAGE,    GENDER_FEMALE },
    { "Razlo",    RACE_GOBLIN,             CLASS_ROGUE,   GENDER_MALE   },
}};
}

#endif
