/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MEMORYTYPES_H
#define PLAYERBOTS_MEMORYTYPES_H

#include "CoaChatIntent.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <string>

// Plain values shared by every part of the bots' memory (M3). Nothing here points at a game object.

// The ten feelings a bot keeps about one character, stored as SMALLINT in 1/10 of a point: 1000 = 100.0.
// Affinity, trust and respect run from -1000 to 1000; the others from 0 to 1000. Order = column order.
enum class MemoryAxis : uint8
{
    Affinity = 0,  // like or dislike
    Trust,         // deeds only: trades, groups, bosses; a kick lowers it
    Respect,       // duels, fights and bosses
    Rivalry,       // repeated competition either way
    Gratitude,     // help received, thanks
    Grudge,        // killed by them, insulted, kicked
    Fear,          // killed by a much stronger character
    Annoyance,     // insults, rude emotes, fleeing duels
    Familiarity,   // any contact
    Camaraderie,   // fighting side by side
    Count
};

// Counters, SMALLINT UNSIGNED, saturating at 65535. Order = column order.
enum class MemoryCounter : uint8
{
    Meetings = 0,  // meetings: first contact of the pair after 30 minutes without one
    Chats,         // messages addressed to the bot
    HelpedMe,      // they killed a creature the bot had tagged
    HelpedThem,    // the bot killed a creature they had tagged
    KilledMe,
    KilledThem,
    DuelsWon,      // won by the bot (a duel the other fled counts as won)
    DuelsLost,
    GroupsShared,
    Trades,
    Count
};

constexpr std::size_t MEMORY_AXIS_COUNT = std::size_t(MemoryAxis::Count);
constexpr std::size_t MEMORY_COUNTER_COUNT = std::size_t(MemoryCounter::Count);
constexpr int16 MEMORY_AXIS_MAX = 1000;
constexpr uint16 MEMORY_COUNTER_MAX = 65535;

// Affinity, trust and respect may go negative.
constexpr bool MemoryAxisSigned(MemoryAxis axis)
{
    return axis == MemoryAxis::Affinity || axis == MemoryAxis::Trust || axis == MemoryAxis::Respect;
}

inline int16 MemoryClampAxis(MemoryAxis axis, int32 value)
{
    int32 const low = MemoryAxisSigned(axis) ? -MEMORY_AXIS_MAX : 0;
    return int16(value < low ? low : (value > MEMORY_AXIS_MAX ? MEMORY_AXIS_MAX : value));
}

// The column names: "affinity", "trust", "respect", "rivalry", "gratitude", "grudge", "fear", "annoyance",
// "familiarity", "camaraderie"; "meetings", "chats", "helped_me", "helped_them", "killed_me", "killed_them",
// "duels_won", "duels_lost", "groups_shared", "trades". Also used in logs and GM output.
char const* MemoryAxisName(MemoryAxis axis);
char const* MemoryCounterName(MemoryCounter counter);

// Lazy decay toward 0: v * 0.5^(elapsed / half-life). Counters never decay.
struct MemoryDecay
{
    std::array<float, MEMORY_AXIS_COUNT> perSecond{};  // ln 2 / half-life in seconds; 0 = never decays

    // `value` as of `fromUnix`, decayed to `toUnix`. Truncates toward 0, so small values do reach 0.
    int16 Apply(int16 value, MemoryAxis axis, uint32 fromUnix, uint32 toUnix) const
    {
        float const k = perSecond[std::size_t(axis)];
        if (!value || k <= 0.0f || toUnix <= fromUnix)
            return value;
        return int16(double(value) * std::exp(-double(k) * double(toUnix - fromUnix)));
    }

    // Half-lives in days, in MemoryAxis order; 0 or less never decays.
    static MemoryDecay FromHalfLives(std::array<float, MEMORY_AXIS_COUNT> const& halfLifeDays)
    {
        constexpr double LN2 = 0.6931471805599453;
        MemoryDecay decay;
        for (std::size_t i = 0; i < MEMORY_AXIS_COUNT; ++i)
            decay.perSecond[i] = halfLifeDays[i] > 0.0f ? float(LN2 / (double(halfLifeDays[i]) * 86400.0)) : 0.0f;
        return decay;
    }
};

// What a relation amounts to, derived when needed and never stored. Worst first when several apply:
// Grudge, Rival, Friend, Disliked, Acquaintance, Stranger.
enum class MemoryLabel : uint8
{
    Stranger = 0,
    Acquaintance,
    Friend,
    Rival,
    Disliked,
    Grudge,
    Count
};

// "stranger", "acquaintance", "friend", "rival", "disliked", "grudge".
char const* MemoryLabelName(MemoryLabel label);

// Thresholds on decayed values (1/10 points). Behaviours, voice and retrieval all use these.
namespace MemoryThreshold
{
constexpr int16 FRIEND_AFFINITY = 400;       // and trust >= FRIEND_TRUST or familiarity >= FRIEND_FAMILIARITY
constexpr int16 FRIEND_TRUST = 150;
constexpr int16 FRIEND_FAMILIARITY = 300;
constexpr int16 GRUDGE = 400;                // grudge alone
// Grudge with a kill (outside battlegrounds) in the last 7 days. Below one same-level kill's +250 decayed over the
// whole window (176 after 7 days at the default 14-day half-life), so one kill stays a grudge that long.
constexpr int16 GRUDGE_AFTER_KILL = 150;
constexpr uint32 GRUDGE_KILL_WINDOW = 7 * 86400;
constexpr int16 GRUDGE_AFFINITY = -600;      // affinity this low is a grudge too
constexpr int16 RIVAL = 400;                 // rivalry
constexpr int16 DISLIKED_AFFINITY = -300;
constexpr int16 DISLIKED_ANNOYANCE = 600;
constexpr int16 ACQUAINTANCE_FAMILIARITY = 100;
constexpr uint16 ACQUAINTANCE_MEETINGS = 3;
constexpr int16 DUEL_REFUSE_AFFINITY = -600;  // duels refused at or below (RivalDuels)
constexpr int16 DUEL_REFUSE_ANNOYANCE = 600;  // or at or above
constexpr uint8 DUEL_REFUSE_FLED = 2;         // or when the other fled this many duels in a row
constexpr uint32 DUEL_FLED_WINDOW = 24 * 3600; // flees count for a day, and a flee after a longer gap restarts the run
constexpr uint32 DUEL_EAGER_HEALTH = 70;      // rivals, grudges and friends are accepted from this health percent
}  // namespace MemoryThreshold

// The label of decayed axes and counters. lastKilledMeAt: newest kill of the bot by the other outside
// battlegrounds (unix, 0 = never); bgGrudgeUntil: a battleground kill makes a grudge until then.
inline MemoryLabel MemoryLabelOf(std::array<int16, MEMORY_AXIS_COUNT> const& axes,
                                 std::array<uint16, MEMORY_COUNTER_COUNT> const& counters, uint32 lastKilledMeAt,
                                 uint32 bgGrudgeUntil, uint32 now)
{
    using namespace MemoryThreshold;
    auto const axis = [&axes](MemoryAxis a) { return axes[std::size_t(a)]; };
    bool const recentKill = lastKilledMeAt && now >= lastKilledMeAt && now - lastKilledMeAt <= GRUDGE_KILL_WINDOW;
    if (axis(MemoryAxis::Grudge) >= GRUDGE || (recentKill && axis(MemoryAxis::Grudge) >= GRUDGE_AFTER_KILL) ||
        axis(MemoryAxis::Affinity) <= GRUDGE_AFFINITY || bgGrudgeUntil > now)
        return MemoryLabel::Grudge;
    if (axis(MemoryAxis::Rivalry) >= RIVAL)
        return MemoryLabel::Rival;
    if (axis(MemoryAxis::Affinity) >= FRIEND_AFFINITY &&
        (axis(MemoryAxis::Trust) >= FRIEND_TRUST || axis(MemoryAxis::Familiarity) >= FRIEND_FAMILIARITY))
        return MemoryLabel::Friend;
    if (axis(MemoryAxis::Affinity) <= DISLIKED_AFFINITY || axis(MemoryAxis::Annoyance) >= DISLIKED_ANNOYANCE)
        return MemoryLabel::Disliked;
    if (axis(MemoryAxis::Familiarity) >= ACQUAINTANCE_FAMILIARITY ||
        counters[std::size_t(MemoryCounter::Meetings)] >= ACQUAINTANCE_MEETINGS)
        return MemoryLabel::Acquaintance;
    return MemoryLabel::Stranger;
}

// How a message sounded. s in [-1, 1] (hostile to friendly), c in [0, 1] (confidence); 0/0 moves nothing.
enum class MemorySentimentSource : uint8
{
    None = 0,   // no rating (CoaMemory.Sentiment = 0): counters only
    Heuristic,  // SentimentHeuristic (words, intent, emote table)
    Brain       // the voice decision's rating answers (CoaMemory.Sentiment = 2, CoaBrain.Use.Rating = 1)
};

struct MemorySentiment
{
    float s = 0.0f;
    float c = 0.0f;
    bool insult = false;
    bool thanks = false;
    MemorySentimentSource source = MemorySentimentSource::None;
};

// Where a message addressed to a bot was said.
enum class MemoryTalkKind : uint8
{
    Whisper = 0,
    Group,    // party or raid
    Guild,
    Say,      // say, yell, "/e"
    Channel,
    Emote     // a text emote aimed at the bot
};

// "whisper", "group", "guild", "say", "channel", "emote".
char const* MemoryTalkKindName(MemoryTalkKind kind);

// One message (or text emote) from a real player addressed to one bot, as the voice director saw it on the world
// thread. Plain values: the player may have logged out by the time a brain rating comes back. Never the words.
struct MemoryConversation
{
    ObjectGuid bot;
    ObjectGuid speaker;
    uint32 speakerAccount = 0;
    std::string speakerName;  // snapshot for other_name
    uint8 speakerLevel = 0;
    uint16 mapId = 0;         // the speaker's
    uint32 zoneId = 0;
    MemoryTalkKind kind = MemoryTalkKind::Say;
    CoaIntent intent = CoaIntent::None;  // CoaChatIntent::Classify, or the emote's intent
    uint32 textEmote = 0;                // Emote kind: the TextEmotes id
    MemorySentiment sentiment;
};

// How the voice director should sound toward someone (CoaMemory.MemoryReplies).
enum class MemoryTone : uint8
{
    None = 0,  // no memory, the switch is off, or nothing notable: M1/M2 lines
    Friend,    // label Friend: coa_mem_reply_friend for greetings, thanks, compliments, jokes and generic lines
    Cold,      // label Disliked: coa_mem_reply_cold for every intent, no persona quirks, no emote
    Grudge     // label Grudge: coa_mem_reply_grudge (coa_mem_reply_cold for a farewell), no quirks, no emote
};

#endif
