/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaPartyStrategy.h"

void CoaPartyStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("coa party greet", {NextAction("coa party greet", 7.0f)}));
    triggers.push_back(new TriggerNode("coa party mount", {NextAction("coa party mount", 6.0f)}));
    triggers.push_back(new TriggerNode("coa party emote", {NextAction("coa party emote", 5.0f)}));
}
