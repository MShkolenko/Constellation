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
    SelfHasAura,        // A = aura id, B = minimum stacks (0 = any): procs (Hot Streak, Fingers of Frost, Icicles x5)
    TargetLacksMyAuraFullCombo, // A = aura id (missing or under 3 s), combo points at max, B = min target health % (0 = any)
};

// A step with Spell 0 is a HOLD: when its condition holds the engine casts nothing this tick and does not fall back
// to the generic pick (Arms below 30 rage keeps it for Mortal Strike instead of Hamstring).
inline constexpr uint32 ROT_HOLD = 0xFFFFFFFFu;

struct RotStep
{
    uint8   Class;
    uint16  Spec;       // 0 = any spec of the class
    uint32  Spell;
    RotCond Cond;
    int32   A = 0;
    int32   B = 0;
    bool    OnSelf = false;
    bool    NoRepeat = false;   // skip while this spell was the last NoRepeat spell cast (Combo Strikes)
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

    // ROGUE: the opener from Stealth (the engine stealths on the approach; both need stealth, which the
    // usability check asks the core), then Slice and Dice when missing, finisher at full points, else the builder.
    { CLASS_ROGUE,   0, 8676,   RotCond::Always },                       // Ambush (stealth only)
    { CLASS_ROGUE,   0, 1833,   RotCond::Always },                       // Cheap Shot (stealth only)
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

    // DRUID: shift to Cat Form as the first action of a fight (from level 5), then the finisher at full points,
    // else Shred; before level 5 the caster pair. Steps of the wrong form fail CheckShapeshift. The form is
    // not kept out of combat: taxi, quest items and lock spells refuse a shapeshifted player (Codex 76).
    { CLASS_DRUID,   0, 768,    RotCond::SelfLacksAura, 768, 0, true },  // Cat Form
    { CLASS_DRUID,   0, 22568,  RotCond::ComboFullOrDying },             // Ferocious Bite
    { CLASS_DRUID,   0, 5221,   RotCond::Always },                       // Shred
    { CLASS_DRUID,   0, 8921,   RotCond::TargetLacksMyAura, 164812 },    // Moonfire
    { CLASS_DRUID,   0, 5176,   RotCond::Always },                       // Wrath


    // ---------------------------------------------------------------------------------------------------
    // SPEC LISTS (levels 10-20, from the client one-button assistant tables and SimulationCraft; reports in
    // .agent/design/mechanics-2026-09-29/rotation-*.md). A list of the spec REPLACES the class kit above.
    // ---------------------------------------------------------------------------------------------------

    // WARRIOR Arms 71 (Garrick): Victory Rush after a kill, Overpower while its aura is missing, Mortal Strike,
    // Execute, Overpower, Slam above 50 rage; below 30 rage HOLD (keep rage for Mortal Strike, not Hamstring).
    { CLASS_WARRIOR, 71, 34428,  RotCond::SelfHealthBelow,   90 },                 // Victory Rush
    { CLASS_WARRIOR, 71, 202168, RotCond::SelfHealthBelow,   70 },                 // Impending Victory
    { CLASS_WARRIOR, 71, 7384,   RotCond::SelfLacksAura,     7384 },               // Overpower while its aura is missing
    { CLASS_WARRIOR, 71, 12294,  RotCond::Always },                                // Mortal Strike (2H weapon)
    { CLASS_WARRIOR, 71, 163201, RotCond::TargetHealthBelow, 20 },                 // Execute
    { CLASS_WARRIOR, 71, 7384,   RotCond::Always },                                // Overpower
    { CLASS_WARRIOR, 71, 1464,   RotCond::PowerAtLeast, POWER_RAGE, 500 },         // Slam above 50 rage
    { CLASS_WARRIOR, 71, 57755,  RotCond::Always },                                // Heroic Throw (8-30 yd only)
    { CLASS_WARRIOR, 71, 0,      RotCond::PowerBelow, POWER_RAGE, 300 },           // HOLD: keep the rage

    // WARRIOR Fury 72 (Zalko)
    { CLASS_WARRIOR, 72, 34428,  RotCond::SelfHealthBelow,   90 },                 // Victory Rush
    { CLASS_WARRIOR, 72, 202168, RotCond::SelfHealthBelow,   70 },                 // Impending Victory
    { CLASS_WARRIOR, 72, 184364, RotCond::SelfHealthBelow,   50, 0, true },        // Enraged Regeneration
    { CLASS_WARRIOR, 72, 5308,   RotCond::TargetHealthBelow, 20 },                 // Execute (Fury)
    { CLASS_WARRIOR, 72, 85288,  RotCond::Always },                                // Raging Blow
    { CLASS_WARRIOR, 72, 23881,  RotCond::Always },                                // Bloodthirst
    { CLASS_WARRIOR, 72, 1464,   RotCond::PowerAtLeast, POWER_RAGE, 400 },         // Slam above 40 rage
    { CLASS_WARRIOR, 72, 57755,  RotCond::Always },                                // Heroic Throw

    // PALADIN Retribution 70 (Aldric)
    { CLASS_PALADIN, 70, 642,    RotCond::SelfHealthBelow,   20, 0, true },        // Divine Shield
    { CLASS_PALADIN, 70, 633,    RotCond::SelfHealthBelow,   25, 0, true },        // Lay on Hands
    { CLASS_PALADIN, 70, 403876, RotCond::SelfHealthBelow,   60, 0, true },        // Divine Protection
    { CLASS_PALADIN, 70, 853,    RotCond::SelfHealthBelow,   50 },                 // Hammer of Justice
    { CLASS_PALADIN, 70, 85256,  RotCond::PowerAtLeast, POWER_HOLY_POWER, 3 },     // Templar's Verdict
    { CLASS_PALADIN, 70, 184575, RotCond::Always },                                // Blade of Justice
    { CLASS_PALADIN, 70, 20271,  RotCond::Always },                                // Judgment
    { CLASS_PALADIN, 70, 35395,  RotCond::Always },                                // Crusader Strike

    // HUNTER Beast Mastery 253 (Rowena). Kill Command 34026 stays off: the core gives it no effect.
    { CLASS_HUNTER,  253, 186265, RotCond::SelfHealthBelow,  30, 0, true },        // Aspect of the Turtle
    { CLASS_HUNTER,  253, 217200, RotCond::TargetLacksMyAura, 217200 },            // Barbed Shot
    { CLASS_HUNTER,  253, 193455, RotCond::Always },                               // Cobra Shot
    { CLASS_HUNTER,  253, 185358, RotCond::Always },                               // Arcane Shot
    { CLASS_HUNTER,  253, 56641,  RotCond::Always },                               // Steady Shot

    // HUNTER Marksmanship 254 (Jubaka)
    { CLASS_HUNTER,  254, 186265, RotCond::SelfHealthBelow,  30, 0, true },        // Aspect of the Turtle
    { CLASS_HUNTER,  254, 257044, RotCond::Always },                               // Rapid Fire
    { CLASS_HUNTER,  254, 19434,  RotCond::SelfLacksAura,    260242 },             // Aimed Shot unless Precise Shots is up
    { CLASS_HUNTER,  254, 185358, RotCond::Always },                               // Arcane Shot (takes Precise Shots)
    { CLASS_HUNTER,  254, 56641,  RotCond::Always },                               // Steady Shot

    // ROGUE Assassination 259 (Tayana)
    { CLASS_ROGUE,   259, 185311, RotCond::SelfHealthBelow,  50, 0, true },        // Crimson Vial
    { CLASS_ROGUE,   259, 8676,   RotCond::Always },                               // Ambush (stealth)
    { CLASS_ROGUE,   259, 1833,   RotCond::Always },                               // Cheap Shot (stealth)
    { CLASS_ROGUE,   259, 315496, RotCond::SelfLacksAuraFullCombo, 315496 },       // Slice and Dice
    { CLASS_ROGUE,   259, 1943,   RotCond::TargetLacksMyAuraFullCombo, 1943, 50 }, // Rupture at full points
    { CLASS_ROGUE,   259, 196819, RotCond::ComboFullOrDying },                     // Eviscerate
    { CLASS_ROGUE,   259, 703,    RotCond::TargetLacksMyAura, 703 },               // Garrote
    { CLASS_ROGUE,   259, 5938,   RotCond::Always },                               // Shiv
    { CLASS_ROGUE,   259, 1329,   RotCond::Always },                               // Mutilate (dagger)
    { CLASS_ROGUE,   259, 1752,   RotCond::Always },                               // Sinister Strike

    // ROGUE Outlaw 260 (Cecily)
    { CLASS_ROGUE,   260, 185311, RotCond::SelfHealthBelow,  50, 0, true },        // Crimson Vial
    { CLASS_ROGUE,   260, 13750,  RotCond::SelfLacksAura,    13750, 0, true },     // Adrenaline Rush
    { CLASS_ROGUE,   260, 8676,   RotCond::Always },                               // Ambush (stealth)
    { CLASS_ROGUE,   260, 1833,   RotCond::Always },                               // Cheap Shot (stealth)
    { CLASS_ROGUE,   260, 315496, RotCond::SelfLacksAuraFullCombo, 315496 },       // Slice and Dice
    { CLASS_ROGUE,   260, 315341, RotCond::ComboFullOrDying },                     // Between the Eyes
    { CLASS_ROGUE,   260, 196819, RotCond::ComboFullOrDying },                     // Eviscerate
    { CLASS_ROGUE,   260, 193315, RotCond::Always },                               // Sinister Strike (Outlaw)

    // PRIEST Shadow 258 (Adeline, Zulwara). Shadowform is a combat form (CombatForms): taken first in a fight.
    { CLASS_PRIEST,  258, 232698, RotCond::SelfLacksAura,    232698, 0, true },    // Shadowform
    { CLASS_PRIEST,  258, 19236,  RotCond::SelfHealthBelow,  40, 0, true },        // Desperate Prayer
    { CLASS_PRIEST,  258, 17,     RotCond::SelfLacksAura,    17, 80, true },       // Power Word: Shield under 80 %
    { CLASS_PRIEST,  258, 335467, RotCond::Always },                               // Devouring Plague
    { CLASS_PRIEST,  258, 34914,  RotCond::TargetLacksMyAura, 34914 },             // Vampiric Touch
    { CLASS_PRIEST,  258, 589,    RotCond::TargetLacksMyAura, 589 },               // Shadow Word: Pain
    { CLASS_PRIEST,  258, 8092,   RotCond::Always },                               // Mind Blast
    { CLASS_PRIEST,  258, 32379,  RotCond::TargetHealthBelow, 20 },                // Shadow Word: Death
    { CLASS_PRIEST,  258, 15407,  RotCond::Always },                               // Mind Flay

    // MAGE Fire 63 (Sennja)
    { CLASS_MAGE,    63, 235313,  RotCond::SelfLacksAura,    235313, 0, true },    // Blazing Barrier
    { CLASS_MAGE,    63, 11366,   RotCond::SelfHasAura,      48108 },              // Pyroblast on Hot Streak
    { CLASS_MAGE,    63, 108853,  RotCond::SelfHasAura,      48107 },              // Fire Blast on Heating Up
    { CLASS_MAGE,    63, 319836,  RotCond::Always },                               // Fire Blast
    { CLASS_MAGE,    63, 133,     RotCond::Always },                               // Fireball

    // MAGE Frost 64 (Emrick)
    { CLASS_MAGE,    64, 11426,   RotCond::SelfLacksAura,    11426, 0, true },     // Ice Barrier
    { CLASS_MAGE,    64, 30455,   RotCond::SelfHasAura,      44544 },              // Ice Lance on Fingers of Frost
    { CLASS_MAGE,    64, 44614,   RotCond::TargetLacksMyAura, 228358 },            // Flurry
    { CLASS_MAGE,    64, 30455,   RotCond::SelfHasAura,      205473, 5 },          // Ice Lance at 5 Icicles
    { CLASS_MAGE,    64, 116,     RotCond::Always },                               // Frostbolt

    // WARLOCK Affliction 265 (Deverel). B on DoT rows = do not open a DoT on a target under that health %.
    { CLASS_WARLOCK, 265, 104773, RotCond::SelfHealthBelow,  35, 0, true },        // Unending Resolve
    { CLASS_WARLOCK, 265, 234153, RotCond::SelfHealthBelow,  50 },                 // Drain Life
    { CLASS_WARLOCK, 265, 980,    RotCond::TargetLacksMyAura, 980 },               // Agony
    { CLASS_WARLOCK, 265, 172,    RotCond::TargetLacksMyAura, 146739, 20 },        // Corruption
    { CLASS_WARLOCK, 265, 316099, RotCond::TargetLacksMyAura, 316099, 20 },        // Unstable Affliction
    { CLASS_WARLOCK, 265, 324536, RotCond::PowerAtLeast, POWER_SOUL_SHARDS, 10, true }, // Malefic Rapture (self: 100 yd area)
    { CLASS_WARLOCK, 265, 686,    RotCond::Always },                               // Shadow Bolt

    // WARLOCK Destruction 267 (Voljara): 686 is Incinerate and 348 is Immolate here (the core swaps them).
    { CLASS_WARLOCK, 267, 104773, RotCond::SelfHealthBelow,  35, 0, true },        // Unending Resolve
    { CLASS_WARLOCK, 267, 234153, RotCond::SelfHealthBelow,  50 },                 // Drain Life
    { CLASS_WARLOCK, 267, 348,    RotCond::TargetLacksMyAura, 157736, 20 },        // Immolate
    { CLASS_WARLOCK, 267, 17962,  RotCond::SelfLacksAura,    117828 },             // Conflagrate while Backdraft is down
    { CLASS_WARLOCK, 267, 116858, RotCond::PowerAtLeast, POWER_SOUL_SHARDS, 20 },  // Chaos Bolt at 2 shards
    { CLASS_WARLOCK, 267, 686,    RotCond::Always },                               // Incinerate

    // MONK Windwalker 269 (Brienne, Bumbu): the last flag NoRepeat = never the same ability twice in a row
    // (Combo Strikes). Fists of Fury has no damage in this core and stays out.
    { CLASS_MONK,    269, 322101, RotCond::SelfHealthBelow,  60, 0, true },        // Expel Harm
    { CLASS_MONK,    269, 107428, RotCond::PowerAtLeast, POWER_CHI, 2, false, true }, // Rising Sun Kick
    { CLASS_MONK,    269, 100784, RotCond::PowerAtLeast, POWER_CHI, 3, false, true }, // Blackout Kick
    { CLASS_MONK,    269, 100780, RotCond::PowerBelow,   POWER_CHI, 4, false, true }, // Tiger Palm to build
    { CLASS_MONK,    269, 100780, RotCond::Always },                               // Tiger Palm fallback
    { CLASS_MONK,    269, 117952, RotCond::Always },                               // Crackling Jade Lightning

    // SHAMAN Elemental 262 (Nakuru). Storm Elemental does nothing in this core and stays out.
    { CLASS_SHAMAN,  262, 192106, RotCond::SelfLacksAura,    192106, 0, true },    // Lightning Shield
    { CLASS_SHAMAN,  262, 108271, RotCond::SelfHealthBelow,  45, 0, true },        // Astral Shift
    { CLASS_SHAMAN,  262, 470411, RotCond::TargetLacksMyAura, 188389 },            // Flame Shock
    { CLASS_SHAMAN,  262, 191634, RotCond::SelfLacksAura,    191634, 0, true },    // Stormkeeper
    { CLASS_SHAMAN,  262, 51505,  RotCond::SelfHasAura,      77762 },              // Lava Burst on Lava Surge
    { CLASS_SHAMAN,  262, 8042,   RotCond::PowerAtLeast, POWER_MAELSTROM, 60 },    // Earth Shock
    { CLASS_SHAMAN,  262, 51505,  RotCond::Always },                               // Lava Burst
    { CLASS_SHAMAN,  262, 188196, RotCond::Always },                               // Lightning Bolt

    // DRUID Feral 103 (Yalanda). Cat Thrash never applies its bleed in this core and stays out.
    { CLASS_DRUID,   103, 768,    RotCond::SelfLacksAura,    768, 0, true },       // Cat Form
    { CLASS_DRUID,   103, 22812,  RotCond::SelfHealthBelow,  50, 0, true },        // Barkskin
    { CLASS_DRUID,   103, 5217,   RotCond::PowerBelow, POWER_ENERGY, 45, true },   // Tiger's Fury
    { CLASS_DRUID,   103, 1079,   RotCond::TargetLacksMyAuraFullCombo, 1079, 30 }, // Rip at full points
    { CLASS_DRUID,   103, 22568,  RotCond::ComboFullOrDying },                     // Ferocious Bite
    { CLASS_DRUID,   103, 0,      RotCond::ComboFullOrDying },                     // HOLD: a finisher was wanted, do not spend points on builders
    { CLASS_DRUID,   103, 1822,   RotCond::TargetLacksMyAura, 155722 },            // Rake
    { CLASS_DRUID,   103, 5221,   RotCond::Always },                               // Shred
    { CLASS_DRUID,   103, 0,      RotCond::PowerBelow, POWER_ENERGY, 40 },         // HOLD: wait for Shred energy

    // DEATH KNIGHT / DEMON HUNTER / EVOKER: not in the roster; the generic pick serves them.
};

// Is this spell a NoRepeat step of the class? (what `CastMemory::LastNoRepeat` remembers)
inline constexpr bool IsNoRepeatSpell(uint8 cls, uint32 spell)
{
    for (RotStep const& s : Rotation)
        if (s.Class == cls && s.NoRepeat && s.Spell == spell)
            return true;
    return false;
}

// SELF BUFFS OUT OF COMBAT: what a player keeps up between fights (an hour-long buff, weapon poisons).
// Cast on self when the aura is missing, the character is idle and the core would accept the cast.
struct BuffStep
{
    uint8  Class;
    uint32 Spell;
    uint32 Aura;
    uint16 Spec = 0;    // 0 = any spec of the class; rows of the current spec, if any, replace the class rows
};

// FORMS TAKEN FOR A FIGHT and left after it (right click on the aura, CMSG_CANCEL_AURA): out of combat a
// shapeshifted player cannot take a taxi, use quest items or cast lock spells.
inline constexpr uint32 CombatForms[] = { 768, 232698 };   // Cat Form, Shadowform

// STEALTH ON THE APPROACH: cast when the victim is this close and the fight has not started, so the
// opener (Ambush, Cheap Shot) can land before the first auto attack breaks stealth.
inline constexpr uint32 OPENER_STEALTH_SPELL = 1784;   // Stealth
inline constexpr float  OPENER_STEALTH_YARDS = 30.0f;

inline constexpr BuffStep Buffs[] =
{
    { CLASS_WARRIOR, 6673,   6673   },  // Battle Shout
    { CLASS_PRIEST,  21562,  21562  },  // Power Word: Fortitude
    { CLASS_MAGE,    1459,   1459   },  // Arcane Intellect
    { CLASS_DRUID,   1126,   1126   },  // Mark of the Wild
    { CLASS_SHAMAN,  192106, 192106 },  // Lightning Shield
    { CLASS_ROGUE,   315584, 315584 },  // Instant Poison
    { CLASS_ROGUE,   3408,   3408   },  // Crippling Poison
};
}

#endif
