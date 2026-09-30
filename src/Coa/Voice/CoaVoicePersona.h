/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAVOICEPERSONA_H
#define PLAYERBOTS_COAVOICEPERSONA_H

#include "CoaChatIntent.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <array>
#include <string>

// Writing habits a persona may have, applied to template lines by ApplyQuirks.
enum CoaVoiceQuirk : uint8
{
    COA_QUIRK_LOWERCASE = 0x01,  // writes without capitals (ASCII letters only)
    COA_QUIRK_SMILEY = 0x02,     // sometimes ends with " :)" or " ^^"
    COA_QUIRK_LOL = 0x04,        // sometimes ends a greeting or a joke with " lol" / " mdr"
    COA_QUIRK_NO_PERIOD = 0x08,  // drops a final '.'
    COA_QUIRK_ELLIPSIS = 0x10    // sometimes ends with "..."
};

// Stable traits of one bot, from an FNV-1a hash of its GUID counter and a fixed salt: no database row, the
// same after every restart. Pure: any thread.
struct CoaVoicePersona
{
    uint64 hash = 0;
    uint8 archetype = 0;       // 0..15: jaded veteran, eager novice, joker, roleplayer, trader, taciturn...
    float chattiness = 0.6f;   // 0.2..1.0: adds 20 x chattiness to the candidate score
    float formality = 0.5f;    // 0..1: at 0.7 or more no lowercase, smiley or "lol" quirk is applied
    float emoteChance = 0.3f;  // 0.1..0.7: chance to play an emote with an answer
    uint8 quirks = 0;          // CoaVoiceQuirk bits
    std::array<uint32, 3> favouriteEmotes{};  // TextEmotes ids, drawn from a fixed friendly palette

    // Traits of the bot with that GUID. Same GUID, same persona.
    static CoaVoicePersona For(ObjectGuid guid);

    // The line with this persona's quirks applied. Uses urand for the "sometimes" rolls, so call it on the
    // world thread; the caller sanitizes (and cuts) the result afterwards.
    std::string ApplyQuirks(std::string line, CoaIntent intent) const;

    // True when `textEmote` is one of the favourites.
    bool Likes(uint32 textEmote) const;
};

#endif
