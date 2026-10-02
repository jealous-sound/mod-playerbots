/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAVOICEBANTER_H
#define PLAYERBOTS_COAVOICEBANTER_H

#include "BrainSink.h"
#include "BrainTypes.h"
#include "CoaVoiceCandidateIndex.h"
#include "CoaVoiceConfig.h"
#include "CoaVoiceEvent.h"
#include "CoaVoiceGovernor.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// What the banter takes from the director (implemented by a private adapter of CoaVoiceDirector): the candidate
// index, the governor, the locale, the director's way to build a channel scope and to queue a line. The banter never
// sends a line itself: it queues one CoaVoiceLine and the director's SendDue/Send does the rest (re-resolves the
// bot and the channel, checks combat, chat filter and validator, plays the emote, says it, and queues the BOT echo
// that lets a second bot answer).
class ICoaVoiceBanterHost
{
public:
    virtual ~ICoaVoiceBanterHost() = default;

    virtual CoaVoiceCandidateIndex const& Index() const = 0;
    virtual CoaVoiceGovernor& Governor() = 0;
    virtual uint32 Locale() const = 0;  // PlayerbotTextMgr::GetLocalePriority() of the tick

    // The scope of a line said in `channelName` by a bot of `teamId` standing on that map and in that zone, exactly
    // as ResolveScope builds it for an event of such a bot (kind Channel, the same key), so the governor's cooldowns,
    // rates and repetition history are shared with the replies to real players.
    virtual CoaVoiceScope ChannelScope(uint8 teamId, std::string const& channelName, uint32 mapId,
                                       uint32 zoneId) const = 0;

    // The director's Schedule: the line joins the pending queue (counted as scheduled). False: the queue is full.
    virtual bool Schedule(CoaVoiceLine&& line) = 0;

    virtual uint64 NextEventId() = 0;  // for the log lines and CoaVoiceLine::eventId
};

// Bot-to-bot ambient chatter, the "Barrens chat": now and then a bot says a short remark in the zone channel of a
// zone where a real player is, and a second bot may answer. Director-initiated; never names or targets a player;
// never during combat; at most CoaVoice.Banter.MaxPerMinutePerZone lines a minute per zone (every bot line in that
// zone's channel counts) and at most one exchange at a time per zone; an exchange is an opener and at most one
// answer (the M1 chain rules: BotChainMax 2). Off by default (CoaVoice.Banter.Enable).
//
// State machine of a zone: Idle -> (scan: a real player is present, the gates hold, the dice fall) Preparing (a
// speaker and a topic are chosen; the opener's text is a template row, or a text job and the emote a brain job,
// both waited for up to their deadlines) -> Opened (the line is queued with the governor reserved; the BOT echo may
// draw an answer) -> Cooling (MinGapSeconds, an exchange's last line) -> Idle. World thread only.
class CoaVoiceBanter
{
public:
    // World thread, every director tick with voice on, after the events were handled and before SendDue. One relaxed
    // test when Banter.Enable = 0 (and the state is dropped once when it was turned off). Every BanterScanMs (5 s):
    // drains the sink, advances the exchanges under way, and scans the zones (contract, section 9).
    void Update(CoaVoiceConfig const& config, ICoaVoiceBanterHost& host, uint64 nowMs);

    // World thread. The director calls it from Send for every line it said in a channel scope: a reply to a real
    // player, a banter opener (line.banter, depth 1) or a banter answer (line.banter, depth 2). It feeds the zone's
    // lines-per-minute count and the quiet clock, and BrainStats::banterOpeners / banterAnswers.
    void OnChannelLine(CoaVoiceLine const& line, uint64 nowMs);

    // World thread. A real player spoke in a channel scope (the director's HandleEvent, human events only): starts the
    // zone's Banter.QuietAfterHumanSeconds clock.
    void OnHumanSpoke(uint64 scopeKey, uint64 nowMs);

    // World thread. Voice turned off, or shutdown: cancels the brain jobs, closes nothing else, forgets every
    // exchange and zone.
    void Reset();

    // World thread (.playerbots voice stats): one line, "banter: ...".
    std::string FormatStats() const;

private:
    friend class CoaVoiceDirector;
    CoaVoiceDrop SendGate(CoaVoiceConfig const& config, CoaVoiceLine const& line, uint64 nowMs);

    // One zone the banter knows: where, the channel its real players share, the lines said in the last minute.
    struct Zone
    {
        uint32 mapId = 0;
        uint32 zoneId = 0;
        uint64 scopeKey = 0;           // the channel scope's key (governor state)
        std::string channelName;       // the channel as the core holds it
        uint8 teamId = 0;
        uint64 nextExchangeMs = 0;     // not before (MinGapSeconds after the last exchange, or the interval dice)
        uint64 lastHumanSpokeMs = 0;   // a real player spoke in the channel (Banter.QuietAfterHumanSeconds)
        uint64 lastSeenMs = 0;         // a scan found a real player here
        std::deque<uint64> linesMs;    // every bot line said in the channel during the last minute
        std::vector<std::string> humanNames;
        std::deque<std::string> recent;
        CoaBanterTopic lastTopic = CoaBanterTopic::Count;
        bool busy = false;             // an exchange is under way
    };

    // One exchange between its choice and its opener being queued.
    struct Exchange
    {
        uint64 id = 0;
        uint64 zoneKey = 0;
        ObjectGuid bot;
        CoaBanterTopic topic = CoaBanterTopic::Zone;
        uint64 startedMs = 0;
        uint64 deadlineMs = 0;       // past it the template goes
        uint64 textJobId = 0;        // 0: no text job
        uint64 emoteJobId = 0;       // 0: no emote job
        bool textDone = false;       // the text result is in (or there is no job)
        bool emoteDone = false;
        std::string templateText;    // the template fallback, already filled and checked
        std::string text;            // the generated line, finished by the worker, when it came
        int32 emote = -1;            // TextEmotes id from the brain, -1: the persona rolls
    };

    struct Stats
    {
        uint64 scans = 0;
        uint64 exchanges = 0;
        uint64 openers = 0;
        uint64 answers = 0;
        uint64 text = 0;
        uint64 skipped = 0;
    };

    std::unordered_map<uint64, uint64> humanSpoke;
    std::unordered_map<uint64, Zone> zones;  // (mapId << 40) | (zoneId << 8) | teamId: the factions have their own
    std::vector<Exchange> exchanges;         // under way, at most Banter.MaxZones
    std::shared_ptr<BrainSink> sink;         // created at the first job, closed by Reset
    std::vector<BrainResult> results;        // reused by Update
    uint32 epoch = 1;                        // ++ at Reset: results of another epoch are stale
    uint64 nextScanMs = 0;
    bool active = false;                     // state exists and must be dropped if banter turns off
    Stats stats;
};

#endif
