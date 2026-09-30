/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINDECISIONRING_H
#define PLAYERBOTS_BRAINDECISIONRING_H

#include "Define.h"
#include <array>
#include <string>
#include <vector>

// What the director did with one brain decision, for .playerbots brain explain <bot>.
struct BrainDecisionEntry
{
    uint64 atUnix = 0;
    uint64 eventId = 0;
    uint64 botGuid = 0;     // raw GUID of the responder; of the first nominee when nobody answered
    std::string scope;      // CoaVoiceScopeName
    std::string engine;     // "" when no result arrived
    std::string outcome;    // "brain", "brain-silent", "ranked", "heuristic", "cache", or "drop:<drop name>"
    std::string reason;     // why the heuristic ran: "timeout", "error:<BrainErrorName>", "invalid", "submit:<name>"
    float replyP = -1.0f;   // -1: not answered
    std::string who;        // "b1", "" when not asked or not used
    std::string line;       // chosen line key, "silent", or ""
    float lineP = -1.0f;
    int32 tone = -1;
    std::string emote;      // emote key, "" when none was answered
    uint32 ms = 0;          // submit to resume
    bool calibrated = false;
};

// The last 200 brain decisions, all bots together. World thread only (the director writes when it resumes a
// decision; commands and the status file read), so it takes no lock; COA_ASSERT_WORLD_THREAD at every entry.
class BrainDecisionRing
{
public:
    static constexpr uint32 Capacity = 200;

    static BrainDecisionRing& Instance();

    void Add(BrainDecisionEntry&& entry);

    // Newest first. At most `max` entries of that bot (responder or first nominee).
    std::vector<BrainDecisionEntry> ForBot(uint64 botGuid, uint32 max) const;

    // Newest first.
    std::vector<BrainDecisionEntry> Last(uint32 max) const;

    void Clear();

private:
    BrainDecisionRing() = default;
    BrainDecisionRing(BrainDecisionRing const&) = delete;
    BrainDecisionRing& operator=(BrainDecisionRing const&) = delete;

    std::array<BrainDecisionEntry, Capacity> entries;
    uint32 next = 0;
    uint32 count = 0;
};

#endif
