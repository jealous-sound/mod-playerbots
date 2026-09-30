/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TEXTMODELCAPS_H
#define PLAYERBOTS_TEXTMODELCAPS_H

#include <string_view>

// What the Anthropic Messages API accepts for one model, from the plan's capability table. Sending a parameter a
// model rejects is a 400 that would disable the text situation for the day, so the table decides what the request
// holds; a model id it does not know gets nothing beyond model, max_tokens, system and messages.
//
//   model id           output_config.effort   temperature/top_p/top_k   thinking   server-side fallbacks
//   claude-opus-5      "low"                  never (400)               omitted    beta + "fallbacks":"default"
//   claude-sonnet-5    "low"                  never (400)               omitted    none
//   claude-haiku-4-5   never (errors)         allowed                   omitted    none
//   any other id       not sent               not sent                  omitted    none
//
// "thinking" is never sent for any model. Pure; any thread.
struct TextModelCaps
{
    bool known = false;
    bool effort = false;           // send output_config.effort
    bool sampling = false;         // temperature may be sent (Text.Temperature)
    bool serverFallbacks = false;  // the fallbacks beta may be sent (Text.Anthropic.ServerFallbacks)
    double priceInPerMTok = 0.0;   // USD per million input tokens (list price); the default of Text.PriceInPerMTok
    double priceOutPerMTok = 0.0;  // USD per million output tokens
    char const* family = "";       // "claude-opus-5", "claude-sonnet-5", "claude-haiku-4-5", or ""

    // The row for a model id: an exact match, or the family name followed by "-" and eight digits (a dated
    // snapshot such as claude-haiku-4-5-20251001). Prices: opus 5 / 25, sonnet 2 / 10, haiku 1 / 5.
    static TextModelCaps For(std::string_view modelId);
};

#endif
