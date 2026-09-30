/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAVOICEEVENT_H
#define PLAYERBOTS_COAVOICEEVENT_H

#include "CoaChatIntent.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <string>

// Where a message was said, and so who may answer and how the answer is sent.
enum class CoaVoiceScopeKind : uint8
{
    Whisper = 0,  // to one bot; answered by whisper
    Party,        // PARTY, PARTY_LEADER; answered with SayToParty (real players only)
    Raid,         // RAID, RAID_LEADER (not RAID_WARNING); answered with SayToRaid, or SayToParty out of a raid
    Guild,        // answered with SayToGuild
    Say,          // answered with Say
    Yell,         // answered with Yell
    CustomEmote,  // "/e ..." naming a bot; answered with Say
    TextEmote,    // "/wave" aimed at a bot; answered with Say
    Channel,      // a CoaVoice.Channels channel; answered in that same channel, only by a bot on it
    Count
};

// M6: what a banter opener is about. The text rows are coa_banter_<name> (CoaBanterTextName); the text lane is told
// the topic in words.
enum class CoaBanterTopic : uint8
{
    Zone = 0,     // a remark about the zone
    Weather,      // weather talk in general terms (the core exposes no weather state to read)
    Level,        // a joke about levels
    Class,        // a joke about classes
    Barrens,      // classic Barrens chat memes
    Quest,        // quests
    Dungeon,      // dungeons (never a group post)
    Count
};

// Why an event, a candidate or a line came to nothing. Counted by the director, shown by .playerbots voice stats.
enum class CoaVoiceDrop : uint8
{
    Disabled = 0,     // voice was turned off with events still queued
    PrefsNotLoaded,   // the preferences were not loaded yet
    SenderGone,       // the speaker logged out or left the scope before the tick
    OptOut,           // the speaker's account opted out of voice
    NotConversation,  // command, link or market post (hooks filter these; counted if one slips through)
    Addressed,        // the line speaks to someone else by name
    NoCandidate,      // no bot may answer
    Chance,           // the heuristic roll said no
    Chain,            // bot-to-bot chain deeper than BotChainMax
    NoAudience,       // bot-to-bot with no real player heard in the scope lately
    GlobalRate,       // GlobalPerMinute reached
    BotCooldown,      // the bot answered too recently
    ScopeCooldown,    // the scope got an answer too recently
    ScopeRate,        // ScopePerMinute reached
    SenderCooldown,   // this speaker got a public answer too recently
    NoLine,           // no text row for the intent in any language
    Repetition,       // every candidate line was a repeat
    PendingFull,      // too many lines waiting
    BotGone,          // at send time: the bot logged out, teleports, or left the scope
    TargetGone,       // at send time: the whisper or emote target is gone
    Combat,           // at send time: the bot fights, and the line was public
    Filtered,         // at send time: the realm's chat filter matched the line
    SendFailed,       // at send time: the route refused (no channel, no guild right...)
    BrainSilent,      // the brain (calibrated) decided nobody answers
    Invalid,          // M6, at send time: CoaVoiceSanitizer::Validate refused a generated or banter line
    Count
};

// The scope of one event, resolved on the world thread from the CoaEvent and the speaker.
struct CoaVoiceScope
{
    CoaVoiceScopeKind kind = CoaVoiceScopeKind::Say;
    uint8 chatType = 0;    // ChatMsg of the message
    uint8 teamId = 0;      // speaker's TeamId (channels are per team)
    uint64 key = 0;        // CoaHash over kind and the ids below: governor and repetition state key
    ObjectGuid groupGuid;  // Party, Raid
    uint32 guildId = 0;    // Guild
    uint32 mapId = 0;      // speaker's map, instance and zone (Say, Yell, emotes, Channel)
    uint32 instanceId = 0;
    uint32 zoneId = 0;
    std::string channelName;  // Channel

    bool IsPublic() const
    {
        return kind == CoaVoiceScopeKind::Say || kind == CoaVoiceScopeKind::Yell ||
               kind == CoaVoiceScopeKind::CustomEmote || kind == CoaVoiceScopeKind::TextEmote ||
               kind == CoaVoiceScopeKind::Channel || kind == CoaVoiceScopeKind::Guild;
    }
};

// A bot that may answer one event.
struct CoaVoiceCandidate
{
    ObjectGuid guid;
    int32 score = 0;
    bool mentioned = false;  // named in the text, the whisper's receiver or the emote's target (score, chance)
    bool named = false;      // its name is in the text (picks coa_reply_name for an unclear line)
};

// A line waiting for its time, in the director's queue. GUIDs only: everything is looked up again at send time.
struct CoaVoiceLine
{
    uint64 dueMs = 0;     // CoaTime::NowMs() at which to send it
    uint64 eventId = 0;   // the event it answers (logs)
    CoaVoiceScope scope;  // where to send it
    ObjectGuid bot;       // who says it
    ObjectGuid target;    // whisper receiver, emote target, or the speaker it answers
    std::string text;     // filled, persona-styled and sanitized
    uint32 emote = 0;     // TextEmotes id played just before the line, 0 for none
    uint8 depth = 0;      // bot-to-bot hop count of this line (1 for an answer to a real player)
    CoaIntent intent = CoaIntent::None;
    bool isDeny = false;  // a whisper refusal sent because the voice stayed silent

    // M6. A text line: `text` is the best-ranked template line, sent when the generated one does not come.
    uint64 textJobId = 0;       // the text job whose line replaces `text` at send time; 0: none
    uint64 textDeadlineMs = 0;  // past it `text` goes as it is and the job is cancelled
    uint32 textEpoch = 0;       // the director's brainEpoch at submit: another epoch's result is stale
    bool generated = false;     // set by ResolveLineText when the generated line replaced `text` (Send counts it)
    // A banter opener in a channel (CoaVoiceBanter): the chance in percent that a second bot answers it through the
    // BOT event path (the echo event carries it as CoaEvent::answerPercent); 0 for every other line.
    uint8 answerPercent = 0;
    bool banter = false;
};

char const* CoaVoiceScopeName(CoaVoiceScopeKind kind);  // "whisper", "party"...
char const* CoaVoiceDropName(CoaVoiceDrop drop);        // "chance", "bot-cooldown"...
// M6, defined in CoaVoiceBanter.cpp: "zone", "weather", "level", "class", "barrens", "quest", "dungeon"; and the
// text row names "coa_banter_zone", "coa_banter_weather", ... (never null).
char const* CoaBanterTopicName(CoaBanterTopic topic);
char const* CoaBanterTextName(CoaBanterTopic topic);

// While the director makes a bot speak on the world thread, this holds that line's depth; 0 on every other
// thread and at every other time. The say/yell hook copies it into the BOT event of that speech, so an answer
// to a director line is one hop deeper. Defined in CoaVoiceDirector.cpp.
namespace CoaVoiceEmit
{
uint8 CurrentDepth();

class ScopedDepth
{
public:
    explicit ScopedDepth(uint8 depth);
    ~ScopedDepth();
    ScopedDepth(ScopedDepth const&) = delete;
    ScopedDepth& operator=(ScopedDepth const&) = delete;

private:
    uint8 previous;
};
}  // namespace CoaVoiceEmit

#endif
