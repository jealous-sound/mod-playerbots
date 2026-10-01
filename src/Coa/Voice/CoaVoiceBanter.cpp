/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaVoiceBanter.h"
#include "ActivityRegistry.h"
#include "BrainService.h"
#include "BrainStateBuilder.h"
#include "BrainStats.h"
#include "BrainTextPrompt.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "CoaChannels.h"
#include "CoaPlayerPrefs.h"
#include "CoaThread.h"
#include "CoaVoicePersona.h"
#include "CoaVoiceSanitizer.h"
#include "CoaVoiceTemplates.h"
#include "DBCStores.h"
#include "Map.h"
#include "NameMatch.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Playerbots.h"
#include "QuestionRegistry.h"
#include "Random.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <algorithm>
#include <array>
#include <ctime>
#include <utility>

namespace
{
constexpr uint64 BANTER_SCAN_MS = 5000;
constexpr uint64 MINUTE_MS = 60000;

uint64 ZoneKey(uint32 mapId, uint32 zoneId, uint8 teamId)
{
    return (uint64(mapId) << 40) | (uint64(zoneId) << 8) | teamId;
}

void PruneLines(std::deque<uint64>& lines, uint64 nowMs)
{
    while (!lines.empty() && nowMs >= lines.front() + MINUTE_MS)
        lines.pop_front();
}

bool SpeakerOkay(CoaVoiceIndexedBot const* entry, Player* bot, CoaVoiceScope const& scope,
                  CoaVoiceConfig const& config, CoaVoiceGovernor const& governor, uint64 nowMs)
{
    if (!entry || !entry->randomBot || entry->hasRealMaster || entry->teamId != scope.teamId || !bot ||
        !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() ||
        bot->IsBeingTeleported() || bot->IsDuringRemoveFromWorld() || !bot->GetSession() ||
        bot->GetSession()->isLogingOut() || bot->GetMapId() != scope.mapId || bot->GetZoneId() != scope.zoneId ||
        bot->GetTeamId() != scope.teamId || bot->InBattleground() || !bot->GetMap() || bot->GetMap()->Instanceable() ||
        ActivityRegistry::IsHeld(bot->GetGUID().GetCounter()))
        return false;
    PlayerbotAI* const ai = GET_PLAYERBOT_AI(bot);
    if (!ai || ai->HasGameClientMaster())
        return false;
    ChannelMgr* const manager = ChannelMgr::forTeam(TeamId(scope.teamId));
    Channel* const channel = manager ? manager->GetChannel(scope.channelName, nullptr, false) : nullptr;
    if (!channel || !CoaChannels::IsOn(bot, channel))
        return false;
    uint64 const last = governor.LastAnswerMs(bot->GetGUID());
    return (!last || nowMs >= last + MINUTE_MS) &&
        governor.PreCheck(config, bot->GetGUID(), scope, ObjectGuid::Empty, nowMs) == CoaVoiceDrop::Count;
}

bool Recruiting(CoaVoiceCandidateIndex const& index, uint32 mapId, uint32 zoneId)
{
    auto const registry = ActivityRegistry::Instance().Current();
    for (uint32 candidate : index.InZone(mapId, zoneId))
    {
        CoaVoiceIndexedBot const& bot = index.Bots()[candidate];
        ActivityMemberView const* const member = registry->Find(bot.guid.GetCounter());
        if (member && member->phase == ActivityPhase::Recruiting)
            return true;
    }
    return false;
}

bool LineOkay(std::string const& line, CoaVoiceConfig const& config, ObjectGuid bot,
              CoaVoiceScope const& scope, std::vector<std::string> const& names,
              CoaVoiceGovernor const& governor, uint64 nowMs)
{
    if (line.find('@') != std::string::npos || !CoaChatIntent::IsConversation(line) ||
        !CoaVoiceSanitizer::Validate(line, {config.maxBytes, &config.blockedWords, true, false}))
        return false;
    CoaIntent const intent = CoaChatIntent::Classify(line);
    if (intent == CoaIntent::Lfg || intent == CoaIntent::Trade || intent == CoaIntent::Duel || intent == CoaIntent::Help)
        return false;
    NameMatch::Prepared const words(line);
    for (std::string const& name : names)
        if (std::find(words.Words().begin(), words.Words().end(), name) != words.Words().end())
            return false;
    return !governor.IsRepeat(config, bot, scope.key, line, nowMs) && !sObjectMgr->IsChatFiltered(line);
}

std::string PartOfDay()
{
    std::time_t const now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    uint32 const hour = uint32(local.tm_hour);
    return hour >= 6 && hour < 12 ? "morning" : hour >= 12 && hour < 18 ? "afternoon" :
        hour >= 18 && hour < 23 ? "evening" : "night";
}

CoaBanterTopic ChooseTopic(Player* bot, CoaBanterTopic last)
{
    std::array<uint32, size_t(CoaBanterTopic::Count)> weights{{3, 2, 2, 2, 2, 2, 1}};
    if (bot->GetLevel() <= 10 || bot->GetLevel() >= 55)
        weights[size_t(CoaBanterTopic::Level)] *= 2;
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->GetZoneId()); area &&
        std::string_view(area->area_name[LOCALE_enUS]).find("Barrens") != std::string_view::npos)
        weights[size_t(CoaBanterTopic::Barrens)] *= 3;
    if (last != CoaBanterTopic::Count)
        weights[size_t(last)] = 0;
    uint32 sum = 0;
    for (uint32 weight : weights)
        sum += weight;
    uint32 roll = urand(1, sum);
    for (std::size_t topic = 0; topic < weights.size(); ++topic)
    {
        if (roll <= weights[topic])
            return CoaBanterTopic(topic);
        roll -= weights[topic];
    }
    return CoaBanterTopic::Zone;
}
}  // namespace

char const* CoaBanterTopicName(CoaBanterTopic topic)
{
    static constexpr char const* NAMES[] = {"zone", "weather", "level", "class", "barrens", "quest", "dungeon"};
    return size_t(topic) < size_t(CoaBanterTopic::Count) ? NAMES[size_t(topic)] : "zone";
}

char const* CoaBanterTextName(CoaBanterTopic topic)
{
    static constexpr char const* NAMES[] = {"coa_banter_zone", "coa_banter_weather", "coa_banter_level",
        "coa_banter_class", "coa_banter_barrens", "coa_banter_quest", "coa_banter_dungeon"};
    return size_t(topic) < size_t(CoaBanterTopic::Count) ? NAMES[size_t(topic)] : "coa_banter_zone";
}

void CoaVoiceBanter::Update(CoaVoiceConfig const& config, ICoaVoiceBanterHost& host, uint64 nowMs)
{
    COA_ASSERT_WORLD_THREAD();
    if (!config.banterEnable)
    {
        Reset();
        return;
    }
    active = true;
    BrainStats& brainStats = BrainStats::Instance();
    auto const brain = BrainService::Current();
    if (sink)
        sink->Drain(results);
    for (BrainResult& result : results)
    {
        if (result.epoch != epoch)
            continue;
        auto const exchange = std::find_if(exchanges.begin(), exchanges.end(), [&result](Exchange const& value)
        {
            return value.id == result.tag && (value.textJobId == result.jobId || value.emoteJobId == result.jobId);
        });
        if (exchange == exchanges.end())
            continue;
        if (result.jobId == exchange->textJobId)
        {
            exchange->textDone = true;
            if (result.error == BrainError::None)
                exchange->text = std::move(result.text);
        }
        else
        {
            exchange->emoteDone = true;
            BrainAnswer const* const answer = result.Find("emote");
            auto const& keys = QuestionRegistry::EmoteKeys();
            if (result.error == BrainError::None && answer && answer->valid &&
                std::find(keys.begin(), keys.end(), answer->choice) != keys.end())
            {
                exchange->emote = int32(BrainStateBuilder::EmoteFor(answer->choice));
                brainStats.banterEmotes.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    results.clear();

    for (auto it = exchanges.begin(); it != exchanges.end();)
    {
        Exchange& exchange = *it;
        if ((!exchange.textDone || !exchange.emoteDone) && nowMs < exchange.deadlineMs)
        {
            ++it;
            continue;
        }
        if (!exchange.textDone && exchange.textJobId)
            brain->Cancel(exchange.textJobId);
        if (!exchange.emoteDone && exchange.emoteJobId)
            brain->Cancel(exchange.emoteJobId);
        auto const found = zones.find(exchange.zoneKey);
        bool queued = false;
        if (found != zones.end())
        {
            Zone& zone = found->second;
            PruneLines(zone.linesMs, nowMs);
            CoaVoiceScope const scope = host.ChannelScope(zone.teamId, zone.channelName, zone.mapId, zone.zoneId);
            Player* const bot = ObjectAccessor::FindPlayer(exchange.bot);
            CoaVoiceGovernor& governor = host.Governor();
            if (nowMs <= zone.lastSeenMs + 15000 &&
                (!zone.lastHumanSpokeMs || nowMs >= zone.lastHumanSpokeMs + config.banterQuietAfterHumanMs) &&
                zone.linesMs.size() + 2 <= config.banterMaxPerMinutePerZone &&
                !Recruiting(host.Index(), zone.mapId, zone.zoneId) &&
                SpeakerOkay(host.Index().Find(exchange.bot), bot, scope, config, governor, nowMs))
            {
                bool generated = !exchange.text.empty() &&
                    LineOkay(exchange.text, config, exchange.bot, scope, zone.humanNames, governor, nowMs);
                std::string const& text = generated ? exchange.text : exchange.templateText;
                if (LineOkay(text, config, exchange.bot, scope, zone.humanNames, governor, nowMs))
                {
                    CoaVoicePersona const persona = CoaVoicePersona::For(exchange.bot);
                    uint32 emote = exchange.emote >= 0 ? uint32(exchange.emote) : 0;
                    if (!config.replyEmotes)
                        emote = 0;
                    else if (exchange.emote < 0 && rand_chance() < persona.emoteChance * 100.0f)
                        emote = persona.favouriteEmotes[urand(0, uint32(persona.favouriteEmotes.size() - 1))];
                    CoaVoiceLine line;
                    line.dueMs = nowMs + urand(1000, 4000);
                    line.eventId = host.NextEventId();
                    line.scope = scope;
                    line.bot = exchange.bot;
                    line.text = text;
                    line.emote = emote;
                    line.depth = 1;
                    line.banter = true;
                    line.generated = generated;
                    line.answerPercent = uint8(config.botToBot ? config.banterAnswerPercent : 0);
                    // Scheduling does not send: commit the governor only after the queue accepted the opener.
                    queued = host.Schedule(std::move(line));
                    if (queued)
                    {
                        governor.Reserve(config, exchange.bot, scope, ObjectGuid::Empty, text, nowMs);
                        governor.NoteHumanSpoke(scope.key, nowMs);
                        zone.lastTopic = exchange.topic;
                        brainStats.banterExchanges.fetch_add(1, std::memory_order_relaxed);
                        if (generated)
                            brainStats.banterText.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            zone.busy = false;
            zone.nextExchangeMs = nowMs + std::max<uint32>(config.banterMinGapMs, 5000);
        }
        if (!queued)
            brainStats.banterSkipped.fetch_add(1, std::memory_order_relaxed);
        it = exchanges.erase(it);
    }

    if (nowMs < nextScanMs)
        return;
    nextScanMs = nowMs + BANTER_SCAN_MS;
    ++stats.scans;
    if (!CoaPlayerPrefs::Instance().IsLoaded())
        return;
    for (auto& [key, zone] : zones)
        zone.humanNames.clear();
    for (auto const& [account, session] : sWorldSessionMgr->GetAllSessions())
    {
        Player* const player = session ? session->GetPlayer() : nullptr;
        if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld() || GET_PLAYERBOT_AI(player) ||
            player->isAFK() || player->InBattleground() || !player->GetMap() || player->GetMap()->Instanceable() ||
            CoaPlayerPrefs::Instance().VoiceOptedOut(account))
            continue;
        for (Channel* channel : CoaChannels::Joined(player))
        {
            if (!channel || !config.ListensTo(channel->GetName()))
                continue;
            uint64 const key = ZoneKey(player->GetMapId(), player->GetZoneId(), player->GetTeamId());
            Zone& zone = zones[key];
            zone.mapId = player->GetMapId();
            zone.zoneId = player->GetZoneId();
            zone.teamId = player->GetTeamId();
            zone.channelName = channel->GetName();
            zone.scopeKey = host.ChannelScope(zone.teamId, zone.channelName, zone.mapId, zone.zoneId).key;
            zone.lastSeenMs = nowMs;
            if (auto const spoke = humanSpoke.find(zone.scopeKey); spoke != humanSpoke.end())
                zone.lastHumanSpokeMs = spoke->second;
            std::string_view const name(player->GetName());
            zone.humanNames.push_back(NameMatch::Fold(name.substr(0, name.find(' '))));
            break;
        }
    }
    std::erase_if(zones, [nowMs](auto const& entry)
    {
        return !entry.second.busy && nowMs > entry.second.lastSeenMs + 30 * MINUTE_MS;
    });
    std::erase_if(humanSpoke, [nowMs](auto const& entry) { return nowMs > entry.second + 30 * MINUTE_MS; });

    for (auto& [key, zone] : zones)
    {
        if (exchanges.size() >= config.banterMaxZones)
            break;
        PruneLines(zone.linesMs, nowMs);
        if (zone.lastSeenMs != nowMs || zone.busy || nowMs < zone.nextExchangeMs ||
            (zone.lastHumanSpokeMs && nowMs < zone.lastHumanSpokeMs + config.banterQuietAfterHumanMs) ||
            zone.linesMs.size() + 2 > config.banterMaxPerMinutePerZone || Recruiting(host.Index(), zone.mapId, zone.zoneId))
            continue;
        if (urand(0, std::max<uint32>(1, config.banterIntervalMs / uint32(BANTER_SCAN_MS)) - 1))
            continue;
        CoaVoiceScope const scope = host.ChannelScope(zone.teamId, zone.channelName, zone.mapId, zone.zoneId);
        std::vector<std::pair<ObjectGuid, uint32>> speakers;
        uint32 weightSum = 0;
        for (uint32 candidate : host.Index().InZone(zone.mapId, zone.zoneId))
        {
            CoaVoiceIndexedBot const& entry = host.Index().Bots()[candidate];
            Player* const bot = ObjectAccessor::FindPlayer(entry.guid);
            if (!SpeakerOkay(&entry, bot, scope, config, host.Governor(), nowMs))
                continue;
            uint32 const weight = std::max<uint32>(1, uint32(CoaVoicePersona::For(entry.guid).chattiness * 1000.0f));
            speakers.emplace_back(entry.guid, weight);
            weightSum += weight;
        }
        if (speakers.empty())
            continue;
        uint32 roll = urand(1, weightSum);
        ObjectGuid guid;
        for (auto const& [candidate, weight] : speakers)
        {
            if (roll <= weight)
            {
                guid = candidate;
                break;
            }
            roll -= weight;
        }
        Player* const bot = ObjectAccessor::FindPlayer(guid);
        if (!bot)
            continue;
        Exchange exchange;
        exchange.id = host.NextEventId();
        exchange.zoneKey = key;
        exchange.bot = guid;
        exchange.topic = ChooseTopic(bot, zone.lastTopic);
        exchange.startedMs = nowMs;
        exchange.deadlineMs = nowMs + std::max<uint32>(config.textTimeoutMs, 2000);
        exchange.textDone = true;
        exchange.emoteDone = true;
        BrainTextPrompt::BotSheet const sheet = BrainTextPrompt::SheetOf(bot);
        CoaVoiceTemplates::Placeholders placeholders;
        placeholders.Set(CoaVoicePlaceholder::ZONE,
            PlayerbotAI::GetLocalizedAreaName(sAreaTableStore.LookupEntry(zone.zoneId)))
            .Set(CoaVoicePlaceholder::CLASS, sheet.className)
            .Set(CoaVoicePlaceholder::SPEC, sheet.spec.empty() ? sheet.className : sheet.spec)
            .Set(CoaVoicePlaceholder::LEVEL, std::to_string(bot->GetLevel()));
        CoaVoicePersona const persona = CoaVoicePersona::For(guid);
        for (std::string const& candidate : CoaVoiceTemplates::Candidates(
            CoaBanterTextName(exchange.topic), host.Locale(), config.templateCandidates))
        {
            std::string line = persona.ApplyQuirks(placeholders.Fill(candidate), CoaIntent::None);
            if (CoaVoiceSanitizer::Clean(line, {config.maxBytes, false, &config.blockedWords}) &&
                LineOkay(line, config, guid, scope, zone.humanNames, host.Governor(), nowMs))
            {
                exchange.templateText = std::move(line);
                break;
            }
        }
        if (exchange.templateText.empty())
        {
            brainStats.banterSkipped.fetch_add(1, std::memory_order_relaxed);
            zone.nextExchangeMs = nowMs + std::max<uint32>(config.banterMinGapMs, 5000);
            continue;
        }
        if (config.textLlm >= 2 && BrainService::TextActive() && rand_chance() < config.banterTextPercent)
        {
            BrainTextPrompt::BanterTextFacts facts;
            facts.bot = sheet;
            facts.topic = exchange.topic;
            facts.partOfDay = PartOfDay();
            facts.recent.assign(zone.recent.begin(), zone.recent.end());
            facts.locale = host.Locale();
            BrainJob job;
            if (BrainTextPrompt::BuildBanterTextJob(facts, job))
            {
                job.tag = exchange.id;
                job.epoch = epoch;
                job.botGuid = guid.GetRawValue();
                job.deadlineMs = nowMs + config.textTimeoutMs;
                job.textFinisher = BrainTextPrompt::MakeFinisher(config.maxBytes, config.blockedWords, false);
                if (!sink)
                    sink = std::make_shared<BrainSink>(1024);
                BrainTicket const ticket = brain->Submit(std::move(job), sink, nullptr);
                if (ticket.status == BrainSubmit::Queued)
                {
                    exchange.textJobId = ticket.jobId;
                    exchange.textDone = false;
                }
            }
        }
        if (BrainConfig::Current()->useEmote && brain->Active(BrainUse::Emote))
        {
            auto const questions = QuestionRegistry::Current();
            BrainJob job;
            job.use = BrainUse::Emote;
            job.lane = BrainLane::Ambient;
            job.situation = "banter_emote";
            job.tag = exchange.id;
            job.epoch = epoch;
            job.botGuid = guid.GetRawValue();
            job.deadlineMs = nowMs + 2000;
            job.state = questions->header +
                "\n\n[situation]\nplace: a public zone channel; the adventurer is about to say a short remark\ntopic: " +
                CoaBanterTopicName(exchange.topic) + "\n\n[bots]\nb0: level " + std::to_string(sheet.level) + " " +
                sheet.className + ", chattiness " + std::to_string(sheet.chattiness);
            BrainQuestion question;
            question.name = "emote";
            question.type = BrainQuestionType::Choice;
            question.instructions = questions->emoteInstructions;
            for (std::string const& emote : QuestionRegistry::EmoteKeys())
            {
                auto const description = std::find_if(questions->emotes.begin(), questions->emotes.end(),
                    [&emote](auto const& value) { return value.first == emote; });
                question.options.push_back({emote, description == questions->emotes.end() ? emote : description->second, ""});
            }
            job.questions.push_back(std::move(question));
            if (!sink)
                sink = std::make_shared<BrainSink>(1024);
            BrainTicket const ticket = brain->Submit(std::move(job), sink, nullptr);
            if (ticket.status == BrainSubmit::Queued)
            {
                exchange.emoteJobId = ticket.jobId;
                exchange.emoteDone = false;
            }
        }
        zone.busy = true;
        exchanges.push_back(std::move(exchange));
    }
}

void CoaVoiceBanter::OnChannelLine(CoaVoiceLine const& line, uint64 nowMs)
{
    COA_ASSERT_WORLD_THREAD();
    if (!active || line.scope.kind != CoaVoiceScopeKind::Channel)
        return;
    uint64 const key = ZoneKey(line.scope.mapId, line.scope.zoneId, line.scope.teamId);
    Zone& zone = zones[key];
    zone.mapId = line.scope.mapId;
    zone.zoneId = line.scope.zoneId;
    zone.teamId = line.scope.teamId;
    zone.channelName = line.scope.channelName;
    zone.scopeKey = line.scope.key;
    PruneLines(zone.linesMs, nowMs);
    zone.linesMs.push_back(nowMs);
    if (line.banter)
    {
        // Reply templates can name the opener bot. Only anonymous openers enter the hosted prompt history.
        if (line.depth == 1)
        {
            zone.recent.push_back(line.text);
            while (zone.recent.size() > 3)
                zone.recent.pop_front();
            BrainStats::Instance().banterOpeners.fetch_add(1, std::memory_order_relaxed);
        }
        else if (line.depth == 2)
            BrainStats::Instance().banterAnswers.fetch_add(1, std::memory_order_relaxed);
    }
}

void CoaVoiceBanter::OnHumanSpoke(uint64 scopeKey, uint64 nowMs)
{
    COA_ASSERT_WORLD_THREAD();
    humanSpoke[scopeKey] = nowMs;
    for (auto& [key, zone] : zones)
        if (zone.scopeKey == scopeKey)
            zone.lastHumanSpokeMs = nowMs;
}

void CoaVoiceBanter::Reset()
{
    COA_ASSERT_WORLD_THREAD();
    if (!active && zones.empty() && exchanges.empty() && humanSpoke.empty() && !sink)
        return;
    auto const brain = BrainService::Current();
    for (Exchange const& exchange : exchanges)
    {
        if (exchange.textJobId)
            brain->Cancel(exchange.textJobId);
        if (exchange.emoteJobId)
            brain->Cancel(exchange.emoteJobId);
    }
    if (sink)
        sink->Close();
    sink.reset();
    zones.clear();
    exchanges.clear();
    humanSpoke.clear();
    results.clear();
    ++epoch;
    nextScanMs = 0;
    active = false;
}

std::string CoaVoiceBanter::FormatStats() const
{
    auto const& counters = BrainStats::Instance();
    auto const read = [](std::atomic<uint64> const& counter) { return counter.load(std::memory_order_relaxed); };
    return Acore::StringFormat("zones={} exchanges={} openers={} answers={} text={} skipped={} emotes={}",
        zones.size(), read(counters.banterExchanges), read(counters.banterOpeners), read(counters.banterAnswers),
        read(counters.banterText), read(counters.banterSkipped), read(counters.banterEmotes));
}

CoaVoiceDrop CoaVoiceBanter::SendGate(CoaVoiceConfig const& config, CoaVoiceLine const& line, uint64 nowMs)
{
    COA_ASSERT_WORLD_THREAD();
    if (!active || !CoaPlayerPrefs::Instance().IsLoaded())
        return CoaVoiceDrop::NoAudience;
    auto const found = zones.find(ZoneKey(line.scope.mapId, line.scope.zoneId, line.scope.teamId));
    if (found == zones.end())
        return CoaVoiceDrop::NoAudience;
    Zone& zone = found->second;
    PruneLines(zone.linesMs, nowMs);
    if (zone.linesMs.size() >= config.banterMaxPerMinutePerZone)
        return CoaVoiceDrop::ScopeRate;
    if (zone.lastHumanSpokeMs && nowMs < zone.lastHumanSpokeMs + config.banterQuietAfterHumanMs)
        return CoaVoiceDrop::NoAudience;
    ChannelMgr* const manager = ChannelMgr::forTeam(TeamId(line.scope.teamId));
    Channel* const channel = manager ? manager->GetChannel(line.scope.channelName, nullptr, false) : nullptr;
    if (!channel)
        return CoaVoiceDrop::BotGone;
    Player* const bot = ObjectAccessor::FindPlayer(line.bot);
    if (!bot || !bot->IsAlive() || bot->GetMapId() != line.scope.mapId || bot->GetZoneId() != line.scope.zoneId ||
        bot->IsInFlight() || ActivityRegistry::IsHeld(line.bot.GetCounter()))
        return CoaVoiceDrop::BotGone;
    bool audience = false;
    NameMatch::Prepared const words(line.text);
    for (auto const& [account, session] : sWorldSessionMgr->GetAllSessions())
    {
        Player* const player = session ? session->GetPlayer() : nullptr;
        if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld() || GET_PLAYERBOT_AI(player) ||
            player->isAFK() || player->InBattleground() || player->GetMapId() != line.scope.mapId ||
            player->GetZoneId() != line.scope.zoneId || player->GetTeamId() != line.scope.teamId ||
            !player->GetMap() || player->GetMap()->Instanceable() ||
            CoaPlayerPrefs::Instance().VoiceOptedOut(account) || !CoaChannels::IsOn(player, channel))
            continue;
        audience = true;
        std::string_view const name(player->GetName());
        std::string const folded = NameMatch::Fold(name.substr(0, name.find(' ')));
        if (std::find(words.Words().begin(), words.Words().end(), folded) != words.Words().end())
            return CoaVoiceDrop::Addressed;
    }
    if (!audience)
        return CoaVoiceDrop::NoAudience;
    if (line.text.find('@') != std::string::npos || !CoaChatIntent::IsConversation(line.text))
        return CoaVoiceDrop::NotConversation;
    CoaIntent const intent = CoaChatIntent::Classify(line.text);
    if (intent == CoaIntent::Lfg || intent == CoaIntent::Trade || intent == CoaIntent::Duel || intent == CoaIntent::Help)
        return CoaVoiceDrop::NotConversation;
    return CoaVoiceDrop::Count;
}
