/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotTextMgr.h"
#include "DatabaseEnv.h"
#include "PlayerbotAIConfig.h"
#include "QueryResult.h"    // Required due to a poor implementation by AC
#include "Random.h"
#include "WorldSessionMgr.h"
#include <algorithm>
#include <string_view>
#include <unordered_set>

namespace
{
// Distinct non-empty texts of the rows `keep` accepts, in `locale`, or in English when no such row has that locale,
// then a urand-driven partial Fisher-Yates keeps maxCount of them. Dedupe because ".bot reload" appends every row
// again (LoadBotTexts never clears the map).
template <typename Keep>
uint32 PickCandidates(std::vector<BotTextEntry> const& rows, Keep const& keep, uint32 locale, uint32 maxCount,
                      std::vector<std::string>& out)
{
    if (!maxCount)
        return 0;
    if (locale >= TOTAL_LOCALES)
        locale = 0;

    std::vector<std::string const*> texts;
    std::unordered_set<std::string_view> seen;
    auto const collect = [&](uint32 textLocale)
    {
        for (BotTextEntry const& row : rows)
        {
            if (!keep(row))
                continue;
            auto const text = row.m_text.find(textLocale);
            if (text == row.m_text.end() || text->second.empty())
                continue;
            if (seen.insert(text->second).second)
                texts.push_back(&text->second);
        }
    };

    if (locale)
        collect(locale);
    if (texts.empty())
        collect(0);

    uint32 const count = uint32(texts.size());
    uint32 const kept = std::min(maxCount, count);
    out.reserve(kept);
    for (uint32 i = 0; i < kept; ++i)
    {
        std::swap(texts[i], texts[urand(i, count - 1)]);
        out.push_back(*texts[i]);
    }
    return kept;
}
}  // namespace

void PlayerbotTextMgr::replaceAll(std::string& str, std::string const& from, std::string const& to)
{
    if (from.empty())
        return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos)
    {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();  // In case 'to' contains 'from', like replacing 'x' with 'yx'
    }
}

void PlayerbotTextMgr::LoadBotTexts()
{
    LOG_INFO("playerbots", "Loading playerbots texts...");

    uint32 count = 0;
    if (PreparedQueryResult result =
            PlayerbotsDatabase.Query(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_TEXT)))
    {
        do
        {
            std::map<uint32, std::string> text;
            Field* fields = result->Fetch();
            std::string name = fields[0].Get<std::string>();
            text[0] = fields[1].Get<std::string>();
            uint8 sayType = fields[2].Get<uint8>();
            uint8 replyType = fields[3].Get<uint8>();
            for (uint8 i = 1; i < TOTAL_LOCALES; ++i)
            {
                text[i] = fields[i + 3].Get<std::string>();
            }

            botTexts[name].push_back(BotTextEntry(name, text, sayType, replyType));
            ++count;
        } while (result->NextRow());
    }

    LOG_INFO("playerbots", "{} playerbots texts loaded", count);
}

void PlayerbotTextMgr::LoadBotTextChance()
{
    if (botTextChance.empty())
    {
        QueryResult results = PlayerbotsDatabase.Query("SELECT name, probability FROM ai_playerbot_texts_chance");
        if (results)
        {
            do
            {
                Field* fields = results->Fetch();
                std::string name = fields[0].Get<std::string>();
                uint32 probability = fields[1].Get<uint32>();

                botTextChance[name] = probability;
            } while (results->NextRow());
        }
    }
}

// general texts

std::string PlayerbotTextMgr::GetBotText(std::string name)
{
    if (botTexts.empty())
    {
        LOG_ERROR("playerbots", "Can't get bot text {}! No bots texts loaded!", name);
        return "";
    }

    // Several map threads read the texts at the same time: look up only, never insert.
    auto const found = botTexts.find(name);
    if (found == botTexts.end() || found->second.empty())
    {
        LOG_ERROR("playerbots", "Can't get bot text {}! No bots texts for this name!", name);
        return "";
    }

    std::vector<BotTextEntry> const& list = found->second;
    BotTextEntry textEntry = list[urand(0, list.size() - 1)];
    return !textEntry.m_text[GetLocalePriority()].empty() ? textEntry.m_text[GetLocalePriority()] : textEntry.m_text[0];
}

std::string PlayerbotTextMgr::GetBotText(std::string name, std::map<std::string, std::string> placeholders)
{
    std::string botText = GetBotText(name);
    if (botText.empty())
        return "";

    for (std::map<std::string, std::string>::iterator i = placeholders.begin(); i != placeholders.end(); ++i)
        replaceAll(botText, i->first, i->second);

    return botText;
}

std::string PlayerbotTextMgr::GetBotTextOrDefault(std::string name, std::string defaultText,
    std::map<std::string, std::string> placeholders)
{
    std::string botText = GetBotText(name, placeholders);
    if (botText.empty())
    {
        for (std::map<std::string, std::string>::iterator i = placeholders.begin(); i != placeholders.end(); ++i)
        {
            replaceAll(defaultText, i->first, i->second);
        }
        return defaultText;
    }

    return botText;
}

// chat replies

std::string PlayerbotTextMgr::GetBotText(ChatReplyType replyType, std::map<std::string, std::string> placeholders)
{
    if (botTexts.empty())
    {
        LOG_ERROR("playerbots", "Can't get bot text reply {}! No bots texts loaded!", replyType);
        return "";
    }
    auto const found = botTexts.find("reply");
    if (found == botTexts.end() || found->second.empty())
    {
        LOG_ERROR("playerbots", "Can't get bot text reply {}! No bots texts replies!", replyType);
        return "";
    }

    std::vector<BotTextEntry> const& list = found->second;
    std::vector<BotTextEntry> proper_list;
    for (auto text : list)
    {
        if (text.m_replyType == replyType)
            proper_list.push_back(text);
    }

    if (proper_list.empty())
        return "";

    BotTextEntry textEntry = proper_list[urand(0, proper_list.size() - 1)];
    std::string botText =
        !textEntry.m_text[GetLocalePriority()].empty() ? textEntry.m_text[GetLocalePriority()] : textEntry.m_text[0];
    for (auto& placeholder : placeholders)
        replaceAll(botText, placeholder.first, placeholder.second);

    return botText;
}

std::string PlayerbotTextMgr::GetBotText(ChatReplyType replyType, std::string name)
{
    std::map<std::string, std::string> placeholders;
    placeholders["%s"] = name;

    return GetBotText(replyType, placeholders);
}

// CoA voice candidates

uint32 PlayerbotTextMgr::GetCandidates(std::string const& name, uint32 locale, uint32 maxCount,
                                       std::vector<std::string>& out) const
{
    out.clear();
    auto const found = botTexts.find(name);
    if (found == botTexts.end())
        return 0;

    auto const any = [](BotTextEntry const&) { return true; };
    return PickCandidates(found->second, any, locale, maxCount, out);
}

uint32 PlayerbotTextMgr::GetCandidates(ChatReplyType replyType, uint32 locale, uint32 maxCount,
                                       std::vector<std::string>& out) const
{
    out.clear();
    auto const found = botTexts.find("reply");
    if (found == botTexts.end())
        return 0;

    auto const ofType = [replyType](BotTextEntry const& row) { return row.m_replyType == uint32(replyType); };
    return PickCandidates(found->second, ofType, locale, maxCount, out);
}

// probabilities

bool PlayerbotTextMgr::rollTextChance(std::string name)
{
    auto const found = botTextChance.find(name);
    if (found == botTextChance.end() || !found->second)
        return true;

    return urand(0, 100) < found->second;
}

bool PlayerbotTextMgr::GetBotText(std::string name, std::string& text)
{
    if (!rollTextChance(name))
        return false;

    text = GetBotText(name);
    return !text.empty();
}

bool PlayerbotTextMgr::GetBotText(std::string name, std::string& text, std::map<std::string, std::string> placeholders)
{
    if (!rollTextChance(name))
        return false;

    text = GetBotText(name, placeholders);
    return !text.empty();
}

void PlayerbotTextMgr::AddLocalePriority(uint32 locale)
{
    if (locale >= TOTAL_LOCALES)
    {
        LOG_WARN("playerbots", "Ignoring locale {} for bot texts because it exceeds TOTAL_LOCALES ({})", locale, TOTAL_LOCALES - 1);
        return;
    }

    botTextLocalePriority[locale].fetch_add(1, std::memory_order_relaxed);
}

uint32 PlayerbotTextMgr::GetLocalePriority()
{
    // AiPlayerbot.BotTextLocale forces one language regardless of the clients connected,
    // e.g. 2 (frFR) for a French server whose players run an enUS client.
    int32 const forced = sPlayerbotAIConfig.botTextLocale;
    if (forced >= 0 && forced < TOTAL_LOCALES)
        return uint32(forced);

    // if no real players online, reset top locale
    uint32 const activeSessions = sWorldSessionMgr->GetActiveSessionCount();
    if (!activeSessions)
    {
        ResetLocalePriority();
        return 0;
    }

    uint32 topLocale = 0;
    for (uint8 i = 0; i < TOTAL_LOCALES; ++i)
    {
        if (botTextLocalePriority[i].load(std::memory_order_relaxed) >
            botTextLocalePriority[topLocale].load(std::memory_order_relaxed))
            topLocale = i;
    }

    return topLocale;
}

void PlayerbotTextMgr::ResetLocalePriority()
{
    for (uint8 i = 0; i < TOTAL_LOCALES; ++i)
    {
        botTextLocalePriority[i].store(0, std::memory_order_relaxed);
    }
}
