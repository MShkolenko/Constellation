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
 * Constellation -- priority lists: what a player of the class presses, in order.
 *
 * WHY A LIST AND NOT A FORMULA (operator, 29.09, after the mechanics audit): ranking by spell level
 * made Ice Lance beat Frostbolt, Hamstring beat Slam, Mind Flay push out Shadow Word: Pain. Damage
 * from coefficients does not work either: Ice Lance, Moonfire and Templar's Verdict carry their
 * damage in a script, and a builder is worth its resource, not its own hit. A guide says "keep the
 * DoT up, spend at full combo points, otherwise the builder", so the list says the same.
 *
 * The engine walks the list top to bottom and casts the first step whose condition holds and
 * which the core would accept now (known, ready, affordable, in range, right form); nothing found
 * falls back to the old generic pick. A step for a spell the character does not know yet is
 * skipped, so one list covers levels 1-10 of the class. Lists here are the class starting kit
 * (spec 0 = any); a spec list (spec id) takes precedence once written.
 */

#ifndef CONSTELLATION_ROTATION_H
#define CONSTELLATION_ROTATION_H

#include "SharedDefines.h"

namespace Constellation
{
enum class RotCond : uint8
{
    Always,
    TargetLacksMyAura,  // A = aura id; also when my aura has under 3 s left (refresh)
    PowerAtLeast,       // A = Powers, B = amount
    PowerBelow,         // A = Powers, B = amount
    ComboFullOrDying,   // combo points at max, or target at or under 30 % with at least one
    TargetHealthBelow,  // A = percent
    SelfHealthBelow,    // A = percent
    SelfLacksAura,      // A = aura id (buffs, shields); B = extra percent health gate (0 = none)
    SelfLacksAuraFullCombo, // A = aura id; and combo points at max (Slice and Dice at full length)
};

struct RotStep
{
    uint8   Class;
    uint16  Spec;       // 0 = any spec of the class
    uint32  Spell;
    RotCond Cond;
    int32   A = 0;
    int32   B = 0;
    bool    OnSelf = false;
};

inline constexpr RotStep Rotation[] =
{
    // WARRIOR: heal-strike after a kill, execute range, the cooldown strike, then the rage spender.
    // Hamstring (0.15 AP) is not a damage button; Whirlwind is area.
    { CLASS_WARRIOR, 0, 34428,  RotCond::SelfHealthBelow,   90 },        // Victory Rush (core gates it on a kill)
    { CLASS_WARRIOR, 0, 163201, RotCond::TargetHealthBelow, 20 },        // Execute
    { CLASS_WARRIOR, 0, 23922,  RotCond::Always },                       // Shield Slam
    { CLASS_WARRIOR, 0, 1464,   RotCond::Always },                       // Slam
    { CLASS_WARRIOR, 0, 57755,  RotCond::Always },                       // Heroic Throw (only out of melee: min range)

    // PALADIN: Judgment (area rule stays in the usability check), Crusader Strike, holy power spender,
    // stun as a life saver.
    { CLASS_PALADIN, 0, 20271,  RotCond::Always },                       // Judgment
    { CLASS_PALADIN, 0, 35395,  RotCond::Always },                       // Crusader Strike
    { CLASS_PALADIN, 0, 53600,  RotCond::PowerAtLeast, POWER_HOLY_POWER, 3 }, // Shield of the Righteous
    { CLASS_PALADIN, 0, 853,    RotCond::SelfHealthBelow,   50 },        // Hammer of Justice

    // HUNTER: spend focus when there is plenty, otherwise the generator.
    { CLASS_HUNTER,  0, 185358, RotCond::PowerAtLeast, POWER_FOCUS, 60 }, // Arcane Shot
    { CLASS_HUNTER,  0, 56641,  RotCond::Always },                       // Steady Shot
    { CLASS_HUNTER,  0, 185358, RotCond::Always },                       // Arcane Shot (whatever focus is left)

    // ROGUE: Slice and Dice when missing, finisher at full points, else the builder.
    { CLASS_ROGUE,   0, 315496, RotCond::SelfLacksAuraFullCombo, 315496 }, // Slice and Dice (spends points)
    { CLASS_ROGUE,   0, 196819, RotCond::ComboFullOrDying },             // Eviscerate
    { CLASS_ROGUE,   0, 1752,   RotCond::Always },                       // Sinister Strike

    // PRIEST: shield first when hurt, keep Pain up, Mind Blast on cooldown, Smite.
    { CLASS_PRIEST,  0, 19236,  RotCond::SelfHealthBelow,   40, 0, true }, // Desperate Prayer
    { CLASS_PRIEST,  0, 17,     RotCond::SelfLacksAura, 17, 80, true },    // Power Word: Shield under 80 %
    { CLASS_PRIEST,  0, 589,    RotCond::TargetLacksMyAura, 589 },       // Shadow Word: Pain
    { CLASS_PRIEST,  0, 8092,   RotCond::Always },                       // Mind Blast
    { CLASS_PRIEST,  0, 585,    RotCond::Always },                       // Smite

    // MAGE (starting kit): instant Fire Blast on cooldown, Frostbolt filler.
    { CLASS_MAGE,    0, 319836, RotCond::Always },                       // Fire Blast
    { CLASS_MAGE,    0, 116,    RotCond::Always },                       // Frostbolt

    // WARLOCK: defensive, keep Corruption up, Drain Life when hurt, Shadow Bolt filler.
    { CLASS_WARLOCK, 0, 104773, RotCond::SelfHealthBelow,   35, 0, true }, // Unending Resolve
    { CLASS_WARLOCK, 0, 172,    RotCond::TargetLacksMyAura, 146739 },    // Corruption
    { CLASS_WARLOCK, 0, 234153, RotCond::SelfHealthBelow,   50 },        // Drain Life
    { CLASS_WARLOCK, 0, 686,    RotCond::Always },                       // Shadow Bolt

    // MONK: heal when hurt, Blackout Kick on cooldown, Tiger Palm with energy, lightning only out of
    // melee (Tiger Palm fails its range check first).
    { CLASS_MONK,    0, 322101, RotCond::SelfHealthBelow,   60, 0, true }, // Expel Harm
    { CLASS_MONK,    0, 100784, RotCond::Always },                       // Blackout Kick
    { CLASS_MONK,    0, 100780, RotCond::Always },                       // Tiger Palm
    { CLASS_MONK,    0, 117952, RotCond::Always },                       // Crackling Jade Lightning

    // SHAMAN: Lightning Shield kept up, Flame Shock up, melee strike when in reach, Lightning Bolt.
    { CLASS_SHAMAN,  0, 192106, RotCond::SelfLacksAura, 192106, 0, true }, // Lightning Shield
    { CLASS_SHAMAN,  0, 470411, RotCond::TargetLacksMyAura, 188389 },    // Flame Shock
    { CLASS_SHAMAN,  0, 73899,  RotCond::Always },                       // Primal Strike (melee range only)
    { CLASS_SHAMAN,  0, 188196, RotCond::Always },                       // Lightning Bolt

    // DRUID: in Cat Form (kept up by the buffs below from level 5) the finisher at full points, else Shred;
    // before Cat Form, or when it is not up, the caster pair. Steps of the wrong form fail CheckShapeshift.
    { CLASS_DRUID,   0, 22568,  RotCond::ComboFullOrDying },             // Ferocious Bite
    { CLASS_DRUID,   0, 5221,   RotCond::Always },                       // Shred
    { CLASS_DRUID,   0, 8921,   RotCond::TargetLacksMyAura, 164812 },    // Moonfire
    { CLASS_DRUID,   0, 5176,   RotCond::Always },                       // Wrath

    // DEATH KNIGHT / DEMON HUNTER / EVOKER: not in the roster; the generic pick serves them.
};

// SELF BUFFS OUT OF COMBAT: what a player keeps up between fights (an hour-long buff, weapon poisons).
// Cast on self when the aura is missing, the character is idle and the core would accept the cast.
struct BuffStep
{
    uint8  Class;
    uint32 Spell;
    uint32 Aura;
};

inline constexpr BuffStep Buffs[] =
{
    { CLASS_WARRIOR, 6673,   6673   },  // Battle Shout
    { CLASS_PRIEST,  21562,  21562  },  // Power Word: Fortitude
    { CLASS_MAGE,    1459,   1459   },  // Arcane Intellect
    { CLASS_DRUID,   1126,   1126   },  // Mark of the Wild (before the form: it cannot be cast in Cat Form)
    { CLASS_DRUID,   768,    768    },  // Cat Form: the druid fights and travels as a cat from level 5
    { CLASS_SHAMAN,  192106, 192106 },  // Lightning Shield
    { CLASS_ROGUE,   315584, 315584 },  // Instant Poison
    { CLASS_ROGUE,   3408,   3408   },  // Crippling Poison
};
}

#endif
