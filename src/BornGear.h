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
 * Constellation -- the born kit gear: what a companion born at level 10 or 20 wears.
 *
 * GENERATED (operator 2026-09-30: level groups "в шмоте для их уровня, это важно"). The selector
 * reads the client data of 11.2.7: armor type and weapon skill of the class, the spec stat rule
 * (ItemSpec), required level, and an effective item level near `gt/ItemLevelByLevel` for the level
 * (13 at 10, 68 at 20), items that come from quest rewards, plain vendors or loot, created in the
 * Quest_Reward context so the old-world scaling lists fit them to the owner. Research and the
 * selector: .agent/design/level-groups-plan-2026-09-30.md, .agent/tmp/levelgroups-gear.md.
 * Order matters inside a set: armor, cloak, neck, rings, main hand, then off hand (an off hand
 * needs the spec spell that lets the character dual wield, so it may stay in the bag until
 * `EquipUpgrades` moves it).
 */

#ifndef CONSTELLATION_BORNGEAR_H
#define CONSTELLATION_BORNGEAR_H

#include "Define.h"

namespace Constellation
{
struct BornItem
{
    uint8  Level;
    uint8  Race;
    uint8  Class;
    uint16 Spec;
    uint32 Item;
};

inline constexpr BornItem BornGear[] =
{
    // level 10 race 1 class 1 spec 71 (Garrick)
    { 10, 1, 1, 71, 6971 },   // head
    { 10, 1, 1, 71, 4443 },   // shoulder
    { 10, 1, 1, 71, 3733 },   // chest
    { 10, 1, 1, 71, 7107 },   // waist
    { 10, 1, 1, 71, 2906 },   // legs
    { 10, 1, 1, 71, 2910 },   // feet
    { 10, 1, 1, 71, 15459 },   // wrist
    { 10, 1, 1, 71, 3559 },   // hands
    { 10, 1, 1, 71, 4792 },   // cloak
    { 10, 1, 1, 71, 5615 },   // mainhand
    // level 10 race 1 class 2 spec 70 (Aldric)
    { 10, 1, 2, 70, 1282 },   // head
    { 10, 1, 2, 70, 4443 },   // shoulder
    { 10, 1, 2, 70, 3733 },   // chest
    { 10, 1, 2, 70, 7107 },   // waist
    { 10, 1, 2, 70, 2545 },   // legs
    { 10, 1, 2, 70, 2910 },   // feet
    { 10, 1, 2, 70, 6722 },   // wrist
    { 10, 1, 2, 70, 3559 },   // hands
    { 10, 1, 2, 70, 4799 },   // cloak
    { 10, 1, 2, 70, 4817 },   // mainhand
    // level 10 race 1 class 3 spec 253 (Rowena)
    { 10, 1, 3, 253, 10743 },   // head
    { 10, 1, 3, 253, 4123 },   // shoulder
    { 10, 1, 3, 253, 6502 },   // chest
    { 10, 1, 3, 253, 11861 },   // waist
    { 10, 1, 3, 253, 59027 },   // legs
    { 10, 1, 3, 253, 6666 },   // feet
    { 10, 1, 3, 253, 6665 },   // wrist
    { 10, 1, 3, 253, 11867 },   // hands
    { 10, 1, 3, 253, 4792 },   // cloak
    { 10, 1, 3, 253, 15205 },   // mainhand
    // level 10 race 1 class 4 spec 260 (Cecily)
    { 10, 1, 4, 260, 6720 },   // head
    { 10, 1, 4, 260, 53158 },   // shoulder
    { 10, 1, 4, 260, 3585 },   // chest
    { 10, 1, 4, 260, 5609 },   // waist
    { 10, 1, 4, 260, 6480 },   // legs
    { 10, 1, 4, 260, 5311 },   // feet
    { 10, 1, 4, 260, 59026 },   // wrist
    { 10, 1, 4, 260, 5629 },   // hands
    { 10, 1, 4, 260, 4799 },   // cloak
    { 10, 1, 4, 260, 16891 },   // mainhand
    { 10, 1, 4, 260, 5757 },   // offhand
    // level 10 race 1 class 5 spec 258 (Adeline)
    { 10, 1, 5, 258, 53157 },   // head
    { 10, 1, 5, 258, 6664 },   // shoulder
    { 10, 1, 5, 258, 6503 },   // chest
    { 10, 1, 5, 258, 4786 },   // waist
    { 10, 1, 5, 258, 6659 },   // legs
    { 10, 1, 5, 258, 6191 },   // feet
    { 10, 1, 5, 258, 16981 },   // wrist
    { 10, 1, 5, 258, 1304 },   // hands
    { 10, 1, 5, 258, 4799 },   // cloak
    { 10, 1, 5, 258, 2908 },   // mainhand ______-___
    { 10, 1, 5, 258, 15206 },   // offhand
    // level 10 race 1 class 8 spec 64 (Emrick)
    { 10, 1, 8, 64, 53157 },   // head
    { 10, 1, 8, 64, 17695 },   // shoulder
    { 10, 1, 8, 64, 6503 },   // chest
    { 10, 1, 8, 64, 4786 },   // waist
    { 10, 1, 8, 64, 6659 },   // legs
    { 10, 1, 8, 64, 6191 },   // feet
    { 10, 1, 8, 64, 16981 },   // wrist
    { 10, 1, 8, 64, 1304 },   // hands
    { 10, 1, 8, 64, 4792 },   // cloak
    { 10, 1, 8, 64, 16891 },   // mainhand
    { 10, 1, 8, 64, 15206 },   // offhand
    // level 10 race 1 class 9 spec 265 (Deverel)
    { 10, 1, 9, 265, 3556 },   // head
    { 10, 1, 9, 265, 10657 },   // shoulder
    { 10, 1, 9, 265, 4781 },   // chest
    { 10, 1, 9, 265, 11936 },   // waist ________ - ____ ________
    { 10, 1, 9, 265, 6659 },   // legs
    { 10, 1, 9, 265, 6191 },   // feet
    { 10, 1, 9, 265, 16981 },   // wrist
    { 10, 1, 9, 265, 3565 },   // hands
    { 10, 1, 9, 265, 4799 },   // cloak
    { 10, 1, 9, 265, 16891 },   // mainhand
    { 10, 1, 9, 265, 15206 },   // offhand
    // level 10 race 1 class 10 spec 269 (Brienne)
    { 10, 1, 10, 269, 6720 },   // head
    { 10, 1, 10, 269, 53158 },   // shoulder
    { 10, 1, 10, 269, 3585 },   // chest
    { 10, 1, 10, 269, 5609 },   // waist
    { 10, 1, 10, 269, 1310 },   // legs
    { 10, 1, 10, 269, 5311 },   // feet
    { 10, 1, 10, 269, 1306 },   // wrist
    { 10, 1, 10, 269, 5299 },   // hands
    { 10, 1, 10, 269, 4792 },   // cloak
    { 10, 1, 10, 269, 16891 },   // mainhand
    { 10, 1, 10, 269, 5757 },   // offhand
    // level 10 race 8 class 1 spec 72 (Zalko)
    { 10, 8, 1, 72, 6971 },   // head
    { 10, 8, 1, 72, 4443 },   // shoulder
    { 10, 8, 1, 72, 3733 },   // chest
    { 10, 8, 1, 72, 3758 },   // waist
    { 10, 8, 1, 72, 2906 },   // legs
    { 10, 8, 1, 72, 2910 },   // feet
    { 10, 8, 1, 72, 6722 },   // wrist
    { 10, 8, 1, 72, 3559 },   // hands
    { 10, 8, 1, 72, 4799 },   // cloak
    { 10, 8, 1, 72, 5757 },   // mainhand
    { 10, 8, 1, 72, 6360 },   // offhand _______________ _______-____
    // level 10 race 8 class 3 spec 254 (Jubaka)
    { 10, 8, 3, 254, 10743 },   // head
    { 10, 8, 3, 254, 4123 },   // shoulder
    { 10, 8, 3, 254, 6502 },   // chest
    { 10, 8, 3, 254, 11861 },   // waist
    { 10, 8, 3, 254, 59027 },   // legs
    { 10, 8, 3, 254, 6666 },   // feet
    { 10, 8, 3, 254, 6665 },   // wrist
    { 10, 8, 3, 254, 11867 },   // hands
    { 10, 8, 3, 254, 4799 },   // cloak
    { 10, 8, 3, 254, 15205 },   // mainhand
    // level 10 race 8 class 4 spec 259 (Tayana)
    { 10, 8, 4, 259, 6720 },   // head
    { 10, 8, 4, 259, 53158 },   // shoulder
    { 10, 8, 4, 259, 3585 },   // chest
    { 10, 8, 4, 259, 5609 },   // waist
    { 10, 8, 4, 259, 1310 },   // legs
    { 10, 8, 4, 259, 5311 },   // feet
    { 10, 8, 4, 259, 1306 },   // wrist
    { 10, 8, 4, 259, 5629 },   // hands
    { 10, 8, 4, 259, 4799 },   // cloak
    { 10, 8, 4, 259, 2908 },   // mainhand ______-___
    { 10, 8, 4, 259, 5627 },   // offhand
    // level 10 race 8 class 5 spec 258 (Zulwara)
    { 10, 8, 5, 258, 53157 },   // head
    { 10, 8, 5, 258, 6664 },   // shoulder
    { 10, 8, 5, 258, 4781 },   // chest
    { 10, 8, 5, 258, 4786 },   // waist
    { 10, 8, 5, 258, 6659 },   // legs
    { 10, 8, 5, 258, 6191 },   // feet
    { 10, 8, 5, 258, 16981 },   // wrist
    { 10, 8, 5, 258, 1304 },   // hands
    { 10, 8, 5, 258, 4792 },   // cloak
    { 10, 8, 5, 258, 2908 },   // mainhand ______-___
    { 10, 8, 5, 258, 15206 },   // offhand
    // level 10 race 8 class 7 spec 262 (Nakuru)
    { 10, 8, 7, 262, 10743 },   // head
    { 10, 8, 7, 262, 4123 },   // shoulder
    { 10, 8, 7, 262, 6502 },   // chest
    { 10, 8, 7, 262, 11861 },   // waist
    { 10, 8, 7, 262, 59027 },   // legs
    { 10, 8, 7, 262, 6666 },   // feet
    { 10, 8, 7, 262, 6665 },   // wrist
    { 10, 8, 7, 262, 11867 },   // hands
    { 10, 8, 7, 262, 4792 },   // cloak
    { 10, 8, 7, 262, 5627 },   // mainhand
    { 10, 8, 7, 262, 4822 },   // offhand
    // level 10 race 8 class 8 spec 63 (Sennja)
    { 10, 8, 8, 63, 53157 },   // head
    { 10, 8, 8, 63, 10657 },   // shoulder
    { 10, 8, 8, 63, 6503 },   // chest
    { 10, 8, 8, 63, 4786 },   // waist
    { 10, 8, 8, 63, 6659 },   // legs
    { 10, 8, 8, 63, 6191 },   // feet
    { 10, 8, 8, 63, 16981 },   // wrist
    { 10, 8, 8, 63, 3565 },   // hands
    { 10, 8, 8, 63, 4799 },   // cloak
    { 10, 8, 8, 63, 16891 },   // mainhand
    { 10, 8, 8, 63, 15206 },   // offhand
    // level 10 race 8 class 9 spec 267 (Voljara)
    { 10, 8, 9, 267, 3556 },   // head
    { 10, 8, 9, 267, 17695 },   // shoulder
    { 10, 8, 9, 267, 3752 },   // chest
    { 10, 8, 9, 267, 11936 },   // waist ________ - ____ ________
    { 10, 8, 9, 267, 6659 },   // legs
    { 10, 8, 9, 267, 6191 },   // feet
    { 10, 8, 9, 267, 16981 },   // wrist
    { 10, 8, 9, 267, 1304 },   // hands
    { 10, 8, 9, 267, 4799 },   // cloak
    { 10, 8, 9, 267, 16891 },   // mainhand
    { 10, 8, 9, 267, 15206 },   // offhand
    // level 10 race 8 class 10 spec 269 (Bumbu)
    { 10, 8, 10, 269, 6720 },   // head
    { 10, 8, 10, 269, 53158 },   // shoulder
    { 10, 8, 10, 269, 3585 },   // chest
    { 10, 8, 10, 269, 5609 },   // waist
    { 10, 8, 10, 269, 1310 },   // legs
    { 10, 8, 10, 269, 5311 },   // feet
    { 10, 8, 10, 269, 59026 },   // wrist
    { 10, 8, 10, 269, 5299 },   // hands
    { 10, 8, 10, 269, 4792 },   // cloak
    { 10, 8, 10, 269, 16891 },   // mainhand
    { 10, 8, 10, 269, 5757 },   // offhand
    // level 10 race 8 class 11 spec 103 (Yalanda)
    { 10, 8, 11, 103, 6720 },   // head
    { 10, 8, 11, 103, 53158 },   // shoulder
    { 10, 8, 11, 103, 3585 },   // chest
    { 10, 8, 11, 103, 5609 },   // waist
    { 10, 8, 11, 103, 6480 },   // legs
    { 10, 8, 11, 103, 5311 },   // feet
    { 10, 8, 11, 103, 1306 },   // wrist
    { 10, 8, 11, 103, 5630 },   // hands
    { 10, 8, 11, 103, 4792 },   // cloak
    { 10, 8, 11, 103, 16894 },   // mainhand
    // level 20 race 1 class 1 spec 71 (Garrick)
    { 20, 1, 1, 71, 13073 },   // head ____ ___-____
    { 20, 1, 1, 71, 13066 },   // shoulder
    { 20, 1, 1, 71, 14624 },   // chest
    { 20, 1, 1, 71, 13145 },   // waist
    { 20, 1, 1, 71, 13074 },   // legs
    { 20, 1, 1, 71, 13068 },   // feet
    { 20, 1, 1, 71, 13076 },   // wrist
    { 20, 1, 1, 71, 13071 },   // hands
    { 20, 1, 1, 71, 12979 },   // cloak
    { 20, 1, 1, 71, 13087 },   // neck
    { 20, 1, 1, 71, 13095 },   // finger1
    { 20, 1, 1, 71, 12996 },   // finger2
    { 20, 1, 1, 71, 13054 },   // mainhand
    // level 20 race 1 class 2 spec 70 (Aldric)
    { 20, 1, 2, 70, 13073 },   // head ____ ___-____
    { 20, 1, 2, 70, 13066 },   // shoulder
    { 20, 1, 2, 70, 82885 },   // chest
    { 20, 1, 2, 70, 13145 },   // waist
    { 20, 1, 2, 70, 13074 },   // legs
    { 20, 1, 2, 70, 13068 },   // feet
    { 20, 1, 2, 70, 13076 },   // wrist
    { 20, 1, 2, 70, 13071 },   // hands
    { 20, 1, 2, 70, 12979 },   // cloak
    { 20, 1, 2, 70, 13088 },   // neck
    { 20, 1, 2, 70, 2951 },   // finger1
    { 20, 1, 2, 70, 12996 },   // finger2
    { 20, 1, 2, 70, 1722 },   // mainhand
    // level 20 race 1 class 3 spec 253 (Rowena)
    { 20, 1, 3, 253, 13127 },   // head
    { 20, 1, 3, 253, 13132 },   // shoulder
    { 20, 1, 3, 253, 1715 },   // chest
    { 20, 1, 3, 253, 13134 },   // waist
    { 20, 1, 3, 253, 13010 },   // legs
    { 20, 1, 3, 253, 13124 },   // feet
    { 20, 1, 3, 253, 13199 },   // wrist
    { 20, 1, 3, 253, 12994 },   // hands
    { 20, 1, 3, 253, 2059 },   // cloak
    { 20, 1, 3, 253, 13084 },   // neck
    { 20, 1, 3, 253, 13093 },   // finger1
    { 20, 1, 3, 253, 13095 },   // finger2
    { 20, 1, 3, 253, 13137 },   // mainhand
    // level 20 race 1 class 4 spec 260 (Cecily)
    { 20, 1, 4, 260, 3020 },   // head
    { 20, 1, 4, 260, 13115 },   // shoulder
    { 20, 1, 4, 260, 12988 },   // chest
    { 20, 1, 4, 260, 13011 },   // waist
    { 20, 1, 4, 260, 13114 },   // legs
    { 20, 1, 4, 260, 1121 },   // feet
    { 20, 1, 4, 260, 13119 },   // wrist
    { 20, 1, 4, 260, 2564 },   // hands
    { 20, 1, 4, 260, 13108 },   // cloak
    { 20, 1, 4, 260, 13084 },   // neck
    { 20, 1, 4, 260, 12996 },   // finger1
    { 20, 1, 4, 260, 13093 },   // finger2
    { 20, 1, 4, 260, 2878 },   // mainhand
    { 20, 1, 4, 260, 13032 },   // offhand
    // level 20 race 1 class 5 spec 258 (Adeline)
    { 20, 1, 5, 258, 2721 },   // head
    { 20, 1, 5, 258, 12998 },   // shoulder
    { 20, 1, 5, 258, 2800 },   // chest
    { 20, 1, 5, 258, 2911 },   // waist
    { 20, 1, 5, 258, 13008 },   // legs
    { 20, 1, 5, 258, 13100 },   // feet
    { 20, 1, 5, 258, 13106 },   // wrist
    { 20, 1, 5, 258, 9395 },   // hands
    { 20, 1, 5, 258, 13005 },   // cloak
    { 20, 1, 5, 258, 1714 },   // neck
    { 20, 1, 5, 258, 12985 },   // finger1
    { 20, 1, 5, 258, 12996 },   // finger2
    { 20, 1, 5, 258, 13048 },   // mainhand
    { 20, 1, 5, 258, 13030 },   // offhand
    // level 20 race 1 class 8 spec 64 (Emrick)
    { 20, 1, 8, 64, 2721 },   // head
    { 20, 1, 8, 64, 13103 },   // shoulder
    { 20, 1, 8, 64, 1716 },   // chest
    { 20, 1, 8, 64, 13105 },   // waist
    { 20, 1, 8, 64, 2277 },   // legs
    { 20, 1, 8, 64, 13100 },   // feet
    { 20, 1, 8, 64, 9433 },   // wrist
    { 20, 1, 8, 64, 9395 },   // hands
    { 20, 1, 8, 64, 12979 },   // cloak
    { 20, 1, 8, 64, 13084 },   // neck
    { 20, 1, 8, 64, 13094 },   // finger1
    { 20, 1, 8, 64, 12996 },   // finger2
    { 20, 1, 8, 64, 4091 },   // mainhand
    { 20, 1, 8, 64, 13030 },   // offhand
    // level 20 race 1 class 9 spec 265 (Deverel)
    { 20, 1, 9, 265, 2721 },   // head
    { 20, 1, 9, 265, 13103 },   // shoulder
    { 20, 1, 9, 265, 2800 },   // chest
    { 20, 1, 9, 265, 13105 },   // waist
    { 20, 1, 9, 265, 2277 },   // legs
    { 20, 1, 9, 265, 13099 },   // feet
    { 20, 1, 9, 265, 9433 },   // wrist
    { 20, 1, 9, 265, 12977 },   // hands
    { 20, 1, 9, 265, 13005 },   // cloak
    { 20, 1, 9, 265, 13085 },   // neck
    { 20, 1, 9, 265, 13094 },   // finger1
    { 20, 1, 9, 265, 12996 },   // finger2
    { 20, 1, 9, 265, 4091 },   // mainhand
    { 20, 1, 9, 265, 13030 },   // offhand
    // level 20 race 1 class 10 spec 269 (Brienne)
    { 20, 1, 10, 269, 13112 },   // head
    { 20, 1, 10, 269, 13115 },   // shoulder
    { 20, 1, 10, 269, 12988 },   // chest
    { 20, 1, 10, 269, 13117 },   // waist
    { 20, 1, 10, 269, 1718 },   // legs
    { 20, 1, 10, 269, 1121 },   // feet
    { 20, 1, 10, 269, 12999 },   // wrist
    { 20, 1, 10, 269, 720 },   // hands
    { 20, 1, 10, 269, 2059 },   // cloak
    { 20, 1, 10, 269, 13089 },   // neck
    { 20, 1, 10, 269, 13097 },   // finger1
    { 20, 1, 10, 269, 13093 },   // finger2
    { 20, 1, 10, 269, 1265 },   // mainhand
    { 20, 1, 10, 269, 13033 },   // offhand
    // level 20 race 8 class 1 spec 72 (Zalko)
    { 20, 8, 1, 72, 13073 },   // head ____ ___-____
    { 20, 8, 1, 72, 13066 },   // shoulder
    { 20, 8, 1, 72, 11678 },   // chest _______ ____'___
    { 20, 8, 1, 72, 13145 },   // waist
    { 20, 8, 1, 72, 13074 },   // legs
    { 20, 8, 1, 72, 13068 },   // feet
    { 20, 8, 1, 72, 13076 },   // wrist
    { 20, 8, 1, 72, 13071 },   // hands
    { 20, 8, 1, 72, 13005 },   // cloak
    { 20, 8, 1, 72, 13088 },   // neck
    { 20, 8, 1, 72, 2951 },   // finger1
    { 20, 8, 1, 72, 13095 },   // finger2
    { 20, 8, 1, 72, 2256 },   // mainhand
    { 20, 8, 1, 72, 12976 },   // offhand
    // level 20 race 8 class 3 spec 254 (Jubaka)
    { 20, 8, 3, 254, 13127 },   // head
    { 20, 8, 3, 254, 13132 },   // shoulder
    { 20, 8, 3, 254, 1715 },   // chest
    { 20, 8, 3, 254, 12978 },   // waist
    { 20, 8, 3, 254, 13129 },   // legs
    { 20, 8, 3, 254, 12982 },   // feet
    { 20, 8, 3, 254, 13199 },   // wrist
    { 20, 8, 3, 254, 12994 },   // hands
    { 20, 8, 3, 254, 13005 },   // cloak
    { 20, 8, 3, 254, 13084 },   // neck
    { 20, 8, 3, 254, 13093 },   // finger1
    { 20, 8, 3, 254, 13095 },   // finger2
    { 20, 8, 3, 254, 13019 },   // mainhand
    // level 20 race 8 class 4 spec 259 (Tayana)
    { 20, 8, 4, 259, 3020 },   // head
    { 20, 8, 4, 259, 2278 },   // shoulder
    { 20, 8, 4, 259, 12988 },   // chest
    { 20, 8, 4, 259, 13011 },   // waist
    { 20, 8, 4, 259, 1718 },   // legs
    { 20, 8, 4, 259, 2276 },   // feet
    { 20, 8, 4, 259, 12999 },   // wrist
    { 20, 8, 4, 259, 2564 },   // hands
    { 20, 8, 4, 259, 13121 },   // cloak
    { 20, 8, 4, 259, 13089 },   // neck
    { 20, 8, 4, 259, 12996 },   // finger1
    { 20, 8, 4, 259, 13093 },   // finger2
    { 20, 8, 4, 259, 2912 },   // mainhand
    { 20, 8, 4, 259, 8006 },   // offhand
    // level 20 race 8 class 5 spec 258 (Zulwara)
    { 20, 8, 5, 258, 2721 },   // head
    { 20, 8, 5, 258, 12998 },   // shoulder
    { 20, 8, 5, 258, 1716 },   // chest
    { 20, 8, 5, 258, 2911 },   // waist
    { 20, 8, 5, 258, 13008 },   // legs
    { 20, 8, 5, 258, 13100 },   // feet
    { 20, 8, 5, 258, 9433 },   // wrist
    { 20, 8, 5, 258, 9395 },   // hands
    { 20, 8, 5, 258, 12979 },   // cloak
    { 20, 8, 5, 258, 13085 },   // neck
    { 20, 8, 5, 258, 12996 },   // finger1
    { 20, 8, 5, 258, 13094 },   // finger2
    { 20, 8, 5, 258, 2236 },   // mainhand
    { 20, 8, 5, 258, 13030 },   // offhand
    // level 20 race 8 class 7 spec 262 (Nakuru)
    { 20, 8, 7, 262, 13128 },   // head
    { 20, 8, 7, 262, 13131 },   // shoulder
    { 20, 8, 7, 262, 1715 },   // chest
    { 20, 8, 7, 262, 13134 },   // waist
    { 20, 8, 7, 262, 13010 },   // legs
    { 20, 8, 7, 262, 13125 },   // feet
    { 20, 8, 7, 262, 13199 },   // wrist
    { 20, 8, 7, 262, 9435 },   // hands
    { 20, 8, 7, 262, 12979 },   // cloak
    { 20, 8, 7, 262, 13084 },   // neck
    { 20, 8, 7, 262, 12996 },   // finger1
    { 20, 8, 7, 262, 12985 },   // finger2
    { 20, 8, 7, 262, 12990 },   // mainhand
    { 20, 8, 7, 262, 13079 },   // offhand
    // level 20 race 8 class 8 spec 63 (Sennja)
    { 20, 8, 8, 63, 13102 },   // head
    { 20, 8, 8, 63, 13103 },   // shoulder
    { 20, 8, 8, 63, 17050 },   // chest
    { 20, 8, 8, 63, 2911 },   // waist
    { 20, 8, 8, 63, 12987 },   // legs
    { 20, 8, 8, 63, 13100 },   // feet
    { 20, 8, 8, 63, 9433 },   // wrist
    { 20, 8, 8, 63, 12977 },   // hands
    { 20, 8, 8, 63, 13005 },   // cloak
    { 20, 8, 8, 63, 13085 },   // neck
    { 20, 8, 8, 63, 12996 },   // finger1
    { 20, 8, 8, 63, 13094 },   // finger2
    { 20, 8, 8, 63, 935 },   // mainhand
    { 20, 8, 8, 63, 13030 },   // offhand
    // level 20 race 8 class 9 spec 267 (Voljara)
    { 20, 8, 9, 267, 13102 },   // head
    { 20, 8, 9, 267, 13103 },   // shoulder
    { 20, 8, 9, 267, 9434 },   // chest
    { 20, 8, 9, 267, 13105 },   // waist
    { 20, 8, 9, 267, 2277 },   // legs
    { 20, 8, 9, 267, 13100 },   // feet
    { 20, 8, 9, 267, 9433 },   // wrist
    { 20, 8, 9, 267, 12977 },   // hands
    { 20, 8, 9, 267, 13005 },   // cloak
    { 20, 8, 9, 267, 13085 },   // neck
    { 20, 8, 9, 267, 12985 },   // finger1
    { 20, 8, 9, 267, 13094 },   // finger2
    { 20, 8, 9, 267, 13035 },   // mainhand
    { 20, 8, 9, 267, 2879 },   // offhand
    // level 20 race 8 class 10 spec 269 (Bumbu)
    { 20, 8, 10, 269, 13112 },   // head
    { 20, 8, 10, 269, 13115 },   // shoulder
    { 20, 8, 10, 269, 12988 },   // chest
    { 20, 8, 10, 269, 13117 },   // waist
    { 20, 8, 10, 269, 1718 },   // legs
    { 20, 8, 10, 269, 2276 },   // feet
    { 20, 8, 10, 269, 13119 },   // wrist
    { 20, 8, 10, 269, 720 },   // hands
    { 20, 8, 10, 269, 12979 },   // cloak
    { 20, 8, 10, 269, 13089 },   // neck
    { 20, 8, 10, 269, 13093 },   // finger1
    { 20, 8, 10, 269, 13097 },   // finger2
    { 20, 8, 10, 269, 13035 },   // mainhand
    { 20, 8, 10, 269, 4090 },   // offhand
    // level 20 race 8 class 11 spec 103 (Yalanda)
    { 20, 8, 11, 103, 13112 },   // head
    { 20, 8, 11, 103, 2278 },   // shoulder
    { 20, 8, 11, 103, 12988 },   // chest
    { 20, 8, 11, 103, 13117 },   // waist
    { 20, 8, 11, 103, 13114 },   // legs
    { 20, 8, 11, 103, 1121 },   // feet
    { 20, 8, 11, 103, 12999 },   // wrist
    { 20, 8, 11, 103, 720 },   // hands
    { 20, 8, 11, 103, 13121 },   // cloak
    { 20, 8, 11, 103, 13089 },   // neck
    { 20, 8, 11, 103, 12996 },   // finger1
    { 20, 8, 11, 103, 2951 },   // finger2
    { 20, 8, 11, 103, 13046 },   // mainhand
};
}

#endif
