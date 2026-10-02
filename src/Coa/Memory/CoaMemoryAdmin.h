/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAMEMORYADMIN_H
#define PLAYERBOTS_COAMEMORYADMIN_H

#include "Define.h"
#include <string>
#include <vector>

// The .playerbots memory commands. World thread (chat commands arrive through CMSG_MESSAGECHAT, console commands
// through World::ProcessCliCommands). Each returns the lines to send at once; answers that need the database
// arrive ticks later through the printer given to CoaMemoryMgr::Update, to `requester` (an account id, 0 for the
// console). Names are character names as typed (normalized here). When memory is not running every command
// answers one line saying so.
class CoaMemoryAdmin
{
public:
    // GM. `playerName` may be empty. An online bot with a ready memory is answered at once from RAM; otherwise
    // from the database (async).
    static std::vector<std::string> Show(uint32 requester, std::string const& botName, std::string const& playerName);
    // GM. RAM and database.
    static std::vector<std::string> Forget(std::string const& botName, std::string const& playerName);
    static std::vector<std::string> Wipe(std::string const& botName);
    // GM. `target`: "#<account id>", or a character name (rows about it, and its own rows when it is a bot), else
    // an account name.
    static std::vector<std::string> Purge(std::string const& target);
    // GM. FormatStats now, then the database row counts (async).
    static std::vector<std::string> Stats(uint32 requester);

    // Player (SEC_PLAYER, in game). What bots keep about the caller's account, 5 relations per page (page from 1),
    // newest first, with bot names, the ten axes, the counters and the event kinds with dates. Dirty memories
    // about that account are flushed first so the database answer is current (PlayerbotsDatabase.WorkerThreads
    // = 1 keeps the order). Localized rows coa_mem_mine_*.
    static std::vector<std::string> Mine(uint32 requester, uint32 accountId, uint32 page);

    // The localized coa_mem_disclosure line (%events, %relations: the retention days), "" when memory is off. Shown
    // by .playerbots optout with no topic.
    static std::string Disclosure();

private:
    // Through the printer CoaMemoryMgr::Update was given; nothing before the first Update.
    static void Print(uint32 requester, std::string const& line);
};

#endif
