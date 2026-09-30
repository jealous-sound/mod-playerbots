/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BattleGroundTactics.h"
#include "Chat.h"
#include "CoaPlayerPrefs.h"
#include "CoaSpecialization.h"
#include "CoaVoiceDirector.h"
#include "GuildTaskMgr.h"
#include "PerfMonitor.h"
#include "PlayerbotMgr.h"
#include "PlayerbotTextMgr.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "WorldSession.h"
#include <map>
#include <optional>
#include <sstream>
#include <string>

using namespace Acore::ChatCommands;

class playerbots_commandscript : public CommandScript
{
public:
    playerbots_commandscript() : CommandScript("playerbots_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable playerbotsDebugCommandTable = {
            {"bg", HandleDebugBGCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsAccountCommandTable = {
            {"setKey", HandleSetSecurityKeyCommand, SEC_PLAYER, Console::No},
            {"link", HandleLinkAccountCommand, SEC_PLAYER, Console::No},
            {"linkedAccounts", HandleViewLinkedAccountsCommand, SEC_PLAYER, Console::No},
            {"unlink", HandleUnlinkAccountCommand, SEC_PLAYER, Console::No},
        };

        static ChatCommandTable playerbotsVoiceCommandTable = {
            {"stats", HandleVoiceStatsCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsCommandTable = {
            {"bot", HandlePlayerbotCommand, SEC_PLAYER, Console::No},
            {"gtask", HandleGuildTaskCommand, SEC_GAMEMASTER, Console::Yes},
            {"pmon", HandlePerfMonCommand, SEC_GAMEMASTER, Console::Yes},
            {"rndbot", HandleRandomPlayerbotCommand, SEC_GAMEMASTER, Console::Yes},
            {"coa", HandleCoaRecruitCommand, SEC_GAMEMASTER, Console::No},
            {"optout", HandleOptOutCommand, SEC_PLAYER, Console::No},
            {"optin", HandleOptInCommand, SEC_PLAYER, Console::No},
            {"debug", playerbotsDebugCommandTable},
            {"account", playerbotsAccountCommandTable},
            {"voice", playerbotsVoiceCommandTable},
        };

        static ChatCommandTable commandTable = {
            {"playerbots", playerbotsCommandTable},
        };

        return commandTable;
    }

    static bool HandlePlayerbotCommand(ChatHandler* handler, char const* args)
    {
        return PlayerbotMgr::HandlePlayerbotMgrCommand(handler, args);
    }

    // .playerbots coa tank|heal|dps [class] : recruit a Conquest of Azeroth bot for that role, of
    // that class when one is named (".playerbots coa heal sun cleric").
    static bool HandleCoaRecruitCommand(ChatHandler* handler, char const* args)
    {
        Player* master = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        std::string const line = args ? args : "";
        std::size_t const space = line.find(' ');
        std::string const wanted = line.substr(0, space);
        std::string const className = space == std::string::npos ? "" : line.substr(space + 1);

        CoaRole role;
        if (wanted == "tank")
            role = CoaRole::Tank;
        else if (wanted == "heal" || wanted == "healer")
            role = CoaRole::Heal;
        else if (wanted == "dps")
            role = CoaRole::Dps;
        else
        {
            handler->SendSysMessage("Usage: .playerbots coa tank|heal|dps [class], e.g. .playerbots coa heal sun cleric");
            return true;
        }

        uint8 classId = 0;
        if (!className.empty())
        {
            classId = FindCoaClass(className);
            if (!classId)
            {
                handler->SendSysMessage("Unknown or ambiguous class '" + className + "'. Classes: Barbarian, Witch Doctor, "
                                        "Felsworn, Witch Hunter, Stormbringer, Knight of Xoroth, Guardian, Templar, "
                                        "Bloodmage, Ranger, Chronomancer, Necromancer, Pyromancer, Cultist, Starcaller, "
                                        "Sun Cleric, Tinker, Venomancer, Reaper, Primalist, Runemaster.");
                return true;
            }
        }

        if (!master)
            return false;

        // Handled either way: returning false would add the generic usage text to our message.
        std::string message;
        RecruitCoaBot(master, role, message, classId);
        handler->SendSysMessage(message);
        return true;
    }

    // .playerbots optout [topic] : with no topic, the account's settings and what the bots do with its messages.
    static bool HandleOptOutCommand(ChatHandler* handler, char const* args)
    {
        return HandleCoaPrefsCommand(handler, args, true);
    }

    // .playerbots optin <topic>
    static bool HandleOptInCommand(ChatHandler* handler, char const* args)
    {
        return HandleCoaPrefsCommand(handler, args, false);
    }

    // .playerbots voice stats : the voice director's counters. Chat and console commands run on the world thread.
    static bool HandleVoiceStatsCommand(ChatHandler* handler, char const* /*args*/)
    {
        std::istringstream lines(CoaVoiceDirector::Instance().FormatStats());
        for (std::string line; std::getline(lines, line);)
        {
            if (!line.empty())
                handler->SendSysMessage(line);
        }
        return true;
    }

    static bool HandleCoaPrefsCommand(ChatHandler* handler, char const* args, bool optOut)
    {
        WorldSession* const session = handler->GetSession();
        if (!session)
            return false;

        // Idempotent. With the voice off the preferences are not loaded at startup, only at first use.
        CoaPlayerPrefs& prefs = CoaPlayerPrefs::Instance();
        prefs.LoadAsync();

        std::string const line = args ? args : "";
        std::size_t const first = line.find_first_not_of(' ');
        std::string const word = first == std::string::npos ? "" : line.substr(first, line.find(' ', first) - first);

        std::optional<CoaPrefTopic> const topic = CoaPlayerPrefs::ParseTopic(word);
        if ((!word.empty() && !topic) || (word.empty() && !optOut))
        {
            handler->SendSysMessage(VoiceText("coa_voice_prefs_usage",
                                              "Usage: .playerbots optout or .playerbots optin, followed by ai, voice, "
                                              "memory, duels, hostile or all."));
            return true;
        }

        // A change made before the stored row is known would be written over the settings not named in it.
        if (!prefs.IsLoaded())
        {
            handler->SendSysMessage(
                VoiceText("coa_voice_prefs_loading", "Your settings are being loaded, try again in a moment."));
            return true;
        }

        uint32 const accountId = session->GetAccountId();
        if (!topic)
        {
            CoaPrefs const current = prefs.Get(accountId);
            std::map<std::string, std::string> values;
            values["%ai"] = PrefWord(current.aiText);
            values["%voice"] = PrefWord(current.voice);
            values["%memory"] = PrefWord(current.memory);
            // Duels and hostile are bits of activity_mask: off when set, default otherwise.
            values["%duels"] = PrefWord((current.activityMask & COA_OPTOUT_DUELS) ? COA_PREF_OUT : COA_PREF_DEFAULT);
            values["%hostile"] =
                PrefWord((current.activityMask & COA_OPTOUT_HOSTILE) ? COA_PREF_OUT : COA_PREF_DEFAULT);
            handler->SendSysMessage(VoiceText("coa_voice_prefs_status",
                                              "Your settings: AI answers %ai, bot answers %voice, bot memory %memory, "
                                              "duel offers %duels, hostile bots %hostile.",
                                              values));
            handler->SendSysMessage(VoiceText("coa_voice_disclosure",
                                              "Bots may answer your messages with ready-made lines. Nothing leaves "
                                              "the server and only your preferences are stored. .playerbots optout "
                                              "voice makes them ignore you, .playerbots optout shows your settings."));
            return true;
        }

        prefs.SetOptOut(accountId, *topic, optOut);
        std::map<std::string, std::string> const placeholders = {{"%topic", TopicWords(*topic)}};
        if (optOut)
            handler->SendSysMessage(
                VoiceText("coa_voice_optout_ack", "Preference saved: %topic off for your account.", placeholders));
        else
            handler->SendSysMessage(
                VoiceText("coa_voice_optin_ack", "Preference saved: %topic back on for your account.", placeholders));
        return true;
    }

    // A text row of coa_texts.sql in the bots' language, or the English default when the row is missing.
    static std::string VoiceText(std::string const& name, std::string const& english,
                                 std::map<std::string, std::string> const& placeholders = {})
    {
        return PlayerbotTextMgr::instance().GetBotTextOrDefault(name, english, placeholders);
    }

    static std::string PrefWord(uint8 value)
    {
        switch (value)
        {
            case COA_PREF_IN:
                return VoiceText("coa_voice_word_in", "on");
            case COA_PREF_OUT:
                return VoiceText("coa_voice_word_out", "off");
            default:
                return VoiceText("coa_voice_word_default", "default");
        }
    }

    static std::string TopicWords(CoaPrefTopic topic)
    {
        switch (topic)
        {
            case CoaPrefTopic::Ai:
                return VoiceText("coa_voice_topic_ai", "AI answers");
            case CoaPrefTopic::Voice:
                return VoiceText("coa_voice_topic_voice", "bot answers");
            case CoaPrefTopic::Memory:
                return VoiceText("coa_voice_topic_memory", "bot memory");
            case CoaPrefTopic::Duels:
                return VoiceText("coa_voice_topic_duels", "duel offers from bots");
            case CoaPrefTopic::Hostile:
                return VoiceText("coa_voice_topic_hostile", "hostile bots");
            default:
                return VoiceText("coa_voice_topic_all", "everything the bots do with your messages");
        }
    }

    static bool HandleRandomPlayerbotCommand(ChatHandler* handler, char const* args)
    {
        return RandomPlayerbotMgr::HandlePlayerbotConsoleCommand(handler, args);
    }

    static bool HandleGuildTaskCommand(ChatHandler* handler, char const* args)
    {
        return GuildTaskMgr::HandleConsoleCommand(handler, args);
    }

    static bool HandlePerfMonCommand(ChatHandler* /*handler*/, char const* args)
    {
        if (!strcmp(args, "reset"))
        {
            sPerfMonitor.Reset();
            return true;
        }

        if (!strcmp(args, "tick"))
        {
            sPerfMonitor.PrintStats(true, false);
            return true;
        }

        if (!strcmp(args, "stack"))
        {
            sPerfMonitor.PrintStats(false, true);
            return true;
        }

        if (!strcmp(args, "toggle"))
        {
            sPlayerbotAIConfig.perfMonEnabled = !sPlayerbotAIConfig.perfMonEnabled;
            if (sPlayerbotAIConfig.perfMonEnabled)
                LOG_INFO("playerbots", "Performance monitor enabled");
            else
                LOG_INFO("playerbots", "Performance monitor disabled");
            return true;
        }

        sPerfMonitor.PrintStats();
        return true;
    }

    static bool HandleDebugBGCommand(ChatHandler* handler, char const* args)
    {
        return BGTactics::HandleConsoleCommand(handler, args);
    }

    static bool HandleSetSecurityKeyCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
        {
            handler->PSendSysMessage("Usage: .playerbots account setKey <securityKey>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();
        std::string key = args;

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleSetSecurityKeyCommand(player, key);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleLinkAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
            return false;

        char* accountName = strtok((char*)args, " ");
        char* key = strtok(nullptr, " ");

        if (!accountName || !key)
        {
            handler->PSendSysMessage("Usage: .playerbots account link <accountName> <securityKey>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleLinkAccountCommand(player, accountName, key);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleViewLinkedAccountsCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleViewLinkedAccountsCommand(player);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleUnlinkAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
            return false;

        char* accountName = strtok((char*)args, " ");
        if (!accountName)
        {
            handler->PSendSysMessage("Usage: .playerbots account unlink <accountName>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleUnlinkAccountCommand(player, accountName);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }
};

void AddPlayerbotsCommandscripts() { new playerbots_commandscript(); }
