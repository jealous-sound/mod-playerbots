/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaMemoryStrategy.h"

void CoaMemoryStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Both always: each trigger tests its own switch, so a .reload config applies without a strategy reset.
    triggers.push_back(new TriggerNode("coa known player near", {NextAction("coa greet known", 11.0f)}));
    triggers.push_back(new TriggerNode("coa friend near", {NextAction("coa offer group", 10.5f)}));
}
