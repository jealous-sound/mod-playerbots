/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Hook and operation threads:
//
// Hook / step                               Thread                       First statements
// ----------------------------------------  ---------------------------  -----------------------------------------
// whisper hook (PlayerbotsPlayerScript)     world (real sender);         bot check -> Enable -> IsConversation ->
//   -> CoaHooks::OnWhisper                  map (bot sender)             prefs loaded, not opted out -> realm
//                                                                        chat filter -> Post; returns whether posted
// group hook -> CoaHooks::OnGroupChat       world; map for bot speech    same, no chat filter (the core has none
//                                           (inferred)                   there); no BOT events, no RAID_WARNING
// guild hook -> CoaHooks::OnGuildChat       world; map for bot speech    same; no BOT events
//                                           (inferred)
// channel hook -> CoaHooks::OnChannelChat   world; map for bot speech    same, plus !lfgHandled and
//                                           (inferred)                   ListensTo(channel)
// CoaPlayerScript::OnPlayerCanUseChat       world (real speaker);        bot check: BOT event for SAY/YELL only
//   (4 args: SAY, YELL, EMOTE; AFK/DND      MAP (bot)                    when BotToBotEnabled(); then Enable ...
//   ignored)                                                             realm chat filter -> Post. Always true.
// CoaPlayerScript::OnPlayerTextEmote        MAP (everyone)               bot check -> Enable -> guid is a player,
//                                                                        not self -> EmoteCategoryFor != None ->
//                                                                        prefs loaded -> Post
// CoaWorldScript OnAfterConfigLoad,         world (main thread)          CoaThread::CaptureWorldThread() first
//   OnStartup, OnUpdate, OnShutdown
// director, governor, index, templates,     world only                   COA_ASSERT_WORLD_THREAD() at public
//   prefs writes                                                         entry points
// CoaChatIntent, NameMatch, sanitizer,      any                          pure, atomic, or a short lock
//   persona For, EmoteCategoryFor,
//   CoaVoiceConfig::Enabled/Current,
//   CoaPlayerPrefs::Get/VoiceOptedOut/
//   IsLoaded, CoaEventBus::Post
//
// Human speakers never race map threads (chat is handled in UpdateSessions while the map threads are idle). Bot
// speech and text emotes run on map threads: those paths only read the speaker's own Player and post to the bus.
//
// Every hook tests CoaVoiceConfig::Enabled() (a relaxed atomic) before the bot check: with voice off the hooks cost
// one load, not even the bot map lookup, and BotToBot implies Enable, so no bot path depends on the order.

#include "CoaHooks.h"
#include "Channel.h"
#include "CoaChatIntent.h"
#include "CoaEventBus.h"
#include "CoaPlayerPrefs.h"
#include "CoaVoiceConfig.h"
#include "CoaVoiceEvent.h"
#include "CoaVoiceTemplates.h"
#include "Group.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "World.h"
#include "WorldSession.h"
#include <utility>

namespace
{
// Enable, a session, preferences loaded, conversation (no command, link or market post), not opted out.
bool VoiceHears(Player* speaker, std::string const& text)
{
    if (!CoaVoiceConfig::Enabled())
        return false;
    WorldSession* session = speaker->GetSession();
    if (!session)
        return false;
    // Addon messages (LANG_ADDON, "prefix\tpayload") reach the whisper, group and guild hooks too. The hooks are not
    // given the language, and the chat box cannot type a tab.
    if (text.find('\t') != std::string::npos)
        return false;
    CoaPlayerPrefs& prefs = CoaPlayerPrefs::Instance();
    return prefs.IsLoaded() && CoaChatIntent::IsConversation(text) && !prefs.VoiceOptedOut(session->GetAccountId());
}

// Plain values only: the director looks every GUID up again on the world thread.
CoaEvent ChatEvent(Player* speaker, uint32 type, std::string const& text)
{
    CoaEvent event;
    event.kind = CoaEventKind::Chat;
    event.chatType = uint8(type);
    event.teamId = uint8(speaker->GetTeamId());
    event.sender = speaker->GetGUID();
    event.text = text;
    return event;
}

// The realm's chat filter, tested as the core tests it right after these hooks (Player::Say, Yell, TextEmote,
// Whisper): a line it blocks reaches no one, so no bot answers it. The core filters these four types only.
// World thread (real speakers).
bool CoreFiltered(uint32 type, std::string const& text)
{
    ServerConfigs config;
    switch (type)
    {
        case CHAT_MSG_WHISPER:
            config = CONFIG_CHAT_FILTER_WHISPER;
            break;
        case CHAT_MSG_SAY:
            config = CONFIG_CHAT_FILTER_SAY;
            break;
        case CHAT_MSG_YELL:
            config = CONFIG_CHAT_FILTER_YELL;
            break;
        case CHAT_MSG_EMOTE:
            config = CONFIG_CHAT_FILTER_EMOTE;
            break;
        default:
            return false;
    }
    return sWorld->getBoolConfig(config) && sObjectMgr->IsChatFiltered(text);
}

// Not RAID_WARNING: raid warnings are callouts, and one naming a bot is HandleCommand's "warning" command.
bool IsGroupChat(uint32 type)
{
    switch (type)
    {
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_LEADER:
            return true;
        default:
            return false;
    }
}
}  // namespace

bool CoaHooks::OnWhisper(Player* sender, Player* receiver, std::string const& text)
{
    if (!CoaVoiceConfig::Enabled() || !sender || !receiver)
        return false;
    // Bot whispers (map thread) stay legacy: bot-to-bot covers say, yell and the director's own lines only.
    if (GET_PLAYERBOT_AI(sender))
        return false;
    // A filtered whisper is never delivered: false leaves HandleCommand exactly as with voice off.
    if (!VoiceHears(sender, text) || CoreFiltered(CHAT_MSG_WHISPER, text))
        return false;

    CoaEvent event = ChatEvent(sender, CHAT_MSG_WHISPER, text);
    event.target = receiver->GetGUID();
    return CoaEventBus::Instance().Post(std::move(event));
}

void CoaHooks::OnGroupChat(Player* sender, uint32 type, std::string const& text, Group* group)
{
    if (!CoaVoiceConfig::Enabled() || !sender || !group)
        return;
    if (GET_PLAYERBOT_AI(sender))
        return;
    // Battleground chat and raid warnings reach this hook too; bots answer neither.
    if (!IsGroupChat(type) || !VoiceHears(sender, text))
        return;

    CoaEvent event = ChatEvent(sender, type, text);
    event.groupGuid = group->GetGUID();
    CoaEventBus::Instance().Post(std::move(event));
}

void CoaHooks::OnGuildChat(Player* sender, std::string const& text)
{
    if (!CoaVoiceConfig::Enabled() || !sender)
        return;
    if (GET_PLAYERBOT_AI(sender))
        return;
    uint32 const guildId = sender->GetGuildId();
    if (!guildId || !VoiceHears(sender, text))
        return;

    CoaEvent event = ChatEvent(sender, CHAT_MSG_GUILD, text);
    event.guildId = guildId;
    CoaEventBus::Instance().Post(std::move(event));
}

bool CoaHooks::OnChannelChat(Player* sender, std::string const& text, Channel* channel, bool lfgHandled)
{
    if (!CoaVoiceConfig::Enabled() || !sender || !channel)
        return false;
    if (GET_PLAYERBOT_AI(sender))
        return false;
    // An LFG request CoaLfgHeard answered belongs to CoaLfg alone.
    if (lfgHandled)
        return false;
    if (!CoaVoiceConfig::Current()->ListensTo(channel->GetName()) || !VoiceHears(sender, text))
        return false;

    CoaEvent event = ChatEvent(sender, CHAT_MSG_CHANNEL, text);
    event.channelName = channel->GetName();
    return CoaEventBus::Instance().Post(std::move(event));
}

namespace
{
class CoaPlayerScript : public PlayerScript
{
public:
    CoaPlayerScript() : PlayerScript("CoaPlayerScript", {PLAYERHOOK_CAN_PLAYER_USE_CHAT, PLAYERHOOK_ON_TEXT_EMOTE}) {}

    using PlayerScript::OnPlayerCanUseChat;  // keep the base overloads visible

    // Say, yell and "/e" (and AFK/DND, ignored). World thread for a real speaker; the bot's map thread when a bot
    // speaks through Player::Say/Yell/TextEmote. Never refuses the message.
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg) override
    {
        if (!CoaVoiceConfig::Enabled() || !player)
            return true;

        if (GET_PLAYERBOT_AI(player))
        {
            // Map thread: only the bot's own Player is read.
            if (CoaVoiceConfig::BotToBotEnabled() && (type == CHAT_MSG_SAY || type == CHAT_MSG_YELL) &&
                CoaChatIntent::IsConversation(msg))
            {
                CoaEvent event = ChatEvent(player, type, msg);
                event.fromBot = true;
                // A director line carries its hop; any other bot speech is a first hop.
                uint8 const depth = CoaVoiceEmit::CurrentDepth();
                event.chainDepth = depth ? depth : 1;
                CoaEventBus::Instance().Post(std::move(event));
            }
            return true;
        }

        if (type != CHAT_MSG_SAY && type != CHAT_MSG_YELL && type != CHAT_MSG_EMOTE)
            return true;
        if (VoiceHears(player, msg) && !CoreFiltered(type, msg))
            CoaEventBus::Instance().Post(ChatEvent(player, type, msg));
        return true;
    }

    // CMSG_TEXT_EMOTE is PROCESS_THREADSAFE: the emoter's map thread, for everyone. The target is not looked up
    // here; the director checks it, and the emoter's preferences, on the world thread.
    void OnPlayerTextEmote(Player* player, uint32 textEmote, uint32 /*emoteNum*/, ObjectGuid guid) override
    {
        if (!CoaVoiceConfig::Enabled() || !player)
            return;
        if (GET_PLAYERBOT_AI(player))
            return;
        if (!guid.IsPlayer() || guid == player->GetGUID())
            return;
        if (CoaVoiceTemplates::EmoteCategoryFor(textEmote) == CoaEmoteCategory::None)
            return;
        if (!CoaPlayerPrefs::Instance().IsLoaded())
            return;

        CoaEvent event;
        event.kind = CoaEventKind::TextEmote;
        event.textEmote = textEmote;
        event.teamId = uint8(player->GetTeamId());
        event.sender = player->GetGUID();
        event.target = guid;
        CoaEventBus::Instance().Post(std::move(event));
    }
};
}  // namespace

void AddSC_coa_hooks() { new CoaPlayerScript(); }
