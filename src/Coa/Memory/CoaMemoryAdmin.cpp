/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaMemoryAdmin.h"
#include "AccountMgr.h"
#include "BotMemory.h"
#include "CharacterCache.h"
#include "CoaMemoryConfig.h"
#include "CoaMemoryMgr.h"
#include "CoaMemoryRepository.h"
#include "CoaThread.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "MemoryRetrieval.h"
#include "ObjectMgr.h"
#include "PlayerbotTextMgr.h"
#include "QueryCallback.h"
#include "QueryResult.h"
#include "StringFormat.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <sstream>

namespace
{
constexpr char const* OFF_LINE = "CoA memory is off";
constexpr uint32 SHOW_LIMIT = 10;
constexpr uint32 MINE_PER_PAGE = 5;
constexpr uint32 MINE_EVENTS = 100;
constexpr std::size_t MINE_EVENTS_PER_BOT = 8;

uint32 NowUnix() { return uint32(time(nullptr)); }

ObjectGuid PlayerGuid(uint32 low) { return ObjectGuid::Create<HighGuid::Player>(low); }

std::string Text(std::string const& name, std::string const& english,
                 std::map<std::string, std::string> const& placeholders = {})
{
    return PlayerbotTextMgr::instance().GetBotTextOrDefault(name, english, placeholders);
}

// A character by name as typed, quotes allowed ("kaelis dawnfire", "'Kaelis Dawnfire'"); empty when unknown.
ObjectGuid GuidByName(std::string name)
{
    if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'') && name.back() == name.front())
        name = name.substr(1, name.size() - 2);
    if (!normalizePlayerName(name))
        return ObjectGuid::Empty;
    return sCharacterCache->GetCharacterGuidByName(name);
}

std::string NameOf(uint32 guid)
{
    if (!guid)
        return "-";
    std::string name;
    if (!sCharacterCache->GetCharacterNameByGuid(PlayerGuid(guid), name) || name.empty())
        return Acore::StringFormat("#{}", guid);
    return name;
}

std::string Date(uint32 ts)
{
    time_t const t = time_t(ts);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &t);
#else
    gmtime_r(&t, &parts);
#endif
    char buffer[16];
    std::size_t const n = std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &parts);
    return std::string(buffer, n);
}

std::vector<std::string> Lines(std::string const& text)
{
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

// "coa_mem_kind_killed_by" for KilledBy, with its English default (the player's point of view).
std::string KindText(MemoryEventKind kind)
{
    static char const* const English[] = {
        "something",                            // None
        "you met",                              // Met
        "a friendly message",                   // ChatFriendly
        "a hostile message",                    // ChatHostile
        "you thanked it",                       // Thanked
        "you insulted it",                      // Insulted
        "a friendly gesture",                   // EmoteFriendly
        "a rude gesture",                       // EmoteHostile
        "it won a duel against you",            // DuelWon
        "you won a duel against it",            // DuelLost
        "you fled a duel with it",              // DuelFled
        "you killed it",                        // KilledBy
        "it killed you",                        // Killed
        "you helped it in a fight",             // Helped
        "it helped you in a fight",             // HelpedThem
        "you grouped together",                 // Grouped
        "you kicked it from a group",           // KickedBy
        "you traded",                           // Traded
        "you killed a boss together",           // BossKill
        "you killed a faction leader together"  // LeaderKill
    };
    std::size_t const index = std::size_t(kind);
    if (!index || index >= std::size_t(MemoryEventKind::Count))
        return English[0];
    std::string name = MemoryEventKindName(kind);
    std::replace(name.begin(), name.end(), '-', '_');
    return Text("coa_mem_kind_" + name, English[index]);
}

std::string LabelText(MemoryLabel label)
{
    return Text(std::string("coa_mem_label_") + MemoryLabelName(label), MemoryLabelName(label));
}

// Relations to show first when no player is named: the strongest feelings.
int32 Weight(MemoryRelation const& relation, MemoryDecay const& decay, uint32 now)
{
    auto const axis = [&](MemoryAxis a)
    { return int32(decay.Apply(relation.axes[std::size_t(a)], a, relation.decayedAt, now)); };
    return std::abs(axis(MemoryAxis::Affinity)) + axis(MemoryAxis::Grudge) + axis(MemoryAxis::Rivalry) +
           axis(MemoryAxis::Familiarity);
}
}  // namespace

void CoaMemoryAdmin::Print(uint32 requester, std::string const& line)
{
    CoaMemoryMgr& mgr = CoaMemoryMgr::Instance();
    if (mgr.printer)
        mgr.printer(requester, line);
}

std::vector<std::string> CoaMemoryAdmin::Show(uint32 requester, std::string const& botName,
                                              std::string const& playerName)
{
    COA_ASSERT_WORLD_THREAD();
    CoaMemoryMgr& mgr = CoaMemoryMgr::Instance();
    if (!mgr.running)
        return {OFF_LINE};

    ObjectGuid const bot = GuidByName(botName);
    if (!bot)
        return {"unknown character " + botName};
    ObjectGuid other;
    if (!playerName.empty())
    {
        other = GuidByName(playerName);
        if (!other)
            return {"unknown character " + playerName};
    }

    std::string const botLabel = NameOf(bot.GetCounter());
    uint32 const now = NowUnix();
    MemoryDecay const decay = mgr.Config().decay;
    std::vector<std::string> lines;

    auto const found = mgr.entries.find(bot.GetCounter());
    if (found != mgr.entries.end() && found->second.state == CoaMemoryMgr::EntryState::Ready)
    {
        BotMemory const& memory = *found->second.memory;
        lines.push_back(Acore::StringFormat("memory of {} (online): {} relations, {} events in RAM", botLabel,
                                            memory.relations.size(), memory.events.size()));
        if (other)
        {
            if (MemoryRelation const* relation = memory.Find(other.GetCounter()))
                lines.push_back(MemoryRetrieval::ShowRelationLine(*relation, decay, now));
            else
                lines.push_back(
                    Acore::StringFormat("{} keeps no relation with {}", botLabel, NameOf(other.GetCounter())));
        }
        else
        {
            std::vector<MemoryRelation const*> strongest;
            for (auto const& [otherGuid, relation] : memory.relations)
                strongest.push_back(&relation);
            std::size_t const keep = std::min<std::size_t>(SHOW_LIMIT, strongest.size());
            std::partial_sort(strongest.begin(), strongest.begin() + keep, strongest.end(),
                              [&](MemoryRelation const* a, MemoryRelation const* b)
                              { return Weight(*a, decay, now) > Weight(*b, decay, now); });
            for (std::size_t i = 0; i < keep; ++i)
                lines.push_back(MemoryRetrieval::ShowRelationLine(*strongest[i], decay, now));
        }

        uint32 shown = 0;
        for (auto it = memory.events.rbegin(); it != memory.events.rend() && shown < SHOW_LIMIT; ++it)
        {
            if (other && it->otherGuid != other.GetCounter())
                continue;
            lines.push_back(MemoryRetrieval::ShowEventLine(*it, NameOf(it->otherGuid), now));
            ++shown;
        }
        return lines;
    }

    // Offline, or still loading: the database answers a few ticks later.
    lines.push_back(Acore::StringFormat("memory of {}: reading the database...", botLabel));
    struct ShowState
    {
        QueryResult relations;
    };
    auto const state = std::make_shared<ShowState>();
    uint32 const botGuid = bot.GetCounter();
    uint32 const otherGuid = other.GetCounter();
    CoaMemoryRepository& repository = CoaMemoryRepository::Instance();
    mgr.AddQuery(
        repository.ShowRelations(botGuid, otherGuid, SHOW_LIMIT)
            .WithChainingCallback(
                [state, botGuid, otherGuid](QueryCallback& callback, QueryResult relations)
                {
                    state->relations = std::move(relations);
                    callback.SetNextQuery(CoaMemoryRepository::Instance().ShowEvents(botGuid, otherGuid, SHOW_LIMIT));
                })
            .WithCallback(
                [state, requester, botGuid, botLabel](QueryResult events)
                {
                    uint32 const now = NowUnix();
                    MemoryDecay const decay = CoaMemoryMgr::Instance().Config().decay;
                    BotMemory memory(botGuid);
                    CoaMemoryRepository::ReadRelations(state->relations, memory);
                    std::vector<MemoryRelation const*> relations;
                    for (auto const& [guid, relation] : memory.relations)
                        relations.push_back(&relation);
                    std::sort(relations.begin(), relations.end(), [](MemoryRelation const* a, MemoryRelation const* b)
                              { return a->lastSeen > b->lastSeen; });

                    std::vector<MemoryEvent> rows;
                    if (events)
                    {
                        do
                        {
                            Field* fields = events->Fetch();
                            MemoryEvent event;
                            event.otherGuid = fields[0].Get<uint32>();
                            event.kind = MemoryEventKind(fields[1].Get<uint8>());
                            event.importance = fields[2].Get<uint8>();
                            event.mapId = fields[3].Get<uint16>();
                            event.zoneId = fields[4].Get<uint32>();
                            event.ts = fields[5].Get<uint32>();
                            event.detail = fields[6].Get<std::string>();
                            rows.push_back(std::move(event));
                        } while (events->NextRow());
                    }

                    Print(requester, Acore::StringFormat("memory of {} (database): {} relations, {} events shown",
                                                         botLabel, relations.size(), rows.size()));
                    for (MemoryRelation const* relation : relations)
                        Print(requester, MemoryRetrieval::ShowRelationLine(*relation, decay, now));
                    for (MemoryEvent const& event : rows)
                        Print(requester, MemoryRetrieval::ShowEventLine(event, NameOf(event.otherGuid), now));
                }));
    return lines;
}

std::vector<std::string> CoaMemoryAdmin::Forget(std::string const& botName, std::string const& playerName)
{
    COA_ASSERT_WORLD_THREAD();
    CoaMemoryMgr& mgr = CoaMemoryMgr::Instance();
    if (!mgr.running)
        return {OFF_LINE};

    ObjectGuid const bot = GuidByName(botName);
    if (!bot)
        return {"unknown character " + botName};
    ObjectGuid const other = GuidByName(playerName);
    if (!other)
        return {"unknown character " + playerName};

    uint32 const botGuid = bot.GetCounter();
    uint32 const otherGuid = other.GetCounter();
    std::size_t removed = 0;
    auto const found = mgr.entries.find(botGuid);
    if (found != mgr.entries.end())
    {
        std::erase_if(found->second.pendingNotes,
                      [otherGuid](MemoryConversation const& note) { return note.speaker.GetCounter() == otherGuid; });
        if (found->second.state == CoaMemoryMgr::EntryState::Ready)
        {
            removed = found->second.memory->Forget(otherGuid);
            found->second.viewDirty = true;
        }
    }
    auto const closed = mgr.recentlyClosed.find(botGuid);
    if (closed != mgr.recentlyClosed.end())
        removed += closed->second.memory->Forget(otherGuid);

    CoaMemoryRepository::Instance().DeletePair(botGuid, otherGuid);
    // A load in flight may have read the rows before the DELETE: it starts again behind it.
    mgr.RestartLoads(botGuid);
    return {Acore::StringFormat("{} forgot {} ({} things in RAM)", NameOf(botGuid), NameOf(otherGuid), removed)};
}

std::vector<std::string> CoaMemoryAdmin::Wipe(std::string const& botName)
{
    COA_ASSERT_WORLD_THREAD();
    CoaMemoryMgr& mgr = CoaMemoryMgr::Instance();
    if (!mgr.running)
        return {OFF_LINE};

    ObjectGuid const bot = GuidByName(botName);
    if (!bot)
        return {"unknown character " + botName};

    uint32 const botGuid = bot.GetCounter();
    auto const found = mgr.entries.find(botGuid);
    if (found != mgr.entries.end())
    {
        found->second.pendingEvents.clear();
        found->second.pendingNotes.clear();
        if (found->second.state == CoaMemoryMgr::EntryState::Ready)
        {
            found->second.memory->Clear();
            found->second.viewDirty = true;
        }
    }
    auto const closed = mgr.recentlyClosed.find(botGuid);
    if (closed != mgr.recentlyClosed.end())
        closed->second.memory->Clear();

    CoaMemoryRepository::Instance().DeleteBot(botGuid);
    mgr.RestartLoads(botGuid);
    return {Acore::StringFormat("{} forgot everything", NameOf(botGuid))};
}

std::vector<std::string> CoaMemoryAdmin::Purge(std::string const& target)
{
    COA_ASSERT_WORLD_THREAD();
    CoaMemoryMgr& mgr = CoaMemoryMgr::Instance();
    if (!mgr.running)
        return {OFF_LINE};
    if (target.empty())
        return {"purge needs a character name, an account name or #<account id>"};

    if (target.front() == '#')
    {
        char* end = nullptr;
        unsigned long const account = std::strtoul(target.c_str() + 1, &end, 10);
        if (!end || *end || !account || account > 0xFFFFFFFFul)
            return {"bad account id " + target};
        mgr.PurgeAccount(uint32(account));
        return {Acore::StringFormat("bots forgot the characters of account #{}", account)};
    }

    if (ObjectGuid const character = GuidByName(target))
    {
        // Rows about the character, and its own when it is a bot (PurgeCharacter covers both).
        uint32 const guid = character.GetCounter();
        mgr.ForgetCharacter(guid);
        auto const found = mgr.entries.find(guid);
        if (found != mgr.entries.end() && found->second.state == CoaMemoryMgr::EntryState::Ready)
        {
            found->second.memory->Clear();
            found->second.viewDirty = true;
        }
        auto const closed = mgr.recentlyClosed.find(guid);
        if (closed != mgr.recentlyClosed.end())
            closed->second.memory->Clear();
        CoaMemoryRepository::Instance().PurgeCharacter(guid);
        mgr.RestartLoads(0);
        return {Acore::StringFormat("bots forgot {} (and its own memory, if it kept one)", NameOf(guid))};
    }

    std::string accountName = target;
    std::transform(accountName.begin(), accountName.end(), accountName.begin(),
                   [](unsigned char c) { return char(std::toupper(c)); });
    uint32 const account = AccountMgr::GetId(accountName);
    if (!account)
        return {"unknown character or account " + target};
    mgr.PurgeAccount(account);
    return {Acore::StringFormat("bots forgot the characters of account {} (#{})", accountName, account)};
}

std::vector<std::string> CoaMemoryAdmin::Stats(uint32 requester)
{
    COA_ASSERT_WORLD_THREAD();
    CoaMemoryMgr& mgr = CoaMemoryMgr::Instance();
    if (!mgr.running)
    {
        std::vector<std::string> lines{OFF_LINE};
        // Enabled but not running: the tables are missing; the stats say so.
        if (CoaMemoryConfig::Current()->enable)
            for (std::string const& line : Lines(mgr.FormatStats()))
                lines.push_back(line);
        return lines;
    }

    mgr.AddQuery(CoaMemoryRepository::Instance().Counts().WithCallback(
        [requester](QueryResult result)
        {
            if (!result)
                return;
            Field* fields = result->Fetch();
            Print(requester, Acore::StringFormat("database: {} relations, {} events", fields[0].Get<uint64>(),
                                                 fields[1].Get<uint64>()));
        }));
    return Lines(mgr.FormatStats());
}

std::vector<std::string> CoaMemoryAdmin::Mine(uint32 requester, uint32 accountId, uint32 page)
{
    COA_ASSERT_WORLD_THREAD();
    CoaMemoryMgr& mgr = CoaMemoryMgr::Instance();
    if (!mgr.running)
        return {Text("coa_mem_off", "Bot memory is off on this realm.")};
    if (!accountId)
        return {};
    page = std::clamp<uint32>(page, 1, 100000);

    // What is still in RAM about this account goes first: the SELECT below runs after it on the one worker.
    for (auto& [guid, entry] : mgr.entries)
    {
        if (entry.state != CoaMemoryMgr::EntryState::Ready)
            continue;
        BotMemory& memory = *entry.memory;
        bool about = std::any_of(memory.unsaved.begin(), memory.unsaved.end(),
                                 [accountId](MemoryEvent const& event) { return event.otherAccount == accountId; });
        for (auto it = memory.dirty.begin(); !about && it != memory.dirty.end(); ++it)
            if (MemoryRelation const* relation = memory.Find(*it))
                about = relation->otherAccount == accountId;
        if (about)
            mgr.FinalFlush(memory);
    }

    struct Row
    {
        uint32 bot = 0;
        uint32 other = 0;
        std::array<int16, MEMORY_AXIS_COUNT> axes{};
        std::array<uint16, MEMORY_COUNTER_COUNT> counters{};
        uint32 lastSeen = 0;
        uint32 decayedAt = 0;
    };
    struct MineState
    {
        std::vector<Row> rows;
        bool more = false;
    };
    auto const state = std::make_shared<MineState>();

    mgr.AddQuery(
        CoaMemoryRepository::Instance()
            .MineRelations(accountId, (page - 1) * MINE_PER_PAGE, MINE_PER_PAGE + 1)
            .WithChainingCallback(
                [state, requester, accountId](QueryCallback& callback, QueryResult result)
                {
                    if (result)
                    {
                        do
                        {
                            Field* fields = result->Fetch();
                            Row row;
                            row.bot = fields[0].Get<uint32>();
                            row.other = fields[1].Get<uint32>();
                            for (std::size_t i = 0; i < MEMORY_AXIS_COUNT; ++i)
                                row.axes[i] = MemoryClampAxis(MemoryAxis(i), fields[2 + i].Get<int16>());
                            for (std::size_t i = 0; i < MEMORY_COUNTER_COUNT; ++i)
                                row.counters[i] = fields[12 + i].Get<uint16>();
                            row.lastSeen = fields[22].Get<uint32>();
                            row.decayedAt = fields[23].Get<uint32>();
                            state->rows.push_back(row);
                        } while (result->NextRow());
                    }

                    if (state->rows.size() > MINE_PER_PAGE)
                    {
                        state->more = true;
                        state->rows.resize(MINE_PER_PAGE);
                    }
                    if (state->rows.empty())
                    {
                        Print(requester, Text("coa_mem_mine_empty", "No bot remembers you."));
                        return;  // ends the chain
                    }

                    std::vector<uint32> bots;
                    for (Row const& row : state->rows)
                        if (std::find(bots.begin(), bots.end(), row.bot) == bots.end())
                            bots.push_back(row.bot);
                    callback.SetNextQuery(CoaMemoryRepository::Instance().MineEvents(accountId, bots, MINE_EVENTS));
                })
            .WithCallback(
                [state, requester, page](QueryResult result)
                {
                    struct Seen
                    {
                        MemoryEventKind kind;
                        uint32 ts;
                    };
                    std::map<std::pair<uint32, uint32>, std::vector<Seen>> events;  // (bot, other), newest first
                    if (result)
                    {
                        do
                        {
                            Field* fields = result->Fetch();
                            uint8 const kind = fields[2].Get<uint8>();
                            if (kind < uint8(MemoryEventKind::Met) || kind >= uint8(MemoryEventKind::Count))
                                continue;
                            events[{fields[0].Get<uint32>(), fields[1].Get<uint32>()}].push_back(
                                {MemoryEventKind(kind), fields[3].Get<uint32>()});
                        } while (result->NextRow());
                    }

                    uint32 const now = NowUnix();
                    MemoryDecay const decay = CoaMemoryMgr::Instance().Config().decay;
                    Print(requester,
                          Text("coa_mem_mine_header", "What bots remember about your characters, page %page:",
                               {{"%page", std::to_string(page)}}));
                    for (Row const& row : state->rows)
                    {
                        std::vector<Seen> const& seen = events[{row.bot, row.other}];
                        uint32 lastKilledMeAt = 0;
                        for (Seen const& event : seen)
                            if (event.kind == MemoryEventKind::KilledBy)
                                lastKilledMeAt = std::max(lastKilledMeAt, event.ts);

                        std::array<int16, MEMORY_AXIS_COUNT> axes{};
                        for (std::size_t i = 0; i < MEMORY_AXIS_COUNT; ++i)
                            axes[i] = decay.Apply(row.axes[i], MemoryAxis(i), row.decayedAt, now);
                        MemoryLabel const label = MemoryLabelOf(axes, row.counters, lastKilledMeAt, 0, now);

                        Print(requester, Text("coa_mem_mine_bot", "%bot about %character: %label",
                                              {{"%bot", NameOf(row.bot)},
                                               {"%character", NameOf(row.other)},
                                               {"%label", LabelText(label)}}));

                        std::map<std::string, std::string> axisValues;
                        std::string axesEnglish;
                        for (std::size_t i = 0; i < MEMORY_AXIS_COUNT; ++i)
                        {
                            std::string const name = MemoryAxisName(MemoryAxis(i));
                            axisValues["%" + name] = std::to_string(axes[i] / 10);
                            axesEnglish += (axesEnglish.empty() ? "" : ", ") + name + " %" + name;
                        }
                        Print(requester, Text("coa_mem_mine_axes", axesEnglish, axisValues));

                        auto const count = [&row](MemoryCounter counter)
                        { return std::to_string(row.counters[std::size_t(counter)]); };
                        Print(requester,
                              Text("coa_mem_mine_counters",
                                   "met %meetings times, %chats messages, you helped it %helped_me, it helped you "
                                   "%helped_them, you killed it %killed_me, it killed you %killed_them, duels it won "
                                   "%duels_won, duels you won %duels_lost, groups %groups, trades %trades",
                                   {{"%meetings", count(MemoryCounter::Meetings)},
                                    {"%chats", count(MemoryCounter::Chats)},
                                    {"%helped_me", count(MemoryCounter::HelpedMe)},
                                    {"%helped_them", count(MemoryCounter::HelpedThem)},
                                    {"%killed_me", count(MemoryCounter::KilledMe)},
                                    {"%killed_them", count(MemoryCounter::KilledThem)},
                                    {"%duels_won", count(MemoryCounter::DuelsWon)},
                                    {"%duels_lost", count(MemoryCounter::DuelsLost)},
                                    {"%groups", count(MemoryCounter::GroupsShared)},
                                    {"%trades", count(MemoryCounter::Trades)}}));

                        if (!seen.empty())
                        {
                            std::string list;
                            for (std::size_t i = 0; i < seen.size() && i < MINE_EVENTS_PER_BOT; ++i)
                                list +=
                                    (list.empty() ? "" : ", ") + KindText(seen[i].kind) + " (" + Date(seen[i].ts) + ")";
                            Print(requester, Text("coa_mem_mine_events", "Remembered: %events", {{"%events", list}}));
                        }
                    }
                    if (state->more)
                        Print(requester, Text("coa_mem_mine_more", "More: .playerbots memory mine %next",
                                              {{"%next", std::to_string(page + 1)}}));
                }));
    return {};
}

std::string CoaMemoryAdmin::Disclosure()
{
    COA_ASSERT_WORLD_THREAD();
    if (!CoaMemoryMgr::Instance().Running())
        return "";
    std::shared_ptr<CoaMemoryConfig const> const config = CoaMemoryConfig::Current();
    return Text("coa_mem_disclosure",
                "Bots remember how you treated them: feelings, counts and dated events, never your words. Events are "
                "kept %events days, the rest %relations days. .playerbots memory mine shows it; .playerbots optout "
                "memory erases it.",
                {{"%events", std::to_string(config->eventRetentionDays)},
                 {"%relations", std::to_string(config->relationRetentionDays)}});
}
