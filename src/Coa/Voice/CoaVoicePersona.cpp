/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaVoicePersona.h"
#include "CoaHash.h"
#include "Random.h"
#include "SharedDefines.h"
#include <algorithm>
#include <string_view>

namespace
{
// Friendly emotes a persona may favour. The order is part of the persona hash contract: never reorder it.
constexpr std::array<uint32, 12> FAVOURITE_PALETTE = {
    TEXT_EMOTE_WAVE, TEXT_EMOTE_BOW,  TEXT_EMOTE_SALUTE, TEXT_EMOTE_CHEER, TEXT_EMOTE_LAUGH,   TEXT_EMOTE_SMILE,
    TEXT_EMOTE_NOD,  TEXT_EMOTE_FLEX, TEXT_EMOTE_DANCE,  TEXT_EMOTE_WINK,  TEXT_EMOTE_CHUCKLE, TEXT_EMOTE_SHRUG};

constexpr uint8 QUIRK_COUNT = 5;
constexpr float FORMAL = 0.7f;

float Unit8(uint64 hash, uint32 shift) { return float((hash >> shift) & 0xFF) / 255.0f; }

bool EndsWith(std::string const& text, std::string_view suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void TrimRight(std::string& text)
{
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
}

// Percent roll on the world thread's RNG.
bool Roll(uint32 percent) { return urand(0, 99) < percent; }
}  // namespace

CoaVoicePersona CoaVoicePersona::For(ObjectGuid guid)
{
    static uint64 const salt = CoaHash::Bytes("coa-voice-persona-v1");
    uint64 const h = CoaHash::U64(guid.GetCounter(), salt);

    CoaVoicePersona persona;
    persona.hash = h;
    persona.archetype = uint8(h & 0xF);
    persona.chattiness = 0.2f + 0.8f * Unit8(h, 4);
    persona.formality = Unit8(h, 12);
    persona.emoteChance = 0.1f + 0.6f * Unit8(h, 20);

    for (uint8 i = 0; i < QUIRK_COUNT; ++i)
    {
        uint64 const pair = (h >> (28 + 2 * i)) & 0x3;
        if (pair == 0x3)
            persona.quirks |= uint8(1u << i);
    }
    if (persona.formality >= FORMAL)
        persona.quirks &= uint8(~(COA_QUIRK_LOWERCASE | COA_QUIRK_SMILEY | COA_QUIRK_LOL));

    // Three distinct favourites: a partial Fisher-Yates over the palette, driven by the hash's top 24 bits.
    std::array<uint32, FAVOURITE_PALETTE.size()> palette = FAVOURITE_PALETTE;
    uint64 draw = h >> 40;
    for (std::size_t i = 0; i < persona.favouriteEmotes.size(); ++i)
    {
        std::size_t const left = palette.size() - i;
        std::size_t const pick = i + std::size_t(draw % left);
        draw /= left;
        std::swap(palette[i], palette[pick]);
        persona.favouriteEmotes[i] = palette[i];
    }

    return persona;
}

std::string CoaVoicePersona::ApplyQuirks(std::string line, CoaIntent intent) const
{
    if (!quirks || line.empty())
        return line;

    if ((quirks & COA_QUIRK_NO_PERIOD) && EndsWith(line, ".") && !EndsWith(line, ".."))
    {
        line.pop_back();
        TrimRight(line);
    }

    if ((quirks & COA_QUIRK_ELLIPSIS) && !line.empty() && (line.back() == '.' || line.back() == '!') &&
        !EndsWith(line, "...") && Roll(20))
    {
        while (!line.empty() && (line.back() == '.' || line.back() == '!'))
            line.pop_back();
        TrimRight(line);
        if (!line.empty())
            line += "...";
    }

    if (quirks & COA_QUIRK_LOWERCASE)
        std::transform(line.begin(), line.end(), line.begin(),
                       [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; });

    // At most one suffix.
    bool suffixed = false;
    if ((quirks & COA_QUIRK_SMILEY) && !line.empty() && line.back() != '?' && Roll(30))
    {
        line += urand(0, 1) ? " :)" : " ^^";
        suffixed = true;
    }

    if (!suffixed && (quirks & COA_QUIRK_LOL) && !line.empty() &&
        (intent == CoaIntent::Greeting || intent == CoaIntent::Joke || intent == CoaIntent::Compliment) && Roll(30))
        line += " lol";

    return line;
}

bool CoaVoicePersona::Likes(uint32 textEmote) const
{
    return textEmote && std::find(favouriteEmotes.begin(), favouriteEmotes.end(), textEmote) != favouriteEmotes.end();
}
