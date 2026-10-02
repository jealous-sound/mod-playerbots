/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityRecruitment.h"
#include "ActivityConfig.h"
#include "ActivityDirector.h"
#include "ActivityRegistry.h"
#include "CoaTime.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "WorldSession.h"
#include <algorithm>
#include <cctype>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace
{
constexpr char const* LOGGER = "playerbots.activity";

// The requests heard since the director's last tick and, per real player, when the last one was taken. Whispers and
// "lfg bot" requests have their own gaps: a player who asks in a channel may whisper a join word at once.
struct Queue
{
    std::mutex lock;
    std::vector<ActivityJoinRequest> requests;
    std::unordered_map<uint64, uint64> lastWhisperMs;  // player guid (raw) -> when its last whisper request was taken
    std::unordered_map<uint64, uint64> lastLfgMs;      // player guid (raw) -> when its last "lfg bot" was taken
};

Queue& Pending()
{
    static Queue queue;
    return queue;
}

bool IsBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Queues `request` unless the player asked less than JOIN_REQUEST_GAP_MS ago or the queue is full.
void Push(ActivityJoinRequest request, bool lfg)
{
    Queue& queue = Pending();
    std::lock_guard<std::mutex> guard(queue.lock);
    std::unordered_map<uint64, uint64>& last = lfg ? queue.lastLfgMs : queue.lastWhisperMs;
    uint64 const raw = request.player.GetRawValue();
    auto const it = last.find(raw);
    if (it != last.end() && request.atMs < it->second + ActivityThreshold::JOIN_REQUEST_GAP_MS)
    {
        LOG_DEBUG(LOGGER, "activity chat join: player {} asked again within {} ms, dropped",
                  request.player.GetCounter(), ActivityThreshold::JOIN_REQUEST_GAP_MS);
        return;
    }
    if (queue.requests.size() >= ActivityThreshold::JOIN_REQUEST_QUEUE_MAX)
    {
        LOG_DEBUG(LOGGER, "activity chat join: queue full, request of player {} dropped", request.player.GetCounter());
        return;
    }
    last[raw] = request.atMs;
    queue.requests.push_back(std::move(request));
}

void Prune(std::unordered_map<uint64, uint64>& last, uint64 nowMs)
{
    for (auto it = last.begin(); it != last.end();)
    {
        if (nowMs >= it->second + ActivityThreshold::JOIN_REQUEST_GAP_MS)
            it = last.erase(it);
        else
            ++it;
    }
}
}  // namespace

namespace ActivityRecruitment
{
bool OnWhisper(Player* sender, Player* receiver, std::string const& text)
{
    // Activities off: this one relaxed load, nothing else.
    if (!ActivityDirector::Running() || !sender || !receiver)
        return false;
    // Bots never join by chat.
    if (!sender->GetSession() || sender->GetSession()->IsBot())
        return false;
    PlayerbotAI* const ai = GET_PLAYERBOT_AI(receiver);
    if (!ai)
        return false;

    std::shared_ptr<ActivityConfig const> const config = ActivityConfig::Current();
    uint32 typedId = 0;
    bool const prefix = IsPrefixJoin(text, *config, typedId);

    // The receiver's row, for this life of the bot only.
    std::optional<ActivityMemberView> row = ActivityRegistry::Find(receiver->GetGUID().GetCounter());
    if (row && row->epoch != ai->GetCoaEpoch())
        row.reset();

    bool const keyword = !prefix && row && row->role == ActivityRole::Leader &&
                         row->Has(ACTIVITY_VIEW_RECRUITING) && IsJoinWord(text, *config);
    if (!prefix && !keyword)
        return false;

    // Consumed from here on, whatever happens to the request.
    ActivityJoinRequest request;
    request.player = sender->GetGUID();
    request.bot = receiver->GetGUID();
    request.source = keyword ? ActivityJoinSource::Keyword : ActivityJoinSource::Prefix;
    if (keyword)
        request.activityId = row->activityId;
    else
        request.activityId = typedId ? typedId : (row ? row->activityId : 0);
    request.atMs = CoaTime::NowMs();
    Push(std::move(request), false);
    return true;
}

void OnLfgRequest(Player* player, uint8 roleMask)
{
    if (!ActivityDirector::Running() || !player)
        return;
    if (!player->GetSession() || player->GetSession()->IsBot())
        return;

    ActivityJoinRequest request;
    request.player = player->GetGUID();
    request.source = ActivityJoinSource::LfgRequest;
    request.roleMask = roleMask;
    request.atMs = CoaTime::NowMs();
    Push(std::move(request), true);
}

void Drain(std::vector<ActivityJoinRequest>& out)
{
    Queue& queue = Pending();
    std::lock_guard<std::mutex> guard(queue.lock);
    if (!queue.requests.empty())
    {
        out.insert(out.end(), std::make_move_iterator(queue.requests.begin()),
                   std::make_move_iterator(queue.requests.end()));
        queue.requests.clear();
    }
    uint64 const nowMs = CoaTime::NowMs();
    Prune(queue.lastWhisperMs, nowMs);
    Prune(queue.lastLfgMs, nowMs);
}

void Clear()
{
    Queue& queue = Pending();
    std::lock_guard<std::mutex> guard(queue.lock);
    queue.requests.clear();
    queue.lastWhisperMs.clear();
    queue.lastLfgMs.clear();
}

std::string Normalize(std::string_view text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    // Blanks and trailing punctuation, in any order ("inv !", "x.").
    while (true)
    {
        while (begin < end && IsBlank(text[begin]))
            ++begin;
        std::size_t const before = end;
        while (end > begin && (IsBlank(text[end - 1]) || text[end - 1] == '.' || text[end - 1] == '!' ||
                               text[end - 1] == '?'))
            --end;
        if (end == before)
            break;
    }
    std::string out(text.substr(begin, end - begin));
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return c < 0x80 ? char(std::tolower(c)) : char(c); });
    return out;
}

bool IsJoinWord(std::string_view text, ActivityConfig const& config)
{
    if (config.chatJoinWords.empty())
        return false;
    std::string const word = Normalize(text);
    return !word.empty() &&
           std::find(config.chatJoinWords.begin(), config.chatJoinWords.end(), word) != config.chatJoinWords.end();
}

bool IsPrefixJoin(std::string_view text, ActivityConfig const& config, uint32& id)
{
    id = 0;
    if (config.whisperPrefix.empty())
        return false;
    std::string const normalized = Normalize(text);
    std::string_view rest = normalized;
    if (rest.substr(0, config.whisperPrefix.size()) != config.whisperPrefix)
        return false;
    rest.remove_prefix(config.whisperPrefix.size());

    static constexpr std::string_view Words[] = {"rejoindre", "join"};
    bool matched = false;
    for (std::string_view const word : Words)
    {
        if (rest.substr(0, word.size()) == word)
        {
            rest.remove_prefix(word.size());
            matched = true;
            break;
        }
    }
    if (!matched)
        return false;
    if (rest.empty())
        return true;

    // Blanks, then 1 to 9 digits, then the end.
    std::size_t blanks = 0;
    while (blanks < rest.size() && IsBlank(rest[blanks]))
        ++blanks;
    if (!blanks)
        return false;
    rest.remove_prefix(blanks);
    if (rest.empty() || rest.size() > 9)
        return false;
    uint32 number = 0;
    for (char const c : rest)
    {
        if (c < '0' || c > '9')
            return false;
        number = number * 10 + uint32(c - '0');
    }
    id = number;
    return true;
}
}  // namespace ActivityRecruitment
