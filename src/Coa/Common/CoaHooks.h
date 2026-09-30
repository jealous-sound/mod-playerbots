/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAHOOKS_H
#define PLAYERBOTS_COAHOOKS_H

#include "Define.h"
#include <string>

class Channel;
class Group;
class Player;

// Hook threads (see the table at the top of CoaHooks.cpp):
// - The chat hooks run on the world thread when a real player speaks (CMSG_MESSAGECHAT is PROCESS_THREADUNSAFE),
//   and on the bot's map thread when a bot speaks (Player::Say/Yell/Whisper called from its AI).
// - OnPlayerTextEmote runs on the emoter's map thread (CMSG_TEXT_EMOTE is PROCESS_THREADSAFE).
// Every entry point below therefore starts with the bot check: a bot speaker returns at once (posting a BOT
// event first only when AiPlayerbot.CoaVoice.BotToBot is on), then the CoaVoice.Enable flag. A hook only
// copies GUIDs and strings into a CoaEvent; the director does the rest on the world thread.
namespace CoaHooks
{
// Whisper hook (Playerbots.cpp), once the receiver is known to be a bot, before PlayerbotAI::HandleCommand.
// True when the director took the whisper as conversation: the caller then passes quietRefusals = true to
// HandleCommand, and the director sends the refusal itself if it stays silent. False for a whisper the realm's
// chat filter blocks (ChatFilter.Whisper), which the core never delivers.
bool OnWhisper(Player* sender, Player* receiver, std::string const& text);

// Group hook, first statement. Opens events for PARTY, PARTY_LEADER, RAID and RAID_LEADER; not RAID_WARNING
// (callouts; one naming a bot is the legacy "warning" command).
void OnGroupChat(Player* sender, uint32 type, std::string const& text, Group* group);

// Guild hook, after the CHAT_MSG_GUILD check and before the playerbotMgr early return.
void OnGuildChat(Player* sender, std::string const& text);

// Channel hook, right after CoaLfgHeard. True when the director took the message (a CoaVoice.Channels
// channel, conversation, not an LFG request CoaLfgHeard handled): the caller then skips the legacy
// sRandomPlayerbotMgr.HandleCommand scan, which could only have dropped the text after a security check per bot.
bool OnChannelChat(Player* sender, std::string const& text, Channel* channel, bool lfgHandled);
}  // namespace CoaHooks

// Script registration, called at the end of AddPlayerbotsScripts (Playerbots.cpp).
void AddSC_coa_hooks();  // CoaPlayerScript: say/yell/emote chat and text emotes (CoaHooks.cpp)
void AddSC_coa_world();  // CoaWorldScript: config, update order, shutdown (CoaWorldScript.cpp)

#endif
