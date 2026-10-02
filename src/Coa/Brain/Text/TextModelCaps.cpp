/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TextModelCaps.h"
#include <algorithm>

namespace
{
bool Matches(std::string_view model, std::string_view family)
{
    if (model == family)
        return true;
    return model.size() == family.size() + 9 && model.substr(0, family.size()) == family &&
           model[family.size()] == '-' &&
           std::all_of(model.begin() + family.size() + 1, model.end(), [](char c) { return c >= '0' && c <= '9'; });
}
}

TextModelCaps TextModelCaps::For(std::string_view modelId)
{
    if (Matches(modelId, "claude-opus-5"))
        return {true, true, false, true, 5.0, 25.0, "claude-opus-5"};
    if (Matches(modelId, "claude-sonnet-5"))
        return {true, true, false, false, 2.0, 10.0, "claude-sonnet-5"};
    if (Matches(modelId, "claude-haiku-4-5"))
        return {true, false, true, false, 1.0, 5.0, "claude-haiku-4-5"};
    return {};
}
