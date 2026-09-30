/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaVoiceGovernor.h"
#include "CoaVoiceConfig.h"
#include "NameMatch.h"
#include <algorithm>
#include <string_view>

namespace
{
constexpr uint64 MINUTE_MS = 60 * 1000;
constexpr uint64 MEMORY_MS = 30 * 60 * 1000;  // repetition window, and idle time after which state is forgotten
constexpr std::size_t BOT_LINES = 10;
constexpr std::size_t OPENING_LINES = 10;  // scope lines searched for a reused opening (kept even if History is less)
constexpr uint32 OPENING_REPEATS = 2;      // an opening found this often is worn out
constexpr std::size_t MAX_SCOPES = 512;
constexpr std::size_t MIN_JACCARD_WORDS = 3;

// Steady clock: `then` is never after `now`, but a zero elapsed time is the safe answer if it were.
uint64 Elapsed(uint64 nowMs, uint64 thenMs) { return nowMs > thenMs ? nowMs - thenMs : 0; }

// Timestamps are pushed in time order, so the recent ones sit at the back.
uint32 CountWithin(std::deque<uint64> const& times, uint64 nowMs, uint64 windowMs)
{
    uint32 count = 0;
    for (auto it = times.rbegin(); it != times.rend() && Elapsed(nowMs, *it) < windowMs; ++it)
        ++count;
    return count;
}

void DropOlder(std::deque<uint64>& times, uint64 nowMs, uint64 windowMs)
{
    while (!times.empty() && Elapsed(nowMs, times.front()) >= windowMs)
        times.pop_front();
}

// The first two words of a normalized line, empty when it has fewer.
std::string_view Opening(std::string const& normalized)
{
    std::size_t const first = normalized.find(' ');
    if (first == std::string::npos)
        return {};
    std::size_t const second = normalized.find(' ', first + 1);
    return std::string_view(normalized).substr(0, second == std::string::npos ? normalized.size() : second);
}

float Jaccard(std::vector<std::string> const& a, std::vector<std::string> const& b)
{
    // Both are sorted and without duplicates.
    std::size_t common = 0;
    auto ia = a.begin();
    auto ib = b.begin();
    while (ia != a.end() && ib != b.end())
    {
        if (*ia < *ib)
            ++ia;
        else if (*ib < *ia)
            ++ib;
        else
        {
            ++common;
            ++ia;
            ++ib;
        }
    }
    std::size_t const all = a.size() + b.size() - common;
    return all ? float(common) / float(all) : 1.0f;
}

struct NormalizedLine
{
    std::string normalized;
    std::vector<std::string> words;  // sorted, unique
};

NormalizedLine Normalize(std::string const& line)
{
    NormalizedLine out;
    NameMatch::Prepared const prepared(line);
    for (std::string const& word : prepared.Words())
    {
        if (!out.normalized.empty())
            out.normalized += ' ';
        out.normalized += word;
    }
    out.words = prepared.Words();
    std::sort(out.words.begin(), out.words.end());
    out.words.erase(std::unique(out.words.begin(), out.words.end()), out.words.end());
    return out;
}
}  // namespace

CoaVoiceDrop CoaVoiceGovernor::PreCheck(CoaVoiceConfig const& config, ObjectGuid bot, CoaVoiceScope const& scope,
                                        ObjectGuid sender, uint64 nowMs) const
{
    bool const whisper = scope.kind == CoaVoiceScopeKind::Whisper;

    if (CountWithin(globalMs, nowMs, MINUTE_MS) >= config.globalPerMinute)
        return CoaVoiceDrop::GlobalRate;

    auto const botIt = bots.find(bot);
    if (botIt != bots.end())
    {
        uint64 const last = whisper ? botIt->second.lastWhisperMs : botIt->second.lastPublicMs;
        uint64 const cooldown = whisper ? config.cooldownWhisperMs : config.cooldownBotMs;
        if (last && Elapsed(nowMs, last) < cooldown)
            return CoaVoiceDrop::BotCooldown;
    }

    auto const scopeIt = scopes.find(scope.key);
    if (scopeIt != scopes.end())
    {
        ScopeState const& state = scopeIt->second;
        uint64 const cooldown = whisper ? config.cooldownWhisperMs : config.cooldownScopeMs;
        if (state.lastReplyMs && Elapsed(nowMs, state.lastReplyMs) < cooldown)
            return CoaVoiceDrop::ScopeCooldown;
        if (CountWithin(state.repliesMs, nowMs, MINUTE_MS) >= config.scopePerMinute)
            return CoaVoiceDrop::ScopeRate;
    }
    else if (config.scopePerMinute == 0)
        return CoaVoiceDrop::ScopeRate;

    if (scope.IsPublic())
    {
        auto const senderIt = senders.find(sender);
        if (senderIt != senders.end() && Elapsed(nowMs, senderIt->second) < config.cooldownSenderMs)
            return CoaVoiceDrop::SenderCooldown;
    }

    return CoaVoiceDrop::Count;
}

bool CoaVoiceGovernor::IsRepeat(CoaVoiceConfig const& config, ObjectGuid bot, uint64 scopeKey, std::string const& line,
                                uint64 nowMs) const
{
    NormalizedLine const candidate = Normalize(line);
    bool const jaccard = config.repetitionThreshold > 0.0f && candidate.words.size() >= MIN_JACCARD_WORDS;

    auto const same = [&](RecentLine const& recent)
    {
        if (recent.normalized == candidate.normalized)
            return true;
        return jaccard && recent.words.size() >= MIN_JACCARD_WORDS &&
               Jaccard(candidate.words, recent.words) >= config.repetitionThreshold;
    };

    auto const botIt = bots.find(bot);
    if (botIt != bots.end())
    {
        for (RecentLine const& recent : botIt->second.lines)
            if (Elapsed(nowMs, recent.atMs) < MEMORY_MS && same(recent))
                return true;
    }

    auto const scopeIt = scopes.find(scopeKey);
    if (scopeIt == scopes.end())
        return false;

    std::deque<RecentLine> const& lines = scopeIt->second.lines;
    std::string_view const opening = Opening(candidate.normalized);
    uint32 openings = 0;
    std::size_t seen = 0;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it, ++seen)
    {
        if (Elapsed(nowMs, it->atMs) >= MEMORY_MS)
            break;  // older lines sit in front of this one
        if (seen < config.repetitionHistory && same(*it))
            return true;
        if (!opening.empty() && seen < OPENING_LINES && Opening(it->normalized) == opening &&
            ++openings >= OPENING_REPEATS)
            return true;
    }

    return false;
}

void CoaVoiceGovernor::Reserve(CoaVoiceConfig const& config, ObjectGuid bot, CoaVoiceScope const& scope,
                               ObjectGuid sender, std::string const& line, uint64 nowMs)
{
    bool const whisper = scope.kind == CoaVoiceScopeKind::Whisper;

    NormalizedLine normalized = Normalize(line);
    RecentLine recent;
    recent.atMs = nowMs;
    recent.normalized = std::move(normalized.normalized);
    recent.words = std::move(normalized.words);

    BotState& botState = bots[bot];
    if (whisper)
        botState.lastWhisperMs = nowMs;
    else
        botState.lastPublicMs = nowMs;
    botState.lines.push_back(recent);
    while (botState.lines.size() > BOT_LINES)
        botState.lines.pop_front();

    ScopeState& scopeState = scopes[scope.key];
    scopeState.lastReplyMs = nowMs;
    scopeState.touchedMs = nowMs;
    DropOlder(scopeState.repliesMs, nowMs, MINUTE_MS);
    scopeState.repliesMs.push_back(nowMs);
    scopeState.lines.push_back(std::move(recent));
    while (scopeState.lines.size() > std::max<std::size_t>(config.repetitionHistory, OPENING_LINES))
        scopeState.lines.pop_front();

    if (scope.IsPublic())
        senders[sender] = nowMs;

    DropOlder(globalMs, nowMs, MINUTE_MS);
    globalMs.push_back(nowMs);
}

void CoaVoiceGovernor::NoteHumanSpoke(uint64 scopeKey, uint64 nowMs)
{
    ScopeState& state = scopes[scopeKey];
    state.lastHumanMs = nowMs;
    state.touchedMs = nowMs;
}

bool CoaVoiceGovernor::HumanSpokeWithin(uint64 scopeKey, uint64 nowMs, uint64 windowMs) const
{
    auto const it = scopes.find(scopeKey);
    return it != scopes.end() && it->second.lastHumanMs && Elapsed(nowMs, it->second.lastHumanMs) <= windowMs;
}

void CoaVoiceGovernor::Prune(uint64 nowMs)
{
    for (auto it = bots.begin(); it != bots.end();)
    {
        BotState& state = it->second;
        if (Elapsed(nowMs, std::max(state.lastPublicMs, state.lastWhisperMs)) >= MEMORY_MS)
        {
            it = bots.erase(it);
            continue;
        }
        while (!state.lines.empty() && Elapsed(nowMs, state.lines.front().atMs) >= MEMORY_MS)
            state.lines.pop_front();
        ++it;
    }

    for (auto it = senders.begin(); it != senders.end();)
    {
        if (Elapsed(nowMs, it->second) >= MEMORY_MS)
            it = senders.erase(it);
        else
            ++it;
    }

    for (auto it = scopes.begin(); it != scopes.end();)
    {
        ScopeState& state = it->second;
        if (Elapsed(nowMs, state.touchedMs) >= MEMORY_MS)
        {
            it = scopes.erase(it);
            continue;
        }
        DropOlder(state.repliesMs, nowMs, MINUTE_MS);
        while (!state.lines.empty() && Elapsed(nowMs, state.lines.front().atMs) >= MEMORY_MS)
            state.lines.pop_front();
        ++it;
    }

    if (scopes.size() > MAX_SCOPES)
    {
        std::vector<std::pair<uint64, uint64>> byAge;  // touchedMs, key
        byAge.reserve(scopes.size());
        for (auto const& [key, state] : scopes)
            byAge.emplace_back(state.touchedMs, key);

        std::size_t const excess = scopes.size() - MAX_SCOPES;
        std::nth_element(byAge.begin(), byAge.begin() + std::ptrdiff_t(excess), byAge.end());
        for (std::size_t i = 0; i < excess; ++i)
            scopes.erase(byAge[i].second);
    }

    DropOlder(globalMs, nowMs, MINUTE_MS);
}

void CoaVoiceGovernor::Reset()
{
    bots.clear();
    scopes.clear();
    senders.clear();
    globalMs.clear();
}

uint64 CoaVoiceGovernor::LastAnswerMs(ObjectGuid bot) const
{
    auto const it = bots.find(bot);
    return it == bots.end() ? 0 : std::max(it->second.lastPublicMs, it->second.lastWhisperMs);
}

uint32 CoaVoiceGovernor::RepliesLastMinute(uint64 nowMs) const { return CountWithin(globalMs, nowMs, MINUTE_MS); }
