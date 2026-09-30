/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYRECRUITMENT_H
#define PLAYERBOTS_ACTIVITYRECRUITMENT_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <string_view>
#include <vector>

class Player;
struct ActivityConfig;

// How a real player asked to join (M5).
enum class ActivityJoinSource : uint8
{
    Keyword = 0,  // a whole-message join word (Chat.JoinWords: "inv", "x", "moi") whispered to a recruiting leader
    Prefix,       // "!join [id]" / "!rejoindre [id]" (WhisperPrefix) whispered to any bot
    LfgRequest,   // "lfg bot ..." said in a CoaLfgChannels channel: recruiting leaders whisper an offer
};

// A join request heard in chat, handed to the director. Plain values only.
struct ActivityJoinRequest
{
    ObjectGuid player;        // the real player
    ObjectGuid bot;           // the bot whispered to (Keyword, Prefix): it whispers the answer back; empty for LFG
    uint32 activityId = 0;    // Keyword: the leader's activity; Prefix: the id typed, else the bot's activity, else 0
                              // (the nearest one); LfgRequest: 0
    ActivityJoinSource source = ActivityJoinSource::Keyword;
    uint8 roleMask = 0;       // LfgRequest: the CoaRole bits asked (1 << CoaRole), informative
    uint64 atMs = 0;          // CoaTime::NowMs() when heard
};

// Chat recruitment (M5): the whisper seam and the "lfg bot" seam turn a real player's words into join requests; the
// director drains them on its tick and answers through the kinds (ActivityKind::Join) with the receiving bot's
// whisper. The hooks run on the world thread for real players (CMSG_MESSAGECHAT is PROCESS_THREADUNSAFE); the queue
// still has its own mutex, so a hook on any thread is safe. With AiPlayerbot.CoaActivity.Enable = 0 every entry
// returns after one relaxed load (ActivityDirector::Running()).
namespace ActivityRecruitment
{
// The whisper hook (Playerbots.cpp), once the receiver is known to have a PlayerbotAI, before voice and
// HandleCommand. True: the whisper is the director's (a join word to a recruiting leader of this life of the bot, or
// the prefix form to any bot while activities run): the caller returns true at once, so the whisper is delivered
// (the player sees it) and nothing else answers it. False for bot senders, and whenever nothing matched: the
// legacy path runs untouched. Requests beyond one per player per JOIN_REQUEST_GAP_MS, or a full queue, are consumed
// and dropped.
bool OnWhisper(Player* sender, Player* receiver, std::string const& text);

// CoaLfgHeard (CoaSpecialization.cpp), once a real player's "lfg bot ..." request was recognized, before the bots'
// offers. Queues a LfgRequest; never changes what CoaLfgHeard does.
void OnLfgRequest(Player* player, uint8 roleMask);

// The director, every tick: every waiting request, oldest first (the queue is emptied).
void Drain(std::vector<ActivityJoinRequest>& out);

// The director's StopRunning and Shutdown: drops what waits and the per-player gaps.
void Clear();

// Pure helpers (any thread; tests).
// The text trimmed of ASCII blanks and of trailing '.', '!', '?', ASCII-lowered.
std::string Normalize(std::string_view text);
// Normalize(text) is one of config.chatJoinWords.
bool IsJoinWord(std::string_view text, ActivityConfig const& config);
// Normalize(text) is config.whisperPrefix followed by "join" or "rejoindre", then nothing or blanks and a number;
// `id` gets the number (0 when absent). A prefix that is empty never matches.
bool IsPrefixJoin(std::string_view text, ActivityConfig const& config, uint32& id);
}  // namespace ActivityRecruitment

#endif
