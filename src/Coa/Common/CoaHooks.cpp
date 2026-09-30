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
// PlayerbotAI(Player*)                      world (OnBotLogin; self-bot  Memory Enable -> bot session (never a
//   -> CoaHooks::OnBotAiCreated             command)                     self-bot) -> Post BotLogin
// ~PlayerbotAI                              map or world                 Memory Enable -> guid and epoch set ->
//   -> CoaHooks::OnBotAiDestroyed                                        Post BotLogout
// CoaPlayerScript::OnPlayerDuelEnd          map (inferred); world for    Memory Enable -> both players -> not
//                                           a cancel opcode              interrupted -> bot pair rule -> Post Duel
// CoaUnitScript::OnUnitDeath                MAP, every unit death        Memory Enable -> player victim: killer's
//                                                                        owner, not self -> bot pair rule ->
//                                                                        BattlegroundKills -> Post PlayerKill;
//                                                                        creature: leader or boss -> killer's
//                                                                        owner -> Post BossKill
// CoaPlayerScript::OnPlayerCreatureKill     MAP, every creature a        Memory Enable -> loot recipient set, not
//                                           player kills                 the killer, not the killer's group ->
//                                                                        Post CreatureHelp
// CoaGroupScript::OnAddMember               map (a bot accepting         Memory Enable -> not a battleground or
//                                           inline) or world             battlefield group -> Post GroupJoin
// CoaGroupScript::OnRemoveMember            map or world                 same -> a kick with a kicker -> Post
//                                                                        GroupLeave
// CoaPlayerScript::OnPlayerDeleteFromDB     world (inferred)             Memory Enable -> Post CharacterDeleted
// TradeStatusAction                         the bot's map                Memory Enable -> bot session -> trader
//   -> CoaHooks::OnTradeCompleted                                        real, or TrackBotToBot -> Post Trade
// CoaWorldScript OnAfterConfigLoad,         world (main thread)          CoaThread::CaptureWorldThread() first
//   OnStartup, OnUpdate, OnShutdown
// director, governor, index, templates,     world only                   COA_ASSERT_WORLD_THREAD() at public
//   prefs writes, CoaMemoryMgr                                           entry points
// CoaChatIntent, NameMatch, sanitizer,      any                          pure, atomic, or a short lock
//   persona For, EmoteCategoryFor,
//   CoaVoiceConfig::Enabled/Current,
//   CoaMemoryConfig::Enabled/TrackBotToBot/
//   BattlegroundKills/IsLeaderEntry,
//   CoaMemoryMgr::Running,
//   CoaPlayerPrefs::Get/VoiceOptedOut/
//   IsLoaded, CoaEventBus::Post
//
// Human speakers never race map threads (chat is handled in UpdateSessions while the map threads are idle). Bot
// speech and text emotes run on map threads: those paths only read the speaker's own Player and post to the bus.
//
// Every hook tests CoaVoiceConfig::Enabled() (a relaxed atomic) before the bot check: with voice off the hooks cost
// one load, not even the bot map lookup, and BotToBot implies Enable, so no bot path depends on the order.
//
// The memory hooks test CoaMemoryConfig::Enabled() first instead: with memory off each costs that one load. Then
// CoaMemoryMgr::Running() (an atomic), so a memory that stayed off (tables missing) posts nothing. Then they read
// only the objects they were handed (on the same map), tell bots from players by WorldSession::IsBot (immutable;
// never GET_PLAYERBOT_AI, never an ObjectAccessor lookup by GUID) and post plain values. Whether the other side is
// a player worth remembering is settled here only when both sessions are at hand; CoaMemoryMgr checks the rest on
// the world thread.

#include "CoaHooks.h"
#include "Channel.h"
#include "CoaChatIntent.h"
#include "CoaEventBus.h"
#include "CoaMemoryConfig.h"
#include "CoaMemoryMgr.h"
#include "CoaPlayerPrefs.h"
#include "CoaVoiceConfig.h"
#include "CoaVoiceEvent.h"
#include "CoaVoiceTemplates.h"
#include "Creature.h"
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

// Bot sessions (random, addclass and alt bots) are created with isBot = true; real sessions, self-bots included, are
// players. The flag never changes: safe on any thread.
bool IsBotSession(Player const* player)
{
    WorldSession const* session = player->GetSession();
    return session && session->IsBot();
}

// Every memory hook's first test: Enable (with memory off, that one relaxed load), then the manager runs (its tables
// were found), so a memory that stayed off posts nothing.
bool MemoryRunning()
{
    return CoaMemoryConfig::Enabled() && CoaMemoryMgr::Running();
}

// A deed between two characters is worth an event when one is a bot and the other a real player, or both are bots
// and CoaMemory.TrackBotToBot is on.
bool MemoryPair(bool firstBot, bool secondBot)
{
    if (!firstBot && !secondBot)
        return false;
    return !(firstBot && secondBot) || CoaMemoryConfig::TrackBotToBot();
}

uint8 SessionFlags(bool senderBot, bool targetBot)
{
    return uint8((senderBot ? COA_EVENT_SENDER_BOT : 0) | (targetBot ? COA_EVENT_TARGET_BOT : 0));
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

void CoaHooks::OnBotAiCreated(Player* bot, uint64 epoch)
{
    if (!MemoryRunning() || !bot || !epoch)
        return;
    // A self-bot keeps its real session: a player's own character never gets a memory.
    if (!IsBotSession(bot))
        return;

    CoaEvent event;
    event.kind = CoaEventKind::BotLogin;
    event.sender = bot->GetGUID();
    event.epoch = epoch;
    CoaEventBus::Instance().Post(std::move(event));
}

void CoaHooks::OnBotAiDestroyed(ObjectGuid bot, uint64 epoch)
{
    // Map or world thread (first statement of ~PlayerbotAI): only the two values are read. The manager ignores a
    // logout it has no entry for (a self-bot, or a bot that logged in while memory was off).
    if (!MemoryRunning() || bot.IsEmpty() || !epoch)
        return;

    CoaEvent event;
    event.kind = CoaEventKind::BotLogout;
    event.sender = bot;
    event.epoch = epoch;
    CoaEventBus::Instance().Post(std::move(event));
}

void CoaHooks::OnTradeCompleted(Player* bot, Player* trader, uint32 itemsReceived, uint32 itemsGiven)
{
    if (!MemoryRunning() || !bot || !trader)
        return;
    // The bot's map thread; the trader stands next to it. Only the side whose accept completed the trade gets here
    // (a bot accepts after the trader), so a trade between two bots is remembered by one of them.
    bool const traderBot = IsBotSession(trader);
    if (!IsBotSession(bot) || (traderBot && !CoaMemoryConfig::TrackBotToBot()))
        return;

    CoaEvent event;
    event.kind = CoaEventKind::Trade;
    event.sender = bot->GetGUID();
    event.target = trader->GetGUID();
    event.value = itemsReceived;
    event.value2 = itemsGiven;
    event.mapId = bot->GetMapId();
    event.zoneId = bot->GetZoneId();
    event.flags = SessionFlags(true, traderBot);
    CoaEventBus::Instance().Post(std::move(event));
}

namespace
{
class CoaPlayerScript : public PlayerScript
{
public:
    CoaPlayerScript()
        : PlayerScript("CoaPlayerScript", {PLAYERHOOK_CAN_PLAYER_USE_CHAT, PLAYERHOOK_ON_TEXT_EMOTE,
                                           PLAYERHOOK_ON_DUEL_END, PLAYERHOOK_ON_CREATURE_KILL,
                                           PLAYERHOOK_ON_DELETE_FROM_DB})
    {
    }

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

    // Player::DuelComplete: the winner's and loser's map thread (a duel keeps both on one map); the world thread for
    // a cancel opcode, which is DUEL_INTERRUPTED. On DUEL_FLED the loser is the one who fled.
    void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override
    {
        if (!MemoryRunning() || !winner || !loser)
            return;
        if (type == DUEL_INTERRUPTED)
            return;
        bool const winnerBot = IsBotSession(winner);
        bool const loserBot = IsBotSession(loser);
        if (!MemoryPair(winnerBot, loserBot))
            return;

        CoaEvent event;
        event.kind = CoaEventKind::Duel;
        event.sender = winner->GetGUID();
        event.target = loser->GetGUID();
        event.value = uint32(type);
        event.senderLevel = winner->GetLevel();
        event.targetLevel = loser->GetLevel();
        event.mapId = winner->GetMapId();
        event.zoneId = winner->GetZoneId();
        event.flags = SessionFlags(winnerBot, loserBot);
        CoaEventBus::Instance().Post(std::move(event));
    }

    // Unit::Kill, map thread, for every creature a player lands the killing blow on (pets excluded: the core passes
    // killer->ToPlayer()). Help given across groups: the creature was tagged by someone else, outside the killer's
    // group. Unit::Kill keeps the loot recipient unless the kill gives no reward (then it is cleared: no event).
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        if (!MemoryRunning() || !killer || !killed)
            return;
        ObjectGuid const recipient = killed->GetLootRecipientGUID();
        if (!recipient.IsPlayer() || recipient == killer->GetGUID())
            return;
        ObjectGuid::LowType const recipientGroup = killed->GetLootRecipientGroupGUID();
        Group const* group = killer->GetGroup();
        if (group && recipientGroup && group->GetGUID().GetCounter() == recipientGroup)
            return;

        CoaEvent event;
        event.kind = CoaEventKind::CreatureHelp;
        event.sender = killer->GetGUID();
        event.target = recipient;
        event.groupGuid = recipientGroup ? ObjectGuid::Create<HighGuid::Group>(recipientGroup) : ObjectGuid::Empty;
        event.entry = killed->GetEntry();
        event.mapId = killed->GetMapId();
        event.zoneId = killed->GetZoneId();
        event.flags = SessionFlags(IsBotSession(killer), false);
        CoaEventBus::Instance().Post(std::move(event));
    }

    // Player::DeleteFromDB, hard deletes only (world thread, inferred; the bot wipe deletes with raw SQL and never
    // gets here). The manager purges both tables and every memory in RAM.
    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction /*trans*/, uint32 guid) override
    {
        if (!MemoryRunning() || !guid)
            return;

        CoaEvent event;
        event.kind = CoaEventKind::CharacterDeleted;
        event.sender = ObjectGuid::Create<HighGuid::Player>(guid);
        CoaEventBus::Instance().Post(std::move(event));
    }
};

// Unit::Kill calls OnUnitDeath on the victim's map thread for every unit that dies: the memory flag comes first, and
// a creature needs a leader entry or a boss flag before anything else is read.
class CoaUnitScript : public UnitScript
{
public:
    CoaUnitScript() : UnitScript("CoaUnitScript", true, {UNITHOOK_ON_UNIT_DEATH}) {}

    void OnUnitDeath(Unit* victim, Unit* killer) override
    {
        if (!MemoryRunning() || !victim)
            return;

        if (Player* dead = victim->ToPlayer())
        {
            // Pets, totems and charmed units count for their player (OnPlayerPVPKill would miss them).
            Player* by = killer ? killer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
            if (!by || by == dead)
                return;
            bool const byBot = IsBotSession(by);
            bool const deadBot = IsBotSession(dead);
            if (!MemoryPair(byBot, deadBot))
                return;
            bool const battleground = dead->InBattleground() || dead->InArena();
            if (battleground && !CoaMemoryConfig::BattlegroundKills())
                return;

            CoaEvent event;
            event.kind = CoaEventKind::PlayerKill;
            event.sender = by->GetGUID();
            event.target = dead->GetGUID();
            event.senderLevel = by->GetLevel();
            event.targetLevel = dead->GetLevel();
            event.mapId = dead->GetMapId();
            event.zoneId = dead->GetZoneId();
            event.flags = uint8(SessionFlags(byBot, deadBot) | (battleground ? COA_EVENT_BATTLEGROUND : 0));
            CoaEventBus::Instance().Post(std::move(event));
            return;
        }

        Creature* creature = victim->ToCreature();
        if (!creature)
            return;
        uint32 const entry = creature->GetEntry();
        bool const leader = CoaMemoryConfig::IsLeaderEntry(entry);
        if (!leader && !creature->isWorldBoss() && !creature->IsDungeonBoss())
            return;
        Player* by = killer ? killer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (!by)
            return;

        // The participants are looked up on the world thread from the group and the map instance.
        Group const* group = by->GetGroup();
        CoaEvent event;
        event.kind = CoaEventKind::BossKill;
        event.sender = by->GetGUID();
        event.entry = entry;
        event.groupGuid = group ? group->GetGUID() : ObjectGuid::Empty;
        event.mapId = creature->GetMapId();
        event.instanceId = creature->GetInstanceId();
        event.zoneId = creature->GetZoneId();
        event.flags = uint8(SessionFlags(IsBotSession(by), false) | (leader ? COA_EVENT_LEADER : 0));
        CoaEventBus::Instance().Post(std::move(event));
    }
};

// Group::AddMember and Group::RemoveMember: the thread of whoever changed the group (a bot accepting an invite on
// its map thread, a player's opcode or a queued operation on the world thread). Only the group's own flags and GUID
// are read; the members are looked up again on the world thread.
class CoaGroupScript : public GroupScript
{
public:
    CoaGroupScript() : GroupScript("CoaGroupScript", {GROUPHOOK_ON_ADD_MEMBER, GROUPHOOK_ON_REMOVE_MEMBER}) {}

    void OnAddMember(Group* group, ObjectGuid guid) override
    {
        if (!MemoryRunning() || !group || guid.IsEmpty())
            return;
        if (group->isBGGroup() || group->isBFGroup())
            return;

        CoaEvent event;
        event.kind = CoaEventKind::GroupJoin;
        event.sender = guid;
        event.groupGuid = group->GetGUID();
        CoaEventBus::Instance().Post(std::move(event));
    }

    // Only kicks are remembered, and only with a kicker: an LFG vote kick (KICK_LFG) names none, so it never is.
    void OnRemoveMember(Group* group, ObjectGuid guid, RemoveMethod method, ObjectGuid kicker,
                        char const* /*reason*/) override
    {
        if (!MemoryRunning() || !group || guid.IsEmpty())
            return;
        if (group->isBGGroup() || group->isBFGroup())
            return;
        if (method != GROUP_REMOVEMETHOD_KICK && method != GROUP_REMOVEMETHOD_KICK_LFG)
            return;
        if (!kicker.IsPlayer() || kicker == guid)
            return;

        CoaEvent event;
        event.kind = CoaEventKind::GroupLeave;
        event.sender = guid;
        event.target = kicker;
        event.groupGuid = group->GetGUID();
        event.value = uint32(method);
        CoaEventBus::Instance().Post(std::move(event));
    }
};
}  // namespace

void AddSC_coa_hooks()
{
    new CoaPlayerScript();
    new CoaUnitScript();
    new CoaGroupScript();
}
