/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINCHEATSHEET_H
#define PLAYERBOTS_BRAINCHEATSHEET_H

#include "Define.h"
#include <string>

// The CoA cheat sheet of the text prompt: the 21 custom classes and their specializations, generated from the
// fork's own data (CoaSpecStrategies[] in src/Ai/Coa/CoaSpecStrategies.h, which is generated from the same source as
// the rotations, and CoaClassName()), so a model that knows nothing of "Conquest of Azeroth" can still talk about a
// Reaper or a Tinker. No Player is needed: the data is static.
namespace BrainCheatSheet
{
// One line per class in class-id order (12 to 32), joined by '\n', no trailing newline:
//   "Barbarian: Brutality (dps, melee), Ancestry (dps, melee, support), Headhunting (dps, ranged)"
// Role words "dps", "tank", "healer"; "melee" for position "close", "ranged" for "ranged"; ", support" for a spec
// whose row says it carries the group. Specs in table order. Built once (a thread-safe function-local static) and
// byte-stable for the life of the process: it is part of the stable, cacheable prefix of every text prompt.
std::string const& Text();

// The line of one class ("" when classId is not a CoA class). Pure; same format as a line of Text().
std::string ClassLine(uint8 classId);
}  // namespace BrainCheatSheet

#endif
