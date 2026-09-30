/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaVoiceDirector.h"
#include "BrainDecisionRing.h"
#include "BrainService.h"
#include "BrainStats.h"
#include "CharacterCache.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "CoaHash.h"
#include "CoaMemoryConfig.h"
#include "CoaMemoryMgr.h"
#include "CoaPlayerPrefs.h"
#include "CoaSpecLookup.h"
#include "CoaSpecialization.h"
#include "CoaThread.h"
#include "CoaTime.h"
#include "CoaVoicePersona.h"
#include "CoaVoiceSanitizer.h"
#include "CoaVoiceTemplates.h"
#include "DBCStores.h"
#include "EmoteAction.h"
#include "Group.h"
#include "Log.h"
#include "MemoryTypes.h"
#include "NameMatch.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Playerbots.h"
#include "QuestionRegistry.h"
#include "Random.h"
#include "SentimentHeuristic.h"
#include "ServerFacade.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>
#include <optional>
#include <utility>

namespace
{
constexpr uint64 MINUTE_MS = 60 * 1000;
constexpr uint64 PRUNE_PERIOD_MS = MINUTE_MS;
constexpr uint64 DENY_MEMORY_MS = 10 * MINUTE_MS;
constexpr uint32 DENY_DELAY_MIN_MS = 1000;
constexpr uint32 DENY_DELAY_MAX_MS = 2500;
constexpr std::size_t MAX_DECISIONS = 256;  // human messages waiting for the brain at once

thread_local uint8 EmitDepth = 0;

CoaVoiceScopeKind KindOf(CoaEvent const& event)
{
    if (event.kind == CoaEventKind::TextEmote)
        return CoaVoiceScopeKind::TextEmote;
    if (event.kind != CoaEventKind::Chat)
        return CoaVoiceScopeKind::Count;

    switch (event.chatType)
    {
        case CHAT_MSG_WHISPER:
            return CoaVoiceScopeKind::Whisper;
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
            return CoaVoiceScopeKind::Party;
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_LEADER:
        case CHAT_MSG_RAID_WARNING:
            return CoaVoiceScopeKind::Raid;
        case CHAT_MSG_GUILD:
            return CoaVoiceScopeKind::Guild;
        case CHAT_MSG_SAY:
            return CoaVoiceScopeKind::Say;
        case CHAT_MSG_YELL:
            return CoaVoiceScopeKind::Yell;
        case CHAT_MSG_EMOTE:
            return CoaVoiceScopeKind::CustomEmote;
        case CHAT_MSG_CHANNEL:
            return CoaVoiceScopeKind::Channel;
        default:
            return CoaVoiceScopeKind::Count;
    }
}

// The chat type a bot's own line is echoed with (CoaVoice.BotToBot), so the echo resolves to the same scope.
uint8 EchoChatType(CoaVoiceScopeKind kind)
{
    switch (kind)
    {
        case CoaVoiceScopeKind::Party:
            return CHAT_MSG_PARTY;
        case CoaVoiceScopeKind::Raid:
            return CHAT_MSG_RAID;
        case CoaVoiceScopeKind::Guild:
            return CHAT_MSG_GUILD;
        default:
            return CHAT_MSG_CHANNEL;
    }
}

bool IsLocal(CoaVoiceScopeKind kind)
{
    return kind == CoaVoiceScopeKind::Say || kind == CoaVoiceScopeKind::Yell ||
           kind == CoaVoiceScopeKind::CustomEmote || kind == CoaVoiceScopeKind::TextEmote;
}

bool IsEmoteScope(CoaVoiceScopeKind kind)
{
    return kind == CoaVoiceScopeKind::CustomEmote || kind == CoaVoiceScopeKind::TextEmote;
}

// The kinds in which a named bot always answers (ReplyChance forces 100): the brain may not silence them.
bool IsForcedKind(CoaVoiceScopeKind kind)
{
    return kind == CoaVoiceScopeKind::Party || kind == CoaVoiceScopeKind::Raid || kind == CoaVoiceScopeKind::Guild ||
           kind == CoaVoiceScopeKind::Say || kind == CoaVoiceScopeKind::Yell || kind == CoaVoiceScopeKind::Channel;
}

float ListenRange(CoaVoiceScopeKind kind)
{
    switch (kind)
    {
        case CoaVoiceScopeKind::Yell:
            return sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_YELL);
        case CoaVoiceScopeKind::CustomEmote:
        case CoaVoiceScopeKind::TextEmote:
            return sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE);
        default:
            return sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY);
    }
}

// "Kegarink Bot" -> "Kegarink": random bots carry a surname, and nobody calls them by it.
std::string_view FirstName(std::string const& name)
{
    std::string_view const view(name);
    return view.substr(0, view.find(' '));
}

std::string NameKeyOf(CoaVoiceCandidateIndex const& index, Player* bot)
{
    if (CoaVoiceIndexedBot const* entry = index.Find(bot->GetGUID()); entry && entry->name == bot->GetName())
        return entry->nameKey;
    return NameMatch::Fold(FirstName(bot->GetName()));
}

// Whether HandleCommand lets `sender` command `bot` (PlayerbotSecurity ALLOW_ALL). Only a GM or the bot's
// master can have it, so the full check runs for those alone.
bool Obeys(Player* bot, Player* sender)
{
    PlayerbotAI* const botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || (!sender->CanBeGameMaster() && botAI->GetMaster() != sender))
        return false;
    return botAI->GetSecurity()->LevelFor(sender) >= PLAYERBOT_SECURITY_ALLOW_ALL;
}

// Whether the real player a line answers has opted out of voice. `target` is null when that player is not in
// the world any more: the character cache gives the account (a public line would still name them).
bool TargetOptedOut(ObjectGuid guid, Player* target)
{
    if (target)
        return !GET_PLAYERBOT_AI(target) && target->GetSession() &&
               CoaPlayerPrefs::Instance().VoiceOptedOut(target->GetSession()->GetAccountId());

    uint32 const account = sCharacterCache->GetCharacterAccountIdByGuid(guid);
    return account && CoaPlayerPrefs::Instance().VoiceOptedOut(account) &&
           !sPlayerbotAIConfig.IsInRandomAccountList(account);
}

// The intent a text emote stands for, for the persona's quirks and the logs.
CoaIntent EmoteIntent(CoaEmoteCategory category)
{
    switch (category)
    {
        case CoaEmoteCategory::Greet:
            return CoaIntent::Greeting;
        case CoaEmoteCategory::Bye:
            return CoaIntent::Farewell;
        case CoaEmoteCategory::Thank:
            return CoaIntent::Thanks;
        case CoaEmoteCategory::Praise:
        case CoaEmoteCategory::Cheer:
            return CoaIntent::Compliment;
        case CoaEmoteCategory::Laugh:
            return CoaIntent::Joke;
        case CoaEmoteCategory::Rude:
        case CoaEmoteCategory::Hurt:
            return CoaIntent::Insult;
        default:
            return CoaIntent::Other;
    }
}

// M3: where a message addressed to a bot was said, as the bots' memory files it.
MemoryTalkKind TalkKindOf(CoaVoiceScopeKind kind)
{
    switch (kind)
    {
        case CoaVoiceScopeKind::Whisper:
            return MemoryTalkKind::Whisper;
        case CoaVoiceScopeKind::Party:
        case CoaVoiceScopeKind::Raid:
            return MemoryTalkKind::Group;
        case CoaVoiceScopeKind::Guild:
            return MemoryTalkKind::Guild;
        case CoaVoiceScopeKind::Channel:
            return MemoryTalkKind::Channel;
        case CoaVoiceScopeKind::TextEmote:
            return MemoryTalkKind::Emote;
        default:
            return MemoryTalkKind::Say;  // say, yell, "/e"
    }
}

// M3 (CoaMemory.MemoryReplies): how `bot` sounds toward `sender`. None after one relaxed load when off.
MemoryTone ToneToward(Player* bot, Player* sender)
{
    if (!CoaMemoryConfig::BehaviourActive(MemoryBehaviour::MemoryReplies))
        return MemoryTone::None;
    return CoaMemoryMgr::Instance().ToneFor(bot, sender);
}

// The coa_mem_reply_* name of a tone for the intent answered; null keeps the M1 name.
char const* MemoryReplyName(MemoryTone tone, CoaIntent intent)
{
    switch (tone)
    {
        case MemoryTone::Grudge:
            return intent == CoaIntent::Farewell ? "coa_mem_reply_cold" : "coa_mem_reply_grudge";
        case MemoryTone::Cold:
            return "coa_mem_reply_cold";
        case MemoryTone::Friend:
            switch (intent)
            {
                case CoaIntent::Greeting:
                case CoaIntent::Thanks:
                case CoaIntent::Compliment:
                case CoaIntent::Joke:
                case CoaIntent::Other:
                case CoaIntent::None:
                    return "coa_mem_reply_friend";
                default:
                    return nullptr;
            }
        default:
            return nullptr;
    }
}

char const* ToneLogName(MemoryTone tone)
{
    switch (tone)
    {
        case MemoryTone::Friend:
            return "friend";
        case MemoryTone::Cold:
            return "cold";
        case MemoryTone::Grudge:
            return "grudge";
        default:
            return "none";
    }
}

// Runs `fn` when it goes out of scope.
template <typename F>
class ScopeExit
{
public:
    explicit ScopeExit(F fn) : onExit(std::move(fn)) {}
    ~ScopeExit() { onExit(); }
    ScopeExit(ScopeExit const&) = delete;
    ScopeExit& operator=(ScopeExit const&) = delete;

private:
    F onExit;
};

std::string ClassName(Player* bot)
{
    if (char const* coa = CoaClassName(bot->getClass()))
        return coa;

    switch (bot->getClass())
    {
        case CLASS_WARRIOR:
            return "Warrior";
        case CLASS_PALADIN:
            return "Paladin";
        case CLASS_HUNTER:
            return "Hunter";
        case CLASS_ROGUE:
            return "Rogue";
        case CLASS_PRIEST:
            return "Priest";
        case CLASS_DEATH_KNIGHT:
            return "Death Knight";
        case CLASS_SHAMAN:
            return "Shaman";
        case CLASS_MAGE:
            return "Mage";
        case CLASS_WARLOCK:
            return "Warlock";
        case CLASS_DRUID:
            return "Druid";
        default:
            return "";
    }
}

CoaVoiceTemplates::Placeholders PlaceholdersFor(Player* bot, Player* sender)
{
    using namespace CoaVoicePlaceholder;
    std::string const className = ClassName(bot);
    CoaSpecStrategy const* const spec = GetCoaSpecStrategyFor(bot);

    CoaVoiceTemplates::Placeholders placeholders;
    placeholders.Set(PLAYER, std::string(FirstName(sender->GetName())))
        .Set(BOT, std::string(FirstName(bot->GetName())))
        .Set(CLASS, className)
        .Set(SPEC, spec && spec->specName ? std::string(spec->specName) : className)
        .Set(LEVEL, std::to_string(bot->GetLevel()))
        .Set(THEIR_LEVEL, std::to_string(sender->GetLevel()))
        .Set(ZONE, PlayerbotAI::GetLocalizedAreaName(sAreaTableStore.LookupEntry(bot->GetZoneId())));
    return placeholders;
}

// English refusals for a realm whose coa_texts rows are missing: the voice kept the legacy refusal quiet, so
// the player must still hear one.
char const* DenyDefault(std::string_view name)
{
    if (name == "coa_deny_taken")
        return "I have a master already";
    if (name == "coa_deny_level")
        return "You are too low level (%theirlevel/%level)";
    if (name == "coa_deny_queue")
        return "I am in a queue. Will do it later";
    if (name == "coa_deny_full")
        return "I am in a full group. Will do it later";
    if (name == "coa_deny_not_leader")
        return "I am in a group with %leader. You can ask them for an invite.";
    if (name == "coa_deny_invite")
        return "Invite me to your group first";
    return "I'm kind of busy now";
}

void SamplePercentiles(std::vector<uint32>& samples, uint32& p50, uint32& p95)
{
    p50 = p95 = 0;
    if (samples.empty())
        return;

    std::size_t const mid = samples.size() / 2;
    std::size_t const high = std::min(samples.size() - 1, samples.size() * 95 / 100);
    std::nth_element(samples.begin(), samples.begin() + mid, samples.end());
    p50 = samples[mid];
    std::nth_element(samples.begin(), samples.begin() + high, samples.end());
    p95 = samples[high];
}

// Plays a text emote at `face` the way EmoteAction does; nothing when it is not close enough to see it.
void PlayEmote(Player* bot, Player* face, uint32 emote)
{
    if (!face || face == bot || !face->IsInWorld() || face->GetMapId() != bot->GetMapId() ||
        face->GetInstanceId() != bot->GetInstanceId() ||
        bot->GetExactDist(face) > sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE))
        return;

    if (!bot->isMoving() && !bot->HasInArc(static_cast<float>(M_PI), face))
        ServerFacade::instance().SetFacingTo(bot, face);

    WorldPacket data(SMSG_TEXT_EMOTE);
    data << uint32(emote);
    data << EmoteActionBase::GetNumberOfEmoteVariants(TextEmotes(emote), bot->getRace(), bot->getGender());
    data << face->GetGUID();
    bot->GetSession()->HandleTextEmoteOpcode(data);
}
}  // namespace

char const* CoaVoiceScopeName(CoaVoiceScopeKind kind)
{
    switch (kind)
    {
        case CoaVoiceScopeKind::Whisper:
            return "whisper";
        case CoaVoiceScopeKind::Party:
            return "party";
        case CoaVoiceScopeKind::Raid:
            return "raid";
        case CoaVoiceScopeKind::Guild:
            return "guild";
        case CoaVoiceScopeKind::Say:
            return "say";
        case CoaVoiceScopeKind::Yell:
            return "yell";
        case CoaVoiceScopeKind::CustomEmote:
            return "custom-emote";
        case CoaVoiceScopeKind::TextEmote:
            return "text-emote";
        case CoaVoiceScopeKind::Channel:
            return "channel";
        default:
            return "unknown";
    }
}

char const* CoaVoiceDropName(CoaVoiceDrop drop)
{
    switch (drop)
    {
        case CoaVoiceDrop::Disabled:
            return "disabled";
        case CoaVoiceDrop::PrefsNotLoaded:
            return "prefs-not-loaded";
        case CoaVoiceDrop::SenderGone:
            return "sender-gone";
        case CoaVoiceDrop::OptOut:
            return "opt-out";
        case CoaVoiceDrop::NotConversation:
            return "not-conversation";
        case CoaVoiceDrop::Addressed:
            return "addressed";
        case CoaVoiceDrop::NoCandidate:
            return "no-candidate";
        case CoaVoiceDrop::Chance:
            return "chance";
        case CoaVoiceDrop::Chain:
            return "chain";
        case CoaVoiceDrop::NoAudience:
            return "no-audience";
        case CoaVoiceDrop::GlobalRate:
            return "global-rate";
        case CoaVoiceDrop::BotCooldown:
            return "bot-cooldown";
        case CoaVoiceDrop::ScopeCooldown:
            return "scope-cooldown";
        case CoaVoiceDrop::ScopeRate:
            return "scope-rate";
        case CoaVoiceDrop::SenderCooldown:
            return "sender-cooldown";
        case CoaVoiceDrop::NoLine:
            return "no-line";
        case CoaVoiceDrop::Repetition:
            return "repetition";
        case CoaVoiceDrop::PendingFull:
            return "pending-full";
        case CoaVoiceDrop::BotGone:
            return "bot-gone";
        case CoaVoiceDrop::TargetGone:
            return "target-gone";
        case CoaVoiceDrop::Combat:
            return "combat";
        case CoaVoiceDrop::Filtered:
            return "filtered";
        case CoaVoiceDrop::SendFailed:
            return "send-failed";
        case CoaVoiceDrop::BrainSilent:
            return "brain-silent";
        default:
            return "unknown";
    }
}

namespace CoaVoiceEmit
{
uint8 CurrentDepth() { return EmitDepth; }

ScopedDepth::ScopedDepth(uint8 depth) : previous(EmitDepth) { EmitDepth = depth; }

ScopedDepth::~ScopedDepth() { EmitDepth = previous; }
}  // namespace CoaVoiceEmit

void CoaVoiceDirector::CostRing::Add(uint32 us)
{
    samples[next] = us;
    next = (next + 1) % samples.size();
    count = std::min<uint32>(count + 1, uint32(samples.size()));
}

void CoaVoiceDirector::CostRing::Percentiles(uint32& p50, uint32& p95) const
{
    std::vector<uint32> copy(samples.begin(), samples.begin() + count);
    SamplePercentiles(copy, p50, p95);
}

CoaVoiceDirector& CoaVoiceDirector::Instance()
{
    static CoaVoiceDirector instance;
    return instance;
}

void CoaVoiceDirector::Update(uint32 /*diff*/, std::vector<CoaEvent>& events)
{
    COA_ASSERT_WORLD_THREAD();

    // Voice off: one relaxed load per tick and nothing else, unless events were still queued when it went off.
    if (closed || !CoaVoiceConfig::Enabled())
    {
        if (!closed)
        {
            for (CoaEvent const& event : events)
            {
                if (CoaVoiceScopeKind const kind = KindOf(event); kind != CoaVoiceScopeKind::Count)
                    ++stats.events[size_t(kind)];
                CountDrop(CoaVoiceDrop::Disabled);
            }
        }
        events.clear();
        if (active)
            Reset();
        return;
    }

    config = CoaVoiceConfig::Current();
    CoaVoiceConfig const& cfg = *config;
    active = true;

    uint64 const now = CoaTime::NowMs();
    tickNowMs = now;
    if (index.IsStale(now, cfg.indexRefreshMs))
    {
        index.Rebuild(now);
        botFirstNames.clear();
        for (CoaVoiceIndexedBot const& bot : index.Bots())
            botFirstNames.insert(bot.nameKey);

        // Real players are called by their first name too ("Jean" for "Jean Dupont"). Bots' sessions are not
        // in this list.
        humanFirstNames.clear();
        for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
        {
            Player* const player = session ? session->GetPlayer() : nullptr;
            if (player && player->IsInWorld())
                ++humanFirstNames[NameMatch::Fold(FirstName(player->GetName()))];
        }
    }

    locale = PlayerbotTextMgr::instance().GetLocalePriority();

    // Brain off: no sink and no decision, so no call.
    if (!decisions.empty() || (brainSink && !brainSink->Empty()))
        DrainDecisions(cfg, now);

    if (!botEchoes.empty())
    {
        events.insert(events.begin(), std::make_move_iterator(botEchoes.begin()),
                      std::make_move_iterator(botEchoes.end()));
        botEchoes.clear();
    }

    for (CoaEvent& event : events)
    {
        auto const start = std::chrono::steady_clock::now();
        if (!HandleEvent(cfg, event, now))
            continue;

        auto const us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
        if (CoaVoiceScopeKind const kind = KindOf(event); kind != CoaVoiceScopeKind::Count)
            costs[size_t(kind)].Add(uint32(std::min<int64>(us.count(), std::numeric_limits<uint32>::max())));
    }

    SendDue(cfg, now);

    if (now >= nextPruneMs)
    {
        nextPruneMs = now + PRUNE_PERIOD_MS;
        governor.Prune(now);
        // Kept at least as long as ScheduleDeny holds a refusal back.
        uint64 const denyMemory = std::max<uint64>(DENY_MEMORY_MS, sPlayerbotAIConfig.repeatDelay);
        std::erase_if(denyRepeat, [now, denyMemory](auto const& entry) { return entry.second + denyMemory < now; });
    }

    events.clear();
}

void CoaVoiceDirector::Shutdown()
{
    COA_ASSERT_WORLD_THREAD();

    if (!pending.empty())
        LOG_DEBUG("playerbots.voice", "CoA voice shutdown: {} lines dropped", pending.size());
    Reset();
    if (brainSink)
        brainSink->Close();
    closed = true;
}

void CoaVoiceDirector::Reset()
{
    pending = {};
    botEchoes.clear();
    governor.Reset();
    index.Clear();
    botFirstNames.clear();
    humanFirstNames.clear();
    denyRepeat.clear();
    nominees.clear();
    nextPruneMs = 0;
    active = false;

    // Decisions still waiting end here (counted as Disabled, so the events add up); a result that comes back
    // later carries an older epoch and is dropped as stale.
    if (!decisions.empty())
    {
        std::shared_ptr<IBrain> const brain = BrainService::Current();
        std::vector<PendingDecision*> noted;
        for (auto& [eventId, decision] : decisions)
        {
            brain->Cancel(decision.jobId);
            CountDrop(CoaVoiceDrop::Disabled);
            if (!decision.memoryNotes.empty())
                noted.push_back(&decision);
        }
        // M3: their messages are still remembered, oldest first, with the heuristic's rating.
        if (!noted.empty())
        {
            std::sort(noted.begin(), noted.end(),
                      [](PendingDecision const* a, PendingDecision const* b) { return a->eventId < b->eventId; });
            for (PendingDecision* decision : noted)
                DeliverMemoryNotes(decision->memoryNotes);
        }
        decisions.clear();
    }
    ++brainEpoch;
    whisperBrainToday.clear();
}

bool CoaVoiceDirector::HandleEvent(CoaVoiceConfig const& config, CoaEvent& event, uint64 nowMs)
{
    currentEventId = nextEventId++;
    CoaVoiceScopeKind const kind = KindOf(event);
    if (kind == CoaVoiceScopeKind::Count)
    {
        CountDrop(CoaVoiceDrop::NotConversation);
        return false;
    }

    // Every event is counted here and ends as one answer or one drop, so the stats add up.
    ++stats.events[size_t(kind)];
    if (event.fromBot)
        ++stats.botEvents;

    auto const drop = [&](CoaVoiceDrop why)
    {
        CountDrop(why);
        LOG_DEBUG("playerbots.voice", "event {} {}: {}", currentEventId, CoaVoiceScopeName(kind),
                  CoaVoiceDropName(why));
    };

    if (!CoaPlayerPrefs::Instance().IsLoaded())
    {
        drop(CoaVoiceDrop::PrefsNotLoaded);
        return false;
    }

    if (event.fromBot && !config.botToBot)
    {
        drop(CoaVoiceDrop::Disabled);
        return false;
    }

    Player* const sender = ObjectAccessor::FindPlayer(event.sender);
    if (!sender || !sender->GetSession() || sender->IsDuringRemoveFromWorld() ||
        (GET_PLAYERBOT_AI(sender) != nullptr) != event.fromBot)
    {
        drop(CoaVoiceDrop::SenderGone);
        return false;
    }

    if (!event.fromBot && CoaPlayerPrefs::Instance().VoiceOptedOut(sender->GetSession()->GetAccountId()))
    {
        drop(CoaVoiceDrop::OptOut);
        return false;
    }

    CoaVoiceScope scope;
    if (!ResolveScope(event, sender, scope))
    {
        LOG_DEBUG("playerbots.voice", "event {} {}: sender-gone", currentEventId, CoaVoiceScopeName(kind));
        return false;
    }

    // A whisper the voice leaves silent gets the refusal the legacy code would have sent (HandleCommand was told
    // to stay quiet about it). Not after this conversation's own limits (the bot's whisper cooldown, the scope's
    // cooldown and rate): the player just got an answer, or is flooding. The realm-wide cap is spent by other
    // people, so it does refuse; denyRepeat keeps refusals at the legacy rate.
    auto const fail = [&](CoaVoiceDrop why) { FailEvent(config, event, scope, why, nowMs); };

    if (!event.fromBot)
        governor.NoteHumanSpoke(scope.key, nowMs);
    else if (event.chainDepth + 1u > config.botChainMax)
    {
        drop(CoaVoiceDrop::Chain);
        return true;
    }
    else if (!governor.HumanSpokeWithin(scope.key, nowMs, config.humanWindowMs))
    {
        drop(CoaVoiceDrop::NoAudience);
        return true;
    }

    if (event.kind == CoaEventKind::Chat && !CoaChatIntent::IsConversation(event.text))
    {
        drop(CoaVoiceDrop::NotConversation);
        return true;
    }

    NameMatch::Prepared const words(kind == CoaVoiceScopeKind::TextEmote ? std::string_view()
                                                                         : std::string_view(event.text));
    nominees.clear();
    if (CoaVoiceDrop const why = Nominate(config, event, scope, sender, words, nominees); why != CoaVoiceDrop::Count)
    {
        fail(why);
        return true;
    }

    // M3: the bots a real player addresses remember the message whatever becomes of the answer. The notes go to the
    // memory when this returns, unless a brain job rating the message took them. Memory off: one relaxed load; a
    // memory that stayed off (tables missing) is not running: no notes.
    auto deliverNotes = [this] { DeliverMemoryNotes(memoryNotes); };
    std::optional<ScopeExit<decltype(deliverNotes)>> notesGuard;
    if (!event.fromBot && CoaMemoryConfig::Enabled() && CoaMemoryMgr::Running())
    {
        NoteConversation(event, kind, sender);
        notesGuard.emplace(deliverNotes);
    }

    // One draw: under the scope's chance anyone may answer; above it, up to a named bot's forced chance, only the
    // named bots may.
    bool const anyNamed =
        std::any_of(nominees.begin(), nominees.end(), [](CoaVoiceCandidate const& c) { return c.named; });
    double const roll = rand_chance();
    bool const anyHit = roll < ReplyChance(config, kind, false, event.chainDepth);
    bool const namedOnly = !anyHit && anyNamed && roll < ReplyChance(config, kind, true, event.chainDepth);
    // M2. One relaxed load when the brain is off; bots and text emotes never go to the brain. A calibrated chain
    // may still make a bot answer what the draw left silent, so such an event goes on to ask it.
    bool const brainRoute = !event.fromBot && event.kind == CoaEventKind::Chat && BrainService::ChatActive();
    bool const brainMayGate = brainRoute && BrainService::Current()->ChainCalibrated();
    if (!anyHit && !namedOnly && !brainMayGate)
    {
        fail(CoaVoiceDrop::Chance);
        return true;
    }

    if (pending.size() >= config.maxPending)
    {
        drop(CoaVoiceDrop::PendingFull);
        return true;
    }

    CoaIntent const intent = kind == CoaVoiceScopeKind::TextEmote
                                 ? EmoteIntent(CoaVoiceTemplates::EmoteCategoryFor(event.textEmote))
                                 : CoaChatIntent::Classify(event.text);

    // "Can you come?" to a free bot that would accept an invite: it asks for one (coa_deny_invite, the legacy
    // "Invite me to your group first") instead of declining, which also keeps a CoaLfg offer true. Without
    // DenyFallback the whisper stays unanswered rather than declined.
    if (kind == CoaVoiceScopeKind::Whisper && !event.fromBot && (intent == CoaIntent::Help || intent == CoaIntent::Lfg))
    {
        Player* const receiver = ObjectAccessor::FindPlayer(event.target);
        PlayerbotAI* const receiverAI = receiver ? GET_PLAYERBOT_AI(receiver) : nullptr;
        if (receiverAI && receiverAI->GetSecurity()->LevelFor(sender) == PLAYERBOT_SECURITY_INVITE)
        {
            LOG_DEBUG("playerbots.voice", "event {} whisper: {} is asked to join, answers with an invite request",
                      currentEventId, receiver->GetName());
            fail(CoaVoiceDrop::NoCandidate);
            return true;
        }
    }

    // Named bots first, so distance, combat or a recent answer never put an unnamed bot ahead of them.
    std::sort(nominees.begin(), nominees.end(),
              [](CoaVoiceCandidate const& a, CoaVoiceCandidate const& b)
              {
                  if (a.mentioned != b.mentioned)
                      return a.mentioned;
                  return a.score > b.score;
              });

    if (brainRoute)
    {
        bool const forced = anyNamed && IsForcedKind(kind);
        if (TrySubmitDecision(config, event, scope, sender, intent, anyHit, namedOnly, forced, nowMs))
            return true;
        // The brain could have said yes; it was not asked.
        if (!anyHit && !namedOnly)
        {
            fail(CoaVoiceDrop::Chance);
            return true;
        }
    }

    CoaVoiceDrop why = CoaVoiceDrop::NoCandidate;
    if (!RunBallot(config, event, scope, sender, nominees, intent, namedOnly, nowMs, nowMs, nullptr, nullptr, why))
        fail(why);
    return true;
}

uint32 CoaVoiceDirector::RunBallot(CoaVoiceConfig const& config, CoaEvent const& event, CoaVoiceScope const& scope,
                                   Player* sender, std::vector<CoaVoiceCandidate>& candidates, CoaIntent intent,
                                   bool namedOnly, uint64 eventMs, uint64 nowMs, BrainPick const* pick,
                                   BrainChatPlan const* plan, CoaVoiceDrop& why)
{
    CoaVoiceScopeKind const kind = scope.kind;
    lastResponder = ObjectGuid::Empty;
    std::size_t const shortlist = std::min<std::size_t>(candidates.size(), config.shortlist);

    // M2: the bot a calibrated brain picked goes first, the others keep their order. Never ahead of a mentioned
    // bot (candidates are sorted mentioned first): a named, whispered or emoted bot keeps its turn.
    if (pick && pick->whoIndex > 0 && std::size_t(pick->whoIndex) < shortlist &&
        (candidates[std::size_t(pick->whoIndex)].mentioned || (!namedOnly && !candidates.front().mentioned)))
        std::rotate(candidates.begin(), candidates.begin() + pick->whoIndex, candidates.begin() + pick->whoIndex + 1);

    // Every shortlisted bot is checked against the state before this event: the first responder's reservation
    // must not refuse a second one when MaxResponders > 1.
    std::array<CoaVoiceDrop, 10> gates{};
    for (std::size_t i = 0; i < shortlist && i < gates.size(); ++i)
        gates[i] = governor.PreCheck(config, candidates[i].guid, scope, event.sender, nowMs);

    CoaVoiceDrop gateReason = CoaVoiceDrop::Count;
    CoaVoiceDrop lineReason = CoaVoiceDrop::Count;
    uint32 responders = 0;
    for (std::size_t i = 0; i < shortlist && i < gates.size(); ++i)
    {
        CoaVoiceCandidate const& candidate = candidates[i];
        if ((responders || namedOnly) && !candidate.mentioned)
            continue;

        if (gates[i] != CoaVoiceDrop::Count)
        {
            if (gateReason == CoaVoiceDrop::Count)
                gateReason = gates[i];
            continue;
        }

        Player* const bot = ObjectAccessor::FindPlayer(candidate.guid);
        if (!bot)
            continue;

        CoaVoiceLine line;
        CoaVoiceDrop lineWhy = CoaVoiceDrop::NoLine;
        bool quiet = false;
        bool const composed =
            (pick && plan && !pick->lineOrder.empty() &&
             ComposeFromBrain(config, scope, bot, sender, intent, *pick, *plan, nowMs, line)) ||
            Compose(config, scope, bot, sender, intent, candidate.named, event.textEmote, nowMs, line, lineWhy, quiet);
        if (!composed)
        {
            if (lineReason == CoaVoiceDrop::Count || lineWhy == CoaVoiceDrop::Repetition)
                lineReason = lineWhy;
            continue;
        }

        // The brain's emote (0: none) replaces the persona's roll. A cold or grudge memory line has none (M3).
        if (pick && pick->emote >= 0 && config.replyEmotes && kind != CoaVoiceScopeKind::TextEmote && !quiet)
            line.emote = uint32(pick->emote);

        uint32 delayMin = config.replyDelayMinMs;
        uint32 delayMax = config.replyDelayMaxMs;
        if (IsEmoteScope(kind))
        {
            delayMin = config.emoteDelayMinMs;
            delayMax = config.emoteDelayMaxMs;
        }
        else if (bot->IsInCombat())
        {
            delayMin = config.combatDelayMinMs;
            delayMax = config.combatDelayMaxMs;
        }

        // From the event, not from now: the reply delay hides the time the brain took.
        line.dueMs = eventMs + urand(delayMin, delayMax);
        if (line.dueMs < nowMs)
            line.dueMs = nowMs;
        line.eventId = currentEventId;
        line.scope = scope;
        line.bot = candidate.guid;
        line.target = event.sender;
        line.depth = uint8(std::min<uint32>(event.chainDepth + 1u, 255u));
        line.intent = intent;

        governor.Reserve(config, candidate.guid, scope, event.sender, line.text, nowMs);
        LOG_DEBUG("playerbots.voice", "event {} {}: {} answers ({}) in {} ms", currentEventId, CoaVoiceScopeName(kind),
                  bot->GetName(), CoaChatIntent::IntentName(intent), line.dueMs - nowMs);
        if (lastResponder.IsEmpty())
            lastResponder = candidate.guid;
        Schedule(config, std::move(line));

        if (++responders >= config.maxResponders)
            break;
    }

    if (!responders)
        why = gateReason != CoaVoiceDrop::Count   ? gateReason
              : lineReason != CoaVoiceDrop::Count ? lineReason
                                                  : CoaVoiceDrop::NoCandidate;
    return responders;
}

CoaVoiceDrop CoaVoiceDirector::BallotGate(CoaVoiceConfig const& config,
                                          std::vector<CoaVoiceCandidate> const& candidates, CoaVoiceScope const& scope,
                                          ObjectGuid sender, bool namedOnly, uint64 nowMs) const
{
    std::size_t const shortlist = std::min<std::size_t>(candidates.size(), config.shortlist);
    CoaVoiceDrop first = CoaVoiceDrop::Count;
    for (std::size_t i = 0; i < shortlist; ++i)
    {
        if (namedOnly && !candidates[i].mentioned)
            continue;
        CoaVoiceDrop const gate = governor.PreCheck(config, candidates[i].guid, scope, sender, nowMs);
        if (gate == CoaVoiceDrop::Count)
            return CoaVoiceDrop::Count;
        if (first == CoaVoiceDrop::Count)
            first = gate;
    }
    return first;
}

void CoaVoiceDirector::FailEvent(CoaVoiceConfig const& config, CoaEvent const& event, CoaVoiceScope const& scope,
                                 CoaVoiceDrop why, uint64 nowMs)
{
    CountDrop(why);
    LOG_DEBUG("playerbots.voice", "event {} {}: {}", currentEventId, CoaVoiceScopeName(scope.kind),
              CoaVoiceDropName(why));
    if (scope.kind == CoaVoiceScopeKind::Whisper && !event.fromBot && config.denyFallback &&
        (why == CoaVoiceDrop::NoCandidate || why == CoaVoiceDrop::Chance || why == CoaVoiceDrop::NoLine ||
         why == CoaVoiceDrop::Repetition || why == CoaVoiceDrop::GlobalRate || why == CoaVoiceDrop::BrainSilent))
        ScheduleDeny(config, scope, ObjectAccessor::FindPlayer(event.target), ObjectAccessor::FindPlayer(event.sender),
                     nowMs);
}

bool CoaVoiceDirector::TrySubmitDecision(CoaVoiceConfig const& config, CoaEvent const& event,
                                         CoaVoiceScope const& scope, Player* sender, CoaIntent intent, bool anyHit,
                                         bool namedOnly, bool forced, uint64 nowMs)
{
    std::shared_ptr<IBrain> const brain = BrainService::Current();
    if (!brain->Active(BrainUse::ChatReply))
        return false;

    // The conversation's own limits hold back every bot whatever the brain says: M1's ballot drops the event on
    // them (silently, but for GlobalRate) and no call is paid for.
    if (CoaVoiceDrop const gate = BallotGate(config, nominees, scope, event.sender, false, nowMs);
        gate != CoaVoiceDrop::Count)
    {
        LOG_DEBUG("playerbots.voice", "event {} {}: {} holds every bot, not asked", currentEventId,
                  CoaVoiceScopeName(scope.kind), CoaVoiceDropName(gate));
        return false;
    }

    BrainStats& brainStats = BrainStats::Instance();
    uint32 const account = sender->GetSession()->GetAccountId();
    if (CoaPlayerPrefs::Instance().Get(account).AiOptedOut())
    {
        brainStats.Count(BrainFallback::AiOptOut);
        return false;
    }

    if (decisions.size() >= MAX_DECISIONS)
    {
        brainStats.Count(BrainFallback::Busy);
        return false;
    }

    bool const whisper = scope.kind == CoaVoiceScopeKind::Whisper;
    bool const whisperCapped = whisper && config.whisperBrainPerSenderPerDay;
    if (whisperCapped)
    {
        if (uint32 const today = BrainClock::UtcDayNow(); whisperBrainDay != today)
        {
            whisperBrainToday.clear();
            whisperBrainDay = today;
        }
        if (auto const used = whisperBrainToday.find(account);
            used != whisperBrainToday.end() && used->second >= config.whisperBrainPerSenderPerDay)
        {
            brainStats.Count(BrainFallback::WhisperCap);
            return false;
        }
    }

    // The shortlist in ballot order; a bot gone since the nomination is left out.
    std::size_t const shortlist = std::min<std::size_t>(nominees.size(), config.shortlist);
    std::vector<CoaVoiceCandidate> kept;
    kept.reserve(shortlist);
    BrainChatFacts facts;
    for (std::size_t i = 0; i < shortlist; ++i)
    {
        CoaVoiceCandidate const& candidate = nominees[i];
        Player* const bot = ObjectAccessor::FindPlayer(candidate.guid);
        if (!bot)
            continue;

        uint64 const last = governor.LastAnswerMs(candidate.guid);
        BrainBotFacts botFacts = BrainStateBuilder::BotFacts(bot, sender, scope.kind, whisper, candidate.named,
                                                             last && tickNowMs < last + MINUTE_MS);
        facts.speakerGrouped |= botFacts.sameGroup;
        facts.speakerGuilded |= botFacts.sameGuild;
        facts.bots.push_back(std::move(botFacts));
        kept.push_back(candidate);
    }
    if (kept.empty())
        return false;

    facts.kind = scope.kind;
    facts.chainDepth = event.chainDepth;
    facts.speakerLevel = sender->GetLevel();
    facts.features = BrainFeatures::Extract(event.text);
    facts.features.namedBots =
        uint32(std::count_if(kept.begin(), kept.end(), [](CoaVoiceCandidate const& c) { return c.named; }));

    BrainChatPlan plan;
    plan.lines = BrainStateBuilder::ChatLines(intent, kept.front().named, locale);
    // "who" picks among the bots M1 puts first: the mentioned ones (sorted first) when there are any, so an unnamed
    // bot never takes a named, whispered or emoted bot's turn; an answer past them is ignored.
    std::size_t const mentioned = std::size_t(
        std::count_if(kept.begin(), kept.end(), [](CoaVoiceCandidate const& c) { return c.mentioned; }));
    std::size_t const whoChoices = mentioned ? mentioned : kept.size();
    bool const askWho = brain->ChainCalibrated() && whoChoices > 1;
    plan.whoCount = askWho ? uint32(whoChoices) : 0;

    // M3: the rating questions and the [memory] lines. Memory off, or not running (tables missing): the job and its
    // cache key stay M2's, and no rating is paid for.
    if (CoaMemoryConfig::Enabled() && CoaMemoryMgr::Running())
    {
        std::shared_ptr<CoaMemoryConfig const> const memory = CoaMemoryConfig::Current();
        facts.askRating = !memoryNotes.empty() && memory->UseBrainRating();
        if (memory->stateTokenBudget)
        {
            std::vector<ObjectGuid> bots;
            bots.reserve(kept.size());
            for (CoaVoiceCandidate const& candidate : kept)
                bots.push_back(candidate.guid);
            facts.memory = CoaMemoryMgr::Instance().BrainLines(bots, event.sender, sender->GetZoneId(),
                                                               memory->stateTokenBudget * 4);
        }
    }

    BrainJob job;
    if (!BrainStateBuilder::BuildChatJob(facts, event.text, plan, askWho, job))
        return false;
    job.tag = currentEventId;
    job.epoch = brainEpoch;
    job.botGuid = kept.front().guid.GetRawValue();
    job.deadlineMs = nowMs + config.decisionTimeoutMs;

    if (!brainSink)
        brainSink = std::make_shared<BrainSink>(1024);

    BrainResult hit;
    BrainTicket const ticket = brain->Submit(std::move(job), brainSink, &hit);
    if (ticket.status != BrainSubmit::Queued && ticket.status != BrainSubmit::Answered)
    {
        brainStats.Count(BrainFallback::Submit);
        LOG_DEBUG("playerbots.voice", "event {} {}: brain refused ({}), heuristic", currentEventId,
                  CoaVoiceScopeName(scope.kind), BrainSubmitName(ticket.status));
        return false;
    }

    PendingDecision decision;
    decision.jobId = ticket.jobId;
    decision.epoch = brainEpoch;
    decision.eventId = currentEventId;
    decision.eventMs = nowMs;
    decision.deadlineMs = nowMs + config.decisionTimeoutMs;
    decision.submitMs = nowMs;
    decision.event = event;
    decision.scope = scope;
    decision.nominees = std::move(kept);
    decision.intent = intent;
    decision.anyHit = anyHit;
    decision.namedOnly = namedOnly;
    decision.forced = forced;
    decision.plan = std::move(plan);
    // M3: the notes wait for the rating; ResumeDecision delivers them.
    if (facts.askRating)
    {
        decision.memoryNotes = std::move(memoryNotes);
        memoryNotes.clear();
    }

    if (whisperCapped)
        ++whisperBrainToday[account];
    brainStats.deferred.fetch_add(1, std::memory_order_relaxed);

    if (ticket.status == BrainSubmit::Queued)
    {
        LOG_DEBUG("playerbots.voice", "event {} {}: waits for the brain", currentEventId,
                  CoaVoiceScopeName(scope.kind));
        decisions.emplace(currentEventId, std::move(decision));
        return true;
    }

    ResumeDecision(config, decision, &hit, nowMs);
    return true;
}

void CoaVoiceDirector::DrainDecisions(CoaVoiceConfig const& config, uint64 nowMs)
{
    if (brainSink)
    {
        brainSink->Drain(brainResults);
        for (BrainResult const& result : brainResults)
        {
            auto const found = decisions.find(result.tag);
            if (result.epoch != brainEpoch || found == decisions.end() || found->second.jobId != result.jobId)
            {
                BrainStats::Instance().stale.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            PendingDecision decision = std::move(found->second);
            decisions.erase(found);
            ResumeDecision(config, decision, &result, nowMs);
        }
        brainResults.clear();
    }

    if (decisions.empty())
        return;

    std::vector<uint64> late;
    for (auto const& [eventId, decision] : decisions)
        if (decision.deadlineMs <= nowMs)
            late.push_back(eventId);
    if (late.empty())
        return;

    // Oldest event first. A job a worker already holds still posts its result, which then counts as stale.
    std::sort(late.begin(), late.end());
    std::shared_ptr<IBrain> const brain = BrainService::Current();
    for (uint64 const eventId : late)
    {
        auto const found = decisions.find(eventId);
        brain->Cancel(found->second.jobId);
        PendingDecision decision = std::move(found->second);
        decisions.erase(found);
        ResumeDecision(config, decision, nullptr, nowMs);
    }
}

void CoaVoiceDirector::ResumeDecision(CoaVoiceConfig const& config, PendingDecision& decision,
                                      BrainResult const* result, uint64 nowMs)
{
    currentEventId = decision.eventId;

    // M3: the message is remembered once whatever happens below, rated by the brain when its answers are valid.
    if (!decision.memoryNotes.empty())
    {
        if (result)
            for (MemoryConversation& note : decision.memoryNotes)
                if (BrainStateBuilder::ReadRating(*result, note.sentiment))
                    ++stats.notesBrainRated;
        DeliverMemoryNotes(decision.memoryNotes);
    }

    CoaVoiceScopeKind const kind = decision.scope.kind;
    BrainStats& brainStats = BrainStats::Instance();
    (result ? brainStats.byAnswer : brainStats.byTimeout).fetch_add(1, std::memory_order_relaxed);

    // ReadDecision is pure: its answers are shown by .playerbots brain explain whatever happens next.
    BrainPick const pick = result ? ReadDecision(config, decision, *result) : BrainPick{};

    BrainDecisionEntry entry;
    entry.atUnix = uint64(std::time(nullptr));
    entry.eventId = decision.eventId;
    entry.botGuid = decision.nominees.empty() ? 0 : decision.nominees.front().guid.GetRawValue();
    entry.scope = CoaVoiceScopeName(kind);
    entry.ms = uint32(std::min<uint64>(nowMs - std::min(nowMs, decision.submitMs), std::numeric_limits<uint32>::max()));
    entry.calibrated = pick.calibrated;
    entry.replyP = pick.replyP;
    if (pick.whoIndex >= 0)
        entry.who = "b" + std::to_string(pick.whoIndex);
    entry.line = pick.lineKey;
    entry.lineP = pick.lineP;
    entry.tone = pick.tone;
    entry.emote = pick.emoteKey;
    if (result)
        entry.engine = result->engine;

    auto const finish = [&entry](std::string outcome)
    {
        entry.outcome = std::move(outcome);
        BrainDecisionRing::Instance().Add(std::move(entry));
    };
    auto const drop = [&](CoaVoiceDrop why)
    {
        CountDrop(why);
        LOG_DEBUG("playerbots.voice", "event {} {}: {}", currentEventId, CoaVoiceScopeName(kind),
                  CoaVoiceDropName(why));
        finish(std::string("drop:") + CoaVoiceDropName(why));
    };

    // The speaker as HandleEvent found them: still here, still a real player, still listening.
    Player* const sender = ObjectAccessor::FindPlayer(decision.event.sender);
    if (!sender || !sender->GetSession() || sender->IsDuringRemoveFromWorld() || GET_PLAYERBOT_AI(sender))
    {
        drop(CoaVoiceDrop::SenderGone);
        return;
    }
    if (CoaPlayerPrefs::Instance().VoiceOptedOut(sender->GetSession()->GetAccountId()))
    {
        drop(CoaVoiceDrop::OptOut);
        return;
    }

    if (!pick.fromBrain)
    {
        if (!result)
        {
            brainStats.Count(BrainFallback::Timeout);
            entry.reason = "timeout";
        }
        else if (result->error != BrainError::None)
        {
            brainStats.Count(BrainFallback::Error);
            entry.reason = std::string("error:") + BrainErrorName(result->error);
        }
        else
        {
            brainStats.Count(BrainFallback::Invalid);
            entry.reason = "invalid";
        }
    }

    // A calibrated engine decides whether anyone answers, except where M1 always answers a named bot.
    // Otherwise the draw made at event time decides, and the brain only ranks lines, tone and emote.
    bool reply = false;
    bool namedOnly = decision.namedOnly;
    bool const brainDecides = pick.fromBrain && pick.calibrated && pick.replyKnown && !decision.forced;
    if (brainDecides)
    {
        reply = !pick.silent;
        if (reply)
            namedOnly = false;
        (reply ? brainStats.brainReplied : brainStats.brainSilent).fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        reply = decision.anyHit || decision.namedOnly;
        if (pick.fromBrain && !pick.calibrated)
            brainStats.ranked.fetch_add(1, std::memory_order_relaxed);
    }

    LOG_DEBUG("playerbots.voice", "event {} {}: brain {} after {} ms ({}), reply p {:.2f}, {}", currentEventId,
              CoaVoiceScopeName(kind), result ? BrainErrorName(result->error) : "timeout", entry.ms,
              result ? result->engine : std::string("-"), pick.replyP,
              brainDecides ? (reply ? "the brain answers" : "the brain stays silent")
                           : (reply ? "the draw answers" : "the draw stays silent"));

    if (!reply)
    {
        CoaVoiceDrop why = pick.replyKnown ? CoaVoiceDrop::BrainSilent : CoaVoiceDrop::Chance;
        // As in M1's ballot, the conversation's own limits come first: a bot that just answered, or a flooded
        // scope, drops the event silently (GlobalRate still refuses) rather than with the brain's refusal.
        if (why == CoaVoiceDrop::BrainSilent)
            if (CoaVoiceDrop const gate = BallotGate(config, decision.nominees, decision.scope, decision.event.sender,
                                                     decision.namedOnly, nowMs);
                gate != CoaVoiceDrop::Count)
                why = gate;
        FailEvent(config, decision.event, decision.scope, why, nowMs);
        finish(why == CoaVoiceDrop::BrainSilent ? std::string("brain-silent")
                                                 : std::string("drop:") + CoaVoiceDropName(why));
        return;
    }

    if (pending.size() >= config.maxPending)
    {
        drop(CoaVoiceDrop::PendingFull);
        return;
    }

    CoaVoiceDrop why = CoaVoiceDrop::NoCandidate;
    if (!RunBallot(config, decision.event, decision.scope, sender, decision.nominees, decision.intent, namedOnly,
                   decision.eventMs, nowMs, pick.fromBrain ? &pick : nullptr, &decision.plan, why))
    {
        FailEvent(config, decision.event, decision.scope, why, nowMs);
        finish(std::string("drop:") + CoaVoiceDropName(why));
        return;
    }

    entry.botGuid = lastResponder.GetRawValue();
    finish(pick.fromCache ? "cache" : brainDecides ? "brain" : pick.fromBrain ? "ranked" : "heuristic");
}

CoaVoiceDirector::BrainPick CoaVoiceDirector::ReadDecision(CoaVoiceConfig const& config,
                                                           PendingDecision const& decision,
                                                           BrainResult const& result) const
{
    BrainPick pick;
    if (result.error != BrainError::None)
        return pick;

    auto const valid = [&result](std::string_view name) -> BrainAnswer const*
    {
        BrainAnswer const* const answer = result.Find(name);
        return answer && answer->valid ? answer : nullptr;
    };
    BrainAnswer const* const reply = valid("reply");
    BrainAnswer const* const who = valid("who");
    BrainAnswer const* const line = valid("line");
    BrainAnswer const* const tone = valid("tone");
    BrainAnswer const* const emote = valid("emote");
    if (!reply && !who && !line && !tone && !emote)
        return pick;

    pick.fromBrain = true;
    pick.calibrated = result.calibrated;
    pick.fromCache = result.fromCache;

    // Uncalibrated engines never decide reply or who: their p is UncalibratedConfidence, under every threshold.
    if (reply && reply->calibrated && pick.calibrated)
    {
        float decay = 1.0f;
        for (uint8 hop = 0; hop < decision.event.chainDepth; ++hop)
            decay *= config.chainDecay;
        pick.replyP = reply->p;
        pick.replyKnown = true;
        pick.silent = reply->p * decay < config.ThresholdFor(decision.scope.kind);
    }

    if (line)
    {
        // The choice leads: a cache hit samples it from the stored distribution, whose keys are reply names.
        std::vector<std::string_view> keys;
        keys.reserve(line->dist.size() + 1);
        keys.emplace_back(line->choice);
        for (auto const& [key, p] : line->dist)
            if (key != line->choice)
                keys.emplace_back(key);

        std::vector<BrainChatLine> const& lines = decision.plan.lines;
        for (std::size_t k = 0; k < keys.size(); ++k)
        {
            std::string_view const key = keys[k];
            if (key == "silent")
            {
                if (k == 0 && pick.calibrated && !pick.replyKnown)
                    pick.replyKnown = pick.silent = true;
                continue;
            }

            for (uint32 i = 0; i < lines.size(); ++i)
            {
                if ((pick.fromCache ? lines[i].replyName : lines[i].key) != key)
                    continue;
                if (std::find(pick.lineOrder.begin(), pick.lineOrder.end(), i) == pick.lineOrder.end())
                    pick.lineOrder.push_back(i);
            }
        }
        pick.lineKey = line->choice;
        pick.lineP = line->p;
    }

    if (who && who->calibrated && pick.calibrated && decision.plan.whoCount > 1 && who->choice.size() >= 2 &&
        who->choice[0] == 'b')
    {
        uint32 index = 0;
        bool digits = true;
        for (std::size_t i = 1; i < who->choice.size() && digits; ++i)
        {
            char const c = who->choice[i];
            digits = c >= '0' && c <= '9' && index < 1000;
            index = index * 10 + uint32(c - '0');
        }
        if (digits && index < decision.plan.whoCount && index < decision.nominees.size())
            pick.whoIndex = int32(index);
    }

    if (tone && tone->level >= 0 && tone->level <= 4)
        pick.tone = tone->level;

    if (emote)
    {
        std::vector<std::string> const& keys = QuestionRegistry::EmoteKeys();
        if (std::find(keys.begin(), keys.end(), emote->choice) != keys.end())
        {
            pick.emoteKey = emote->choice;
            pick.emote = int32(BrainStateBuilder::EmoteFor(emote->choice));
        }
    }
    return pick;
}

bool CoaVoiceDirector::ComposeFromBrain(CoaVoiceConfig const& config, CoaVoiceScope const& scope, Player* bot,
                                        Player* sender, CoaIntent intent, BrainPick const& pick,
                                        BrainChatPlan const& plan, uint64 nowMs, CoaVoiceLine& line)
{
    // M3: a disliked player or a grudge gets the memory's dry lines from Compose, never a ranked M1 line.
    if (MemoryTone const tone = ToneToward(bot, sender); tone == MemoryTone::Cold || tone == MemoryTone::Grudge)
        return false;

    CoaVoicePersona const persona = CoaVoicePersona::For(bot->GetGUID());
    CoaVoiceTemplates::Placeholders const placeholders = PlaceholdersFor(bot, sender);

    CoaVoiceSanitizeOptions options;
    options.maxBytes = config.maxBytes;
    options.blockedWords = &config.blockedWords;

    for (uint32 const index : pick.lineOrder)
    {
        if (index >= plan.lines.size())
            continue;

        std::string text = placeholders.Fill(plan.lines[index].row);
        // Cold and hostile tones get no smiley, "lol" or ellipsis.
        if (pick.tone < 0 || pick.tone >= 2)
            text = persona.ApplyQuirks(std::move(text), intent);
        if (!CoaVoiceSanitizer::Clean(text, options))
            continue;
        if (governor.IsRepeat(config, bot->GetGUID(), scope.key, text, nowMs))
            continue;

        line.text = std::move(text);
        // With an emote answered, RunBallot sets it; otherwise the persona rolls as in Compose.
        if (pick.emote < 0 && config.replyEmotes && rand_chance() < persona.emoteChance * 100.0)
            line.emote = CoaVoiceTemplates::ReplyEmoteFor(intent, persona);
        return true;
    }
    return false;
}

bool CoaVoiceDirector::ResolveScope(CoaEvent const& event, Player* sender, CoaVoiceScope& scope)
{
    scope.kind = KindOf(event);
    scope.chatType = event.chatType;
    scope.teamId = event.teamId;
    scope.mapId = sender->GetMapId();
    scope.instanceId = sender->GetInstanceId();
    scope.zoneId = sender->GetZoneId();

    uint64 key = CoaHash::U64(uint64(scope.kind));
    switch (scope.kind)
    {
        case CoaVoiceScopeKind::Whisper:
            key = CoaHash::U64(event.sender.GetRawValue(), CoaHash::U64(event.target.GetRawValue(), key));
            break;
        case CoaVoiceScopeKind::Party:
        case CoaVoiceScopeKind::Raid:
        {
            Group* const group = sender->GetGroup();
            if (!group || group->GetGUID() != event.groupGuid)
            {
                CountDrop(CoaVoiceDrop::SenderGone);
                return false;
            }
            scope.groupGuid = event.groupGuid;
            key = CoaHash::U64(event.groupGuid.GetRawValue(), key);
            break;
        }
        case CoaVoiceScopeKind::Guild:
            if (!event.guildId || sender->GetGuildId() != event.guildId)
            {
                CountDrop(CoaVoiceDrop::SenderGone);
                return false;
            }
            scope.guildId = event.guildId;
            key = CoaHash::U64(event.guildId, key);
            break;
        case CoaVoiceScopeKind::Channel:
            if (event.channelName.empty())
            {
                CountDrop(CoaVoiceDrop::SenderGone);
                return false;
            }
            scope.channelName = event.channelName;
            key = CoaHash::Bytes(NameMatch::Fold(event.channelName), CoaHash::U64(scope.teamId, key));
            break;
        default:
            // Say, yell and both emote kinds share one local state per map, instance and zone.
            key = CoaHash::U64(uint64(CoaVoiceScopeKind::Say));
            key = CoaHash::U64(scope.zoneId, CoaHash::U64(scope.instanceId, CoaHash::U64(scope.mapId, key)));
            break;
    }

    scope.key = key;
    return true;
}

CoaVoiceDrop CoaVoiceDirector::Nominate(CoaVoiceConfig const& config, CoaEvent const& event, CoaVoiceScope const& scope,
                                        Player* sender, NameMatch::Prepared const& words,
                                        std::vector<CoaVoiceCandidate>& out)
{
    bool const fromBot = event.fromBot;

    // "Aelys, tu viens ?": a line that opens on a name speaks to that one.
    std::string vocativeKey;
    if (scope.kind != CoaVoiceScopeKind::Whisper && scope.kind != CoaVoiceScopeKind::TextEmote &&
        !words.Vocative().empty())
        vocativeKey = NameMatch::Fold(words.Vocative());
    bool vocativeIsCandidate = false;

    // "s gray", "c linen", "help me" from a master or a GM: where HandleCommand runs (whispers, party, raid, the
    // speaker's own bots in guild and channels) a bot that obeys executes it, so it does not also chat about it.
    // For everyone else the everyday words stay conversation ("c est bon").
    bool const ownerCommand = !fromBot && event.kind == CoaEventKind::Chat && !IsLocal(scope.kind) &&
                              CoaChatIntent::DispatchesAsTrigger(event.text);

    auto const add = [&](Player* bot, std::string const& nameKey, bool target)
    {
        if (!vocativeKey.empty() && nameKey == vocativeKey)
            vocativeIsCandidate = true;
        if (ownerCommand && Obeys(bot, sender))
            return;

        bool const named = words.Mentions(nameKey);
        CoaVoiceCandidate candidate;
        candidate.guid = bot->GetGUID();
        candidate.named = named;
        candidate.mentioned = named || target;
        candidate.score = Score(scope, bot, sender, candidate.mentioned);
        out.push_back(candidate);
    };

    switch (scope.kind)
    {
        case CoaVoiceScopeKind::Whisper:
        {
            Player* const bot = ObjectAccessor::FindPlayer(event.target);
            if (bot && Eligible(scope, bot, sender, fromBot))
                add(bot, NameKeyOf(index, bot), true);
            break;
        }
        case CoaVoiceScopeKind::TextEmote:
        {
            Player* const bot = ObjectAccessor::FindPlayer(event.target);
            if (bot && bot->GetMapId() == sender->GetMapId() && bot->GetInstanceId() == sender->GetInstanceId() &&
                !bot->InBattleground() && bot->GetExactDist(sender) <= ListenRange(scope.kind) &&
                Eligible(scope, bot, sender, fromBot))
                add(bot, NameKeyOf(index, bot), true);
            break;
        }
        case CoaVoiceScopeKind::Party:
        case CoaVoiceScopeKind::Raid:
        {
            Group* const group = sender->GetGroup();
            if (!group)
                return CoaVoiceDrop::SenderGone;

            bool const subGroupOnly =
                (scope.chatType == CHAT_MSG_PARTY || scope.chatType == CHAT_MSG_PARTY_LEADER) && group->isRaidGroup();
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* const member = ref->GetSource();
                if (!member || member == sender || !GET_PLAYERBOT_AI(member))
                    continue;
                if (subGroupOnly && !group->SameSubGroup(sender, member))
                    continue;
                if (Eligible(scope, member, sender, fromBot))
                    add(member, NameKeyOf(index, member), false);
            }
            break;
        }
        case CoaVoiceScopeKind::Guild:
            for (uint32 const i : index.InGuild(scope.guildId))
            {
                CoaVoiceIndexedBot const& entry = index.Bots()[i];
                Player* const bot = ObjectAccessor::FindPlayer(entry.guid);
                if (bot && bot->GetGuildId() == scope.guildId && Eligible(scope, bot, sender, fromBot))
                    add(bot, entry.nameKey, false);
            }
            break;
        case CoaVoiceScopeKind::Channel:
        {
            ChannelMgr* const mgr = ChannelMgr::forTeam(TeamId(scope.teamId));
            Channel* const channel = mgr ? mgr->GetChannel(scope.channelName, nullptr, false) : nullptr;
            if (!channel)
                return CoaVoiceDrop::SenderGone;

            for (uint32 const i : index.InZone(scope.mapId, scope.zoneId))
            {
                CoaVoiceIndexedBot const& entry = index.Bots()[i];
                if (entry.teamId != scope.teamId)
                    continue;
                Player* const bot = ObjectAccessor::FindPlayer(entry.guid);
                if (bot && bot->IsInChannel(channel) && !bot->InBattleground() && Eligible(scope, bot, sender, fromBot))
                    add(bot, entry.nameKey, false);
            }
            break;
        }
        default:
        {
            // Say, yell, "/e": the bots of the speaker's zone that stand within earshot.
            float const range = ListenRange(scope.kind);
            for (uint32 const i : index.InZone(scope.mapId, scope.zoneId))
            {
                CoaVoiceIndexedBot const& entry = index.Bots()[i];
                if (entry.mapId != scope.mapId || entry.instanceId != scope.instanceId)
                    continue;
                Player* const bot = ObjectAccessor::FindPlayer(entry.guid);
                if (!bot || bot->GetMapId() != scope.mapId || bot->GetInstanceId() != scope.instanceId ||
                    !bot->IsAlive() || bot->GetExactDist(sender) > range)
                    continue;
                PlayerbotAI* const botAI = GET_PLAYERBOT_AI(bot);
                if (!botAI || botAI->IsOpposing(sender) || !Eligible(scope, bot, sender, fromBot))
                    continue;
                if (scope.kind == CoaVoiceScopeKind::CustomEmote && !words.Mentions(entry.nameKey))
                    continue;
                add(bot, entry.nameKey, false);
            }
            break;
        }
    }

    // The line opens on the name of someone who is not answering here (another player, or a bot out of earshot)
    // or of one candidate: only the ones it names may answer. NameMatch leaves the vocative empty for common
    // words ("Bon, ...", "Mais ..."), so bots named like them do not silence those lines.
    if (!vocativeKey.empty())
    {
        bool addressed = vocativeIsCandidate || botFirstNames.contains(vocativeKey);
        if (!addressed)
        {
            // A player by full name (also one who logged in since the last refresh), or by first name; the
            // speaker's own first name counts only when another player shares it.
            Player* const other = ObjectAccessor::FindPlayerByName(words.Vocative());
            addressed = other && other != sender;
            if (!addressed)
            {
                auto const human = humanFirstNames.find(vocativeKey);
                uint32 const own = !fromBot && NameMatch::Fold(FirstName(sender->GetName())) == vocativeKey ? 1 : 0;
                addressed = human != humanFirstNames.end() && human->second > own;
            }
        }

        if (addressed)
        {
            std::erase_if(out, [](CoaVoiceCandidate const& c) { return !c.mentioned; });
            if (out.empty())
                return CoaVoiceDrop::Addressed;
        }
    }

    if (out.empty())
        return CoaVoiceDrop::NoCandidate;

    // Too many: every mentioned bot stays, the others are sampled evenly (reservoir).
    if (out.size() > config.maxNominees)
    {
        auto const firstUnmentioned =
            std::stable_partition(out.begin(), out.end(), [](CoaVoiceCandidate const& c) { return c.mentioned; });
        std::size_t const mentioned = std::size_t(firstUnmentioned - out.begin());
        if (mentioned >= config.maxNominees)
            out.resize(mentioned);
        else
        {
            std::size_t const keep = config.maxNominees - mentioned;
            std::size_t const others = out.size() - mentioned;
            CoaVoiceCandidate* const rest = out.data() + mentioned;
            for (std::size_t i = keep; i < others; ++i)
            {
                std::size_t const j = urand(0, uint32(i));
                if (j < keep)
                    std::swap(rest[j], rest[i]);
            }
            out.resize(mentioned + keep);
        }
    }

    return CoaVoiceDrop::Count;
}

bool CoaVoiceDirector::Eligible(CoaVoiceScope const& scope, Player* bot, Player* sender, bool fromBot) const
{
    if (!bot || bot == sender || !bot->IsInWorld() || bot->IsDuringRemoveFromWorld() || bot->IsBeingTeleported())
        return false;

    WorldSession* const session = bot->GetSession();
    if (!session || session->isLogingOut())
        return false;

    PlayerbotAI* const botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    // A bot with a real master answers its master only (the rule of HandleBotOutgoingPacket), and never a bot.
    if (botAI->HasGameClientMaster() && (fromBot || botAI->GetMaster() != sender))
        return false;

    return !scope.IsPublic() || botAI->IsActivityAllowedCached();
}

int32 CoaVoiceDirector::Score(CoaVoiceScope const& scope, Player* bot, Player* sender, bool mentioned) const
{
    int32 score = mentioned ? 100 : 0;

    if (Group* const group = bot->GetGroup(); group && group == sender->GetGroup())
        score += 40;

    if (scope.kind != CoaVoiceScopeKind::Guild && bot->GetGuildId() && bot->GetGuildId() == sender->GetGuildId())
        score += 20;

    if (IsLocal(scope.kind))
        score -= int32(bot->GetExactDist(sender));

    score += int32(20.0f * CoaVoicePersona::For(bot->GetGUID()).chattiness);

    if (uint64 const last = governor.LastAnswerMs(bot->GetGUID()); last && tickNowMs < last + MINUTE_MS)
        score -= 30;

    if (bot->IsInCombat() && scope.kind != CoaVoiceScopeKind::Party && scope.kind != CoaVoiceScopeKind::Raid &&
        scope.kind != CoaVoiceScopeKind::Whisper)
        score -= 50;

    return score + int32(urand(0, 10));
}

double CoaVoiceDirector::ReplyChance(CoaVoiceConfig const& config, CoaVoiceScopeKind kind, bool named,
                                     uint8 depth) const
{
    uint32 chance = config.ChanceFor(kind);

    // A bot named in party, raid, guild, say, yell or a channel always answers (whispers and emotes are aimed at
    // one bot anyway and keep their own chance).
    if (named && kind != CoaVoiceScopeKind::Whisper && kind != CoaVoiceScopeKind::TextEmote &&
        kind != CoaVoiceScopeKind::CustomEmote)
        chance = 100;

    double p = chance / 100.0;
    for (uint8 hop = 0; hop < depth; ++hop)
        p *= config.chainDecay;

    return p * 100.0;
}

bool CoaVoiceDirector::Compose(CoaVoiceConfig const& config, CoaVoiceScope const& scope, Player* bot, Player* sender,
                               CoaIntent intent, bool named, uint32 textEmote, uint64 nowMs, CoaVoiceLine& line,
                               CoaVoiceDrop& why, bool& quiet)
{
    bool const isTextEmote = scope.kind == CoaVoiceScopeKind::TextEmote;
    CoaEmoteCategory const category = isTextEmote ? CoaVoiceTemplates::EmoteCategoryFor(textEmote)
                                                   : CoaEmoteCategory::None;
    char const* const name =
        isTextEmote ? CoaVoiceTemplates::EmoteName(category) : CoaVoiceTemplates::ReplyName(intent, named);
    why = CoaVoiceDrop::NoLine;
    quiet = false;
    if (!name)
        return false;

    // M3 (CoaMemory.MemoryReplies): the memory's own lines for a friend, a disliked player or a grudge, when the
    // realm has rows for them; otherwise the M1 name and path below, unchanged. Memory off: tone None at once.
    MemoryTone const tone = ToneToward(bot, sender);
    char const* const memoryName = MemoryReplyName(tone, isTextEmote ? EmoteIntent(category) : intent);
    std::vector<std::string> rows;
    if (memoryName)
        rows = CoaVoiceTemplates::Candidates(memoryName, locale, config.templateCandidates);
    bool const memoryLine = !rows.empty();
    quiet = memoryLine && (tone == MemoryTone::Cold || tone == MemoryTone::Grudge);

    CoaVoicePersona const persona = CoaVoicePersona::For(bot->GetGUID());
    CoaVoiceTemplates::Placeholders placeholders = PlaceholdersFor(bot, sender);

    CoaVoiceSanitizeOptions options;
    options.maxBytes = config.maxBytes;
    options.blockedWords = &config.blockedWords;

    auto const pick = [&](std::vector<std::string> const& texts)
    {
        for (std::string const& row : texts)
        {
            std::string text = quiet ? placeholders.Fill(row) : persona.ApplyQuirks(placeholders.Fill(row), intent);
            if (!CoaVoiceSanitizer::Clean(text, options))
                continue;
            if (governor.IsRepeat(config, bot->GetGUID(), scope.key, text, nowMs))
            {
                why = CoaVoiceDrop::Repetition;
                continue;
            }
            line.text = std::move(text);
            return true;
        }
        return false;
    };

    if (!memoryLine)
        rows = CoaVoiceTemplates::Candidates(name, locale, config.templateCandidates);
    bool found = pick(rows);

    // No coa_* row at all (texts not installed): the legacy "reply" rows, which say "%s" for the speaker.
    if (!found && rows.empty() && !isTextEmote)
    {
        placeholders.Set(CoaVoicePlaceholder::LEGACY_NAME, std::string(FirstName(sender->GetName())));
        found = pick(CoaVoiceTemplates::Candidates(CoaVoiceTemplates::LegacyReplyType(intent, named), locale,
                                                   config.templateCandidates));
    }

    if (!found)
        return false;

    if (memoryLine)
    {
        ++stats.toneLines[size_t(tone)];
        LOG_DEBUG("playerbots.voice", "event {} {}: {} uses a {} memory line", currentEventId,
                  CoaVoiceScopeName(scope.kind), bot->GetName(), ToneLogName(tone));
    }

    if (!quiet && !isTextEmote && config.replyEmotes && rand_chance() < persona.emoteChance * 100.0)
        line.emote = CoaVoiceTemplates::ReplyEmoteFor(intent, persona);
    return true;
}

void CoaVoiceDirector::Schedule(CoaVoiceConfig const& config, CoaVoiceLine&& line)
{
    if (pending.size() >= config.maxPending)
    {
        CountDrop(CoaVoiceDrop::PendingFull);
        return;
    }

    if (!line.isDeny)
        ++stats.scheduled[size_t(line.scope.kind)];
    pending.push(std::move(line));
}

void CoaVoiceDirector::ScheduleDeny(CoaVoiceConfig const& config, CoaVoiceScope const& scope, Player* bot,
                                    Player* sender, uint64 nowMs)
{
    PlayerbotAI* const botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI || !sender || !sender->GetSession())
        return;

    // Mirrors PlayerbotSecurity::CheckLevelFor, as HandleCommand would have called it for this whisper.
    DenyReason reason = PLAYERBOT_DENY_NONE;
    PlayerbotSecurityLevel const level = botAI->GetSecurity()->LevelFor(sender, &reason);
    if (level >= PLAYERBOT_SECURITY_ALLOW_ALL)
        return;

    // With a CommandPrefix, HandleCommand returns before its ALLOW_ALL check: an INVITE level hears nothing.
    if (level == PLAYERBOT_SECURITY_INVITE && !sPlayerbotAIConfig.commandPrefix.empty())
        return;

    Player* const master = botAI->GetMaster();
    if (master && botAI->IsOpposing(master) && master->GetSession() && !master->CanBeGameMaster())
        return;

    char const* name = CoaVoiceTemplates::DenyName(level, reason);
    if (!name)
        return;

    Player* leader = nullptr;
    if (level == PLAYERBOT_SECURITY_TALK && reason == PLAYERBOT_DENY_NOT_LEADER)
    {
        leader = botAI->GetGroupLeader();
        if (!leader)
            name = "coa_deny_busy";
    }

    uint64 const key =
        CoaHash::Bytes(name, CoaHash::U64(sender->GetGUID().GetRawValue(), CoaHash::U64(bot->GetGUID().GetRawValue())));
    if (auto const last = denyRepeat.find(key);
        last != denyRepeat.end() && nowMs < last->second + uint64(sPlayerbotAIConfig.repeatDelay))
    {
        ++stats.denyRepeats;
        LOG_DEBUG("playerbots.voice", "event {} whisper: {} refusal to {} held back", currentEventId, bot->GetName(),
                  sender->GetName());
        return;
    }

    CoaVoiceTemplates::Placeholders placeholders = PlaceholdersFor(bot, sender);
    placeholders.Set(CoaVoicePlaceholder::LEADER, leader ? leader->GetName() : std::string());

    CoaVoiceSanitizeOptions options;
    options.maxBytes = config.maxBytes;
    options.blockedWords = &config.blockedWords;

    std::string text;
    for (std::string const& row : CoaVoiceTemplates::Candidates(name, locale, config.templateCandidates))
    {
        text = placeholders.Fill(row);
        if (CoaVoiceSanitizer::Clean(text, options))
            break;
        text.clear();
    }

    if (text.empty())
    {
        text = placeholders.Fill(DenyDefault(name));
        if (!CoaVoiceSanitizer::Clean(text, options))
            return;
    }

    CoaVoiceLine line;
    line.dueMs = nowMs + urand(DENY_DELAY_MIN_MS, DENY_DELAY_MAX_MS);
    line.eventId = currentEventId;
    line.scope = scope;
    line.bot = bot->GetGUID();
    line.target = sender->GetGUID();
    line.text = std::move(text);
    line.depth = 1;
    line.isDeny = true;

    denyRepeat[key] = nowMs;
    LOG_DEBUG("playerbots.voice", "event {} whisper: {} refuses ({})", currentEventId, bot->GetName(), name);
    Schedule(config, std::move(line));
}

void CoaVoiceDirector::SendDue(CoaVoiceConfig const& config, uint64 nowMs)
{
    uint32 emitted = 0;
    while (!pending.empty() && pending.top().dueMs <= nowMs && emitted < config.maxEmitsPerTick)
    {
        CoaVoiceLine const line = pending.top();
        pending.pop();
        ++emitted;
        Send(config, line);
    }
}

bool CoaVoiceDirector::Send(CoaVoiceConfig const& config, CoaVoiceLine const& line)
{
    CoaVoiceScope const& scope = line.scope;
    auto const fail = [&](CoaVoiceDrop why)
    {
        CountDrop(why);
        LOG_DEBUG("playerbots.voice", "event {} {}: line dropped, {}", line.eventId, CoaVoiceScopeName(scope.kind),
                  CoaVoiceDropName(why));
        return false;
    };

    Player* const bot = ObjectAccessor::FindPlayer(line.bot);
    PlayerbotAI* const botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI || !bot->GetSession() || bot->GetSession()->isLogingOut() || bot->IsBeingTeleported() ||
        bot->IsDuringRemoveFromWorld())
        return fail(CoaVoiceDrop::BotGone);

    Player* target = nullptr;
    if (scope.kind == CoaVoiceScopeKind::Whisper || scope.kind == CoaVoiceScopeKind::TextEmote)
    {
        target = ObjectAccessor::FindConnectedPlayer(line.target);
        if (!target || !target->GetSession())
            return fail(CoaVoiceDrop::TargetGone);
    }
    else
        target = ObjectAccessor::FindPlayer(line.target);

    // The player may have opted out since the line was decided. A refusal still goes: the legacy one would too.
    if (!line.isDeny && TargetOptedOut(line.target, target))
        return fail(CoaVoiceDrop::OptOut);

    Channel* channel = nullptr;
    switch (scope.kind)
    {
        case CoaVoiceScopeKind::Party:
        case CoaVoiceScopeKind::Raid:
        {
            Group* const group = bot->GetGroup();
            if (!group || group->GetGUID() != scope.groupGuid)
                return fail(CoaVoiceDrop::BotGone);
            break;
        }
        case CoaVoiceScopeKind::Guild:
            if (bot->GetGuildId() != scope.guildId)
                return fail(CoaVoiceDrop::BotGone);
            break;
        case CoaVoiceScopeKind::Channel:
        {
            ChannelMgr* const mgr = ChannelMgr::forTeam(bot->GetTeamId());
            channel = mgr ? mgr->GetChannel(scope.channelName, nullptr, false) : nullptr;
            if (!channel || !bot->IsInChannel(channel))
                return fail(CoaVoiceDrop::BotGone);
            break;
        }
        case CoaVoiceScopeKind::Whisper:
            break;
        default:
            // A local answer makes no sense once the bot has left for another map or instance.
            if (bot->GetMapId() != scope.mapId || bot->GetInstanceId() != scope.instanceId)
                return fail(CoaVoiceDrop::BotGone);
            break;
    }

    if (scope.IsPublic() && bot->IsInCombat())
        return fail(CoaVoiceDrop::Combat);

    if (sObjectMgr->IsChatFiltered(line.text))
        return fail(CoaVoiceDrop::Filtered);

    if (line.emote)
        PlayEmote(bot, target, line.emote);

    bool said = true;
    {
        // The say/yell hook reads this depth into the BOT event of the line (CoaVoice.BotToBot).
        CoaVoiceEmit::ScopedDepth const depth(line.depth);
        switch (scope.kind)
        {
            case CoaVoiceScopeKind::Whisper:
                bot->Whisper(line.text, LANG_UNIVERSAL, target);
                botAI->LogChat("whispers to " + target->GetName(), line.text);
                break;
            case CoaVoiceScopeKind::Yell:
                said = botAI->Yell(line.text);
                break;
            case CoaVoiceScopeKind::Party:
                said = botAI->SayToParty(line.text);
                break;
            case CoaVoiceScopeKind::Raid:
                said = bot->GetGroup()->isRaidGroup() ? botAI->SayToRaid(line.text) : botAI->SayToParty(line.text);
                break;
            case CoaVoiceScopeKind::Guild:
                said = botAI->SayToGuild(line.text);
                break;
            case CoaVoiceScopeKind::Channel:
                channel->Say(bot->GetGUID(), line.text, LANG_UNIVERSAL);
                botAI->LogChat("tells channel " + channel->GetName(), line.text);
                break;
            default:
                said = botAI->Say(line.text);
                break;
        }
    }

    if (!said)
        return fail(CoaVoiceDrop::SendFailed);

    if (line.isDeny)
        ++stats.denies;
    else
        ++stats.sent[size_t(scope.kind)];

    if (config.botToBot && !line.isDeny &&
        (scope.kind == CoaVoiceScopeKind::Party || scope.kind == CoaVoiceScopeKind::Raid ||
         scope.kind == CoaVoiceScopeKind::Guild || scope.kind == CoaVoiceScopeKind::Channel))
        QueueBotEcho(line, bot);

    LOG_DEBUG("playerbots.voice", "event {} {}: {} said its line", line.eventId, CoaVoiceScopeName(scope.kind),
              bot->GetName());
    return true;
}

void CoaVoiceDirector::QueueBotEcho(CoaVoiceLine const& line, Player* bot)
{
    CoaEvent echo;
    echo.kind = CoaEventKind::Chat;
    echo.chatType = EchoChatType(line.scope.kind);
    echo.teamId = line.scope.teamId;
    echo.chainDepth = line.depth;
    echo.fromBot = true;
    echo.guildId = line.scope.guildId;
    echo.sender = bot->GetGUID();
    echo.groupGuid = line.scope.groupGuid;
    echo.channelName = line.scope.channelName;
    echo.text = line.text;
    echo.postedMs = tickNowMs;
    botEchoes.push_back(std::move(echo));
}

void CoaVoiceDirector::NoteConversation(CoaEvent const& event, CoaVoiceScopeKind kind, Player* sender)
{
    constexpr std::size_t MAX_NOTES = 3;
    memoryNotes.clear();
    if (std::none_of(nominees.begin(), nominees.end(), [](CoaVoiceCandidate const& c) { return c.mentioned; }))
        return;

    // Rated once for every bot addressed; the words are only read here, never kept.
    std::shared_ptr<CoaMemoryConfig const> const memory = CoaMemoryConfig::Current();
    bool const isTextEmote = kind == CoaVoiceScopeKind::TextEmote;
    MemoryConversation note;
    note.speaker = event.sender;
    note.speakerAccount = sender->GetSession()->GetAccountId();
    note.speakerName = sender->GetName();
    note.speakerLevel = uint8(sender->GetLevel());
    note.mapId = uint16(sender->GetMapId());
    note.zoneId = sender->GetZoneId();
    note.kind = TalkKindOf(kind);
    if (isTextEmote)
    {
        note.intent = EmoteIntent(CoaVoiceTemplates::EmoteCategoryFor(event.textEmote));
        note.textEmote = event.textEmote;
        note.sentiment = SentimentHeuristic::FromEmote(event.textEmote, *memory);
    }
    else
    {
        note.intent = CoaChatIntent::Classify(event.text);
        note.sentiment = SentimentHeuristic::Rate(event.text, note.intent, *memory);
    }

    for (CoaVoiceCandidate const& candidate : nominees)
    {
        if (!candidate.mentioned)
            continue;
        note.bot = candidate.guid;
        memoryNotes.push_back(note);
        if (memoryNotes.size() >= MAX_NOTES)
            break;
    }
}

void CoaVoiceDirector::DeliverMemoryNotes(std::vector<MemoryConversation>& notes)
{
    CoaMemoryMgr& memory = CoaMemoryMgr::Instance();
    for (MemoryConversation const& note : notes)
        memory.OnConversation(note);
    stats.notesDelivered += notes.size();
    notes.clear();
}

std::string CoaVoiceDirector::FormatStats() const
{
    COA_ASSERT_WORLD_THREAD();

    std::shared_ptr<CoaVoiceConfig const> const current = CoaVoiceConfig::Current();
    uint64 const now = CoaTime::NowMs();
    CoaEventBus& bus = CoaEventBus::Instance();

    auto const sum = [](auto const& counts)
    {
        uint64 total = 0;
        for (uint64 const n : counts)
            total += n;
        return total;
    };

    std::string out = Acore::StringFormat(
        "CoA voice: enable={} bot-to-bot={} prefs={} index={} bots ({}) pending={} scopes={}\n",
        current->enable ? 1 : 0, CoaVoiceConfig::BotToBotEnabled() ? 1 : 0,
        CoaPlayerPrefs::Instance().IsLoaded() ? "loaded" : "loading", index.Bots().size(),
        index.BuiltMs() ? Acore::StringFormat("{} s old", (now - index.BuiltMs()) / 1000) : std::string("not built"),
        pending.size(), governor.TrackedScopes());

    out += Acore::StringFormat("bus: posted={} dropped={} queued={}/{}\n", bus.Posted(), bus.Dropped(), bus.Pending(),
                               bus.Capacity());

    out += "events:";
    for (std::size_t kind = 0; kind < stats.events.size(); ++kind)
        out += Acore::StringFormat(" {}={}", CoaVoiceScopeName(CoaVoiceScopeKind(kind)), stats.events[kind]);
    out += Acore::StringFormat(" (bot={})\n", stats.botEvents);

    out += Acore::StringFormat("answers: scheduled={} sent={} refusals={} (held back={}) last minute={}\n",
                               sum(stats.scheduled), sum(stats.sent), stats.denies, stats.denyRepeats,
                               governor.RepliesLastMinute(now));

    out += "drops:";
    bool anyDrop = false;
    for (std::size_t drop = 0; drop < stats.drops.size(); ++drop)
    {
        if (!stats.drops[drop])
            continue;
        out += Acore::StringFormat(" {}={}", CoaVoiceDropName(CoaVoiceDrop(drop)), stats.drops[drop]);
        anyDrop = true;
    }
    out += anyDrop ? "\n" : " none\n";

    auto const cost = [this](CoaVoiceScopeKind kind)
    {
        uint32 p50 = 0;
        uint32 p95 = 0;
        costs[size_t(kind)].Percentiles(p50, p95);
        return Acore::StringFormat("{}/{}", p50, p95);
    };

    std::vector<uint32> all;
    for (CostRing const& ring : costs)
        all.insert(all.end(), ring.samples.begin(), ring.samples.begin() + ring.count);
    uint32 allP50 = 0;
    uint32 allP95 = 0;
    SamplePercentiles(all, allP50, allP95);

    BrainStats const& brain = BrainStats::Instance();
    auto const load = [](std::atomic<uint64> const& counter) { return counter.load(std::memory_order_relaxed); };
    out += Acore::StringFormat(
        "brain: {} waiting={} deferred={} answered={} timed-out={} replied={} silent={} ranked={} stale={} fallbacks:",
        BrainService::ChatActive() ? "on" : "off", decisions.size(), load(brain.deferred), load(brain.byAnswer),
        load(brain.byTimeout), load(brain.brainReplied), load(brain.brainSilent), load(brain.ranked),
        load(brain.stale));
    for (std::size_t fallback = 0; fallback < brain.fallbacks.size(); ++fallback)
        out += Acore::StringFormat(" {}={}", BrainFallbackName(BrainFallback(fallback)),
                                   load(brain.fallbacks[fallback]));
    out += "\n";

    // M3. Shown once memory has been on, so the stats of a realm without memory stay M2's.
    auto const counted = [](uint64 n) { return n != 0; };
    bool const memoryUsed = stats.notesDelivered || stats.notesBrainRated ||
                            std::any_of(stats.toneLines.begin(), stats.toneLines.end(), counted);
    if (CoaMemoryConfig::Enabled() || memoryUsed)
        out += Acore::StringFormat("memory: notes={} brain-rated={} tone friend={} cold={} grudge={}\n",
                                   stats.notesDelivered, stats.notesBrainRated,
                                   stats.toneLines[size_t(MemoryTone::Friend)],
                                   stats.toneLines[size_t(MemoryTone::Cold)],
                                   stats.toneLines[size_t(MemoryTone::Grudge)]);

    out +=
        Acore::StringFormat("cost us p50/p95: channel={} say={} whisper={} all={}/{}", cost(CoaVoiceScopeKind::Channel),
                            cost(CoaVoiceScopeKind::Say), cost(CoaVoiceScopeKind::Whisper), allP50, allP95);
    return out;
}
