/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MemoryRetrieval.h"
#include "BotMemory.h"
#include "DBCStores.h"
#include "StringFormat.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <utility>

namespace
{
bool MatchesContext(MemoryEventKind kind, MemoryRetrieval::Context context)
{
    switch (context)
    {
        case MemoryRetrieval::Context::Chat:
            return kind == MemoryEventKind::ChatFriendly || kind == MemoryEventKind::ChatHostile ||
                   kind == MemoryEventKind::Thanked || kind == MemoryEventKind::Insulted ||
                   kind == MemoryEventKind::EmoteFriendly || kind == MemoryEventKind::EmoteHostile;
        case MemoryRetrieval::Context::Duel:
            return kind == MemoryEventKind::DuelWon || kind == MemoryEventKind::DuelLost ||
                   kind == MemoryEventKind::DuelFled;
        case MemoryRetrieval::Context::Pvp:
            return kind == MemoryEventKind::KilledBy || kind == MemoryEventKind::Killed;
        case MemoryRetrieval::Context::Group:
            return kind == MemoryEventKind::Grouped || kind == MemoryEventKind::KickedBy ||
                   kind == MemoryEventKind::BossKill || kind == MemoryEventKind::LeaderKill ||
                   kind == MemoryEventKind::Helped || kind == MemoryEventKind::HelpedThem ||
                   kind == MemoryEventKind::Traded;
    }
    return false;
}

// The English DBC name of an area, empty when unknown.
std::string ZoneName(uint32 zoneId)
{
    if (!zoneId)
        return "";
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
    if (!area || !area->area_name[LOCALE_enUS])
        return "";
    return area->area_name[LOCALE_enUS];
}

// Axes in points (1/10 truncated), decayed to `now`. `all`: every axis, else only the non-zero ones.
std::string AxesText(MemoryRelation const& relation, MemoryDecay const& decay, uint32 now, bool all)
{
    std::string text;
    for (std::size_t i = 0; i < MEMORY_AXIS_COUNT; ++i)
    {
        MemoryAxis const axis = MemoryAxis(i);
        int32 const value = decay.Apply(relation.axes[i], axis, relation.decayedAt, now);
        if (!all && value / 10 == 0)
            continue;
        if (!text.empty())
            text += ", ";
        text += Acore::StringFormat("{} {}", MemoryAxisName(axis), value / 10);
    }
    return text;
}

std::string UtcTime(uint32 ts, char const* format)
{
    time_t const t = time_t(ts);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &t);
#else
    gmtime_r(&t, &parts);
#endif
    char buffer[32];
    std::size_t const n = std::strftime(buffer, sizeof(buffer), format, &parts);
    return std::string(buffer, n);
}
}  // namespace

std::vector<MemoryEvent const*> MemoryRetrieval::Rank(BotMemory const& memory, uint32 aboutGuid, uint32 zoneId,
                                                      Context context, uint32 now, std::size_t max, bool aboutOnly)
{
    struct Scored
    {
        MemoryEvent const* event;
        float score;
        std::size_t order;  // position in memory.events: later is newer
    };

    std::vector<Scored> scored;
    scored.reserve(memory.events.size());
    std::size_t order = 0;
    for (MemoryEvent const& event : memory.events)
    {
        if (aboutOnly && event.otherGuid != aboutGuid)
            continue;
        double const ageHours = now > event.ts ? double(now - event.ts) / 3600.0 : 0.0;
        float relevance = 0.0f;
        if (aboutGuid && event.otherGuid == aboutGuid)
            relevance = 1.0f;
        else if (zoneId && event.zoneId == zoneId)
            relevance = 0.6f;
        else if (MatchesContext(event.kind, context))
            relevance = 0.4f;
        float const score =
            0.5f * float(event.importance) / 10.0f + 0.3f * float(std::exp(-ageHours / 72.0)) + 0.2f * relevance;
        scored.push_back({&event, score, order++});
    }

    auto const better = [](Scored const& a, Scored const& b)
    {
        if (a.score != b.score)
            return a.score > b.score;
        if (a.event->ts != b.event->ts)
            return a.event->ts > b.event->ts;
        return a.order > b.order;
    };
    std::size_t const keep = std::min(max, scored.size());
    std::partial_sort(scored.begin(), scored.begin() + keep, scored.end(), better);

    std::vector<MemoryEvent const*> ranked;
    ranked.reserve(keep);
    for (std::size_t i = 0; i < keep; ++i)
        ranked.push_back(scored[i].event);
    return ranked;
}

std::string MemoryRetrieval::BrainRelationLine(std::string_view label, MemoryRelation const& relation,
                                               MemoryDecay const& decay, uint32 now)
{
    std::array<int16, MEMORY_AXIS_COUNT> axes{};
    for (std::size_t i = 0; i < MEMORY_AXIS_COUNT; ++i)
        axes[i] = decay.Apply(relation.axes[i], MemoryAxis(i), relation.decayedAt, now);
    MemoryLabel const memoryLabel =
        MemoryLabelOf(axes, relation.counters, relation.lastKilledMeAt, relation.bgGrudgeUntil, now);

    std::string line = Acore::StringFormat("{} and the speaker: {}", label, MemoryLabelName(memoryLabel));
    std::string const axesText = AxesText(relation, decay, now, false);
    if (!axesText.empty())
        line += "; " + axesText;

    auto const counter = [&relation](MemoryCounter c) { return uint32(relation.counters[std::size_t(c)]); };
    auto const add = [&line](std::string const& part) { line += "; " + part; };
    if (uint32 const n = counter(MemoryCounter::Meetings))
        add(Acore::StringFormat("met {} times, last {} ago", n,
                                AgeText(now > relation.lastSeen ? now - relation.lastSeen : 0)));
    if (uint32 const n = counter(MemoryCounter::Chats))
        add(Acore::StringFormat("chats {}", n));
    if (uint32 const n = counter(MemoryCounter::HelpedMe))
        add(Acore::StringFormat("helped {} {} times", label, n));
    if (uint32 const n = counter(MemoryCounter::HelpedThem))
        add(Acore::StringFormat("helped by {} {} times", label, n));
    if (uint32 const n = counter(MemoryCounter::KilledMe))
        add(Acore::StringFormat("killed {} {} times", label, n));
    if (uint32 const n = counter(MemoryCounter::KilledThem))
        add(Acore::StringFormat("killed by {} {} times", label, n));
    uint32 const won = counter(MemoryCounter::DuelsWon);
    uint32 const lost = counter(MemoryCounter::DuelsLost);
    if (won || lost)
        add(Acore::StringFormat("{} won {} duels, lost {}", label, won, lost));  // the bot's side: unambiguous
    if (uint32 const n = counter(MemoryCounter::GroupsShared))
        add(Acore::StringFormat("grouped {} times", n));
    if (uint32 const n = counter(MemoryCounter::Trades))
        add(Acore::StringFormat("traded {} times", n));
    return line;
}

std::string MemoryRetrieval::BrainEventLine(std::string_view label, MemoryEvent const& event, uint32 now)
{
    std::string const b(label);
    std::string phrase;
    switch (event.kind)
    {
        case MemoryEventKind::Met:
            phrase = "met the speaker";
            break;
        case MemoryEventKind::ChatFriendly:
            phrase = "the speaker was friendly in chat";
            break;
        case MemoryEventKind::ChatHostile:
            phrase = "the speaker was hostile in chat";
            break;
        case MemoryEventKind::Thanked:
            phrase = "the speaker thanked " + b;
            break;
        case MemoryEventKind::Insulted:
            phrase = "the speaker insulted " + b;
            break;
        case MemoryEventKind::EmoteFriendly:
            phrase = "the speaker made a friendly gesture";
            break;
        case MemoryEventKind::EmoteHostile:
            phrase = "the speaker made a rude gesture";
            break;
        case MemoryEventKind::DuelWon:
            phrase = b + " won a duel against the speaker";
            break;
        case MemoryEventKind::DuelLost:
            phrase = b + " lost a duel to the speaker";
            break;
        case MemoryEventKind::DuelFled:
            phrase = "the speaker fled a duel with " + b;
            break;
        case MemoryEventKind::KilledBy:
            phrase = "the speaker killed " + b;
            break;
        case MemoryEventKind::Killed:
            phrase = b + " killed the speaker";
            break;
        case MemoryEventKind::Helped:
            phrase = "the speaker helped " + b + " in a fight";
            break;
        case MemoryEventKind::HelpedThem:
            phrase = b + " helped the speaker in a fight";
            break;
        case MemoryEventKind::Grouped:
            phrase = b + " grouped with the speaker";
            break;
        case MemoryEventKind::KickedBy:
            phrase = "the speaker kicked " + b + " from a group";
            break;
        case MemoryEventKind::Traded:
            phrase = b + " traded with the speaker";
            break;
        case MemoryEventKind::BossKill:
            phrase = b + " and the speaker killed a boss together";
            break;
        case MemoryEventKind::LeaderKill:
            phrase = b + " and the speaker killed a faction leader together";
            break;
        default:
            phrase = "something happened";
            break;
    }

    std::string const age = AgeText(now > event.ts ? now - event.ts : 0);
    std::string line = Acore::StringFormat("{} [{}] {}", label, age == "now" ? "just now" : age + " ago", phrase);
    std::string const zone = ZoneName(event.zoneId);
    if (!zone.empty())
        line += " in " + zone;
    return line;
}

std::string MemoryRetrieval::ShowRelationLine(MemoryRelation const& relation, MemoryDecay const& decay, uint32 now)
{
    std::array<int16, MEMORY_AXIS_COUNT> axes{};
    for (std::size_t i = 0; i < MEMORY_AXIS_COUNT; ++i)
        axes[i] = decay.Apply(relation.axes[i], MemoryAxis(i), relation.decayedAt, now);
    MemoryLabel const label =
        MemoryLabelOf(axes, relation.counters, relation.lastKilledMeAt, relation.bgGrudgeUntil, now);

    std::string const name =
        relation.otherName.empty() ? Acore::StringFormat("#{}", relation.otherGuid) : relation.otherName;
    std::string const who =
        relation.otherIsBot ? std::string("bot") : Acore::StringFormat("player, account {}", relation.otherAccount);

    std::string counters;
    for (std::size_t i = 0; i < MEMORY_COUNTER_COUNT; ++i)
    {
        if (!counters.empty())
            counters += ", ";
        counters += Acore::StringFormat("{} {}", MemoryCounterName(MemoryCounter(i)), relation.counters[i]);
    }

    return Acore::StringFormat("{} ({}): {}; {}; {}; seen {} ago", name, who, MemoryLabelName(label),
                               AxesText(relation, decay, now, true), counters,
                               AgeText(now > relation.lastSeen ? now - relation.lastSeen : 0));
}

std::string MemoryRetrieval::ShowEventLine(MemoryEvent const& event, std::string const& otherName, uint32 /*now*/)
{
    std::string line =
        Acore::StringFormat("{} UTC {} {} ({})", UtcTime(event.ts, "%Y-%m-%d %H:%M"), MemoryEventKindName(event.kind),
                            otherName.empty() ? std::string("-") : otherName, uint32(event.importance));
    std::string const zone = ZoneName(event.zoneId);
    if (!zone.empty())
        line += " " + zone;
    if (!event.detail.empty())
        line += ": " + event.detail;
    return line;
}

std::string MemoryRetrieval::AgeText(uint32 seconds)
{
    if (seconds < 60)
        return "now";
    if (seconds < 3600)
        return Acore::StringFormat("{} min", seconds / 60);
    if (seconds < 48 * 3600)
        return Acore::StringFormat("{} h", seconds / 3600);
    return Acore::StringFormat("{} d", seconds / 86400);
}

uint32 MemoryRetrieval::EstimateTokens(std::string_view text) { return uint32((text.size() + 3) / 4); }
