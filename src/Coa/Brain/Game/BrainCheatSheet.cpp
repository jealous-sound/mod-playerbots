/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainCheatSheet.h"
#include "CoaSpecialization.h"
#include "CoaSpecStrategies.h"
#include <string_view>

std::string BrainCheatSheet::ClassLine(uint8 classId)
{
    char const* const name = CoaClassName(classId);
    if (classId < 12 || classId > 32 || !name)
        return "";
    std::string out = std::string(name) + ": ";
    bool first = true;
    for (CoaSpecStrategy const& spec : CoaSpecStrategies)
    {
        if (spec.classId != classId)
            continue;
        if (!first)
            out += ", ";
        first = false;
        char const* const role = spec.role == CoaSpecRole::Tank ? "tank" :
            spec.role == CoaSpecRole::Heal ? "healer" : "dps";
        out += std::string(spec.specName) + " (" + role + ", " +
            (std::string_view(spec.position) == "close" ? "melee" : "ranged");
        if (spec.support)
            out += ", support";
        out += ")";
    }
    return first ? "" : out;
}

std::string const& BrainCheatSheet::Text()
{
    static std::string const text = []
    {
        std::string out;
        for (uint8 classId = 12; classId <= 32; ++classId)
        {
            std::string line = ClassLine(classId);
            if (line.empty())
                continue;
            if (!out.empty())
                out += '\n';
            out += line;
        }
        return out;
    }();
    return text;
}
