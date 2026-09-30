/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityConfig.h"
#include "ActivityDirector.h"
#include "ActivityKind.h"
#include "ActivityTypes.h"
#include "BattleGroundTactics.h"
#include "BrainAdmin.h"
#include "BrainConfig.h"
#include "BrainService.h"
#include "BrainStateBuilder.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CoaMemoryAdmin.h"
#include "CoaMemoryMgr.h"
#include "CoaPlayerPrefs.h"
#include "CoaSpecialization.h"
#include "CoaVoiceDirector.h"
#include "Config.h"
#include "GuildTaskMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "PlayerbotMgr.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "QuestionRegistry.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "WorldSession.h"
#include <cctype>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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

        static ChatCommandTable playerbotsBrainCommandTable = {
            {"status", HandleBrainStatusCommand, SEC_GAMEMASTER, Console::Yes},
            {"on", HandleBrainOnCommand, SEC_GAMEMASTER, Console::Yes},
            {"off", HandleBrainOffCommand, SEC_GAMEMASTER, Console::Yes},
            {"reset", HandleBrainResetCommand, SEC_GAMEMASTER, Console::Yes},
            {"explain", HandleBrainExplainCommand, SEC_GAMEMASTER, Console::Yes},
            {"test", HandleBrainTestCommand, SEC_GAMEMASTER, Console::Yes},
            {"probe", HandleBrainProbeCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsMemoryCommandTable = {
            {"show", HandleMemoryShowCommand, SEC_GAMEMASTER, Console::Yes},
            {"forget", HandleMemoryForgetCommand, SEC_GAMEMASTER, Console::Yes},
            {"wipe", HandleMemoryWipeCommand, SEC_GAMEMASTER, Console::Yes},
            {"purge", HandleMemoryPurgeCommand, SEC_GAMEMASTER, Console::Yes},
            {"stats", HandleMemoryStatsCommand, SEC_GAMEMASTER, Console::Yes},
            {"mine", HandleMemoryMineCommand, SEC_PLAYER, Console::No},
        };

        static ChatCommandTable playerbotsActivityCommandTable = {
            {"list", HandleActivityListCommand, SEC_GAMEMASTER, Console::Yes},
            {"info", HandleActivityInfoCommand, SEC_GAMEMASTER, Console::Yes},
            {"start", HandleActivityStartCommand, SEC_GAMEMASTER, Console::Yes},
            {"stop", HandleActivityStopCommand, SEC_GAMEMASTER, Console::Yes},
            {"pause", HandleActivityPauseCommand, SEC_GAMEMASTER, Console::Yes},
            {"resume", HandleActivityResumeCommand, SEC_GAMEMASTER, Console::Yes},
            {"reload", HandleActivityReloadCommand, SEC_GAMEMASTER, Console::Yes},
            {"census", HandleActivityCensusCommand, SEC_GAMEMASTER, Console::Yes},
            {"join", HandleActivityJoinCommand, SEC_PLAYER, Console::No},
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
            {"brain", playerbotsBrainCommandTable},
            {"memory", playerbotsMemoryCommandTable},
            {"activity", playerbotsActivityCommandTable},
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

    // .playerbots brain ...: the CoA brain's backends are BrainAdmin's. Chat and console commands run on the world
    // thread. Test and probe answers arrive ticks later, printed by CoaWorldScript through BrainAdmin::Update to the
    // requester's account (0: the console, through the playerbots.brain logger).
    static bool HandleBrainStatusCommand(ChatHandler* handler, char const* /*args*/)
    {
        return SendBrainLines(handler, BrainAdmin::Status());
    }

    static bool HandleBrainOnCommand(ChatHandler* handler, char const* /*args*/)
    {
        return SendBrainLines(handler, BrainAdmin::TurnOn());
    }

    static bool HandleBrainOffCommand(ChatHandler* handler, char const* /*args*/)
    {
        return SendBrainLines(handler, BrainAdmin::TurnOff());
    }

    static bool HandleBrainResetCommand(ChatHandler* handler, char const* /*args*/)
    {
        return SendBrainLines(handler, BrainAdmin::Reset());
    }

    // .playerbots brain explain <bot> : its last decisions, online or not.
    static bool HandleBrainExplainCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const words = BrainWords(args);
        if (words.size() != 1)
            return SendBrainUsage(handler);

        std::string name = words[0];
        ObjectGuid const guid =
            normalizePlayerName(name) ? sCharacterCache->GetCharacterGuidByName(name) : ObjectGuid::Empty;
        if (guid.IsEmpty())
        {
            SendBrainLine(handler, "unknown character " + words[0]);
            return true;
        }
        return SendBrainLines(handler, BrainAdmin::Explain(guid.GetRawValue(), name));
    }

    // .playerbots brain test <bot> <message...> : asks the chain what the director would ask for that message
    // whispered to that bot; nothing is said in game. .playerbots brain test selftest : the offline parser check.
    static bool HandleBrainTestCommand(ChatHandler* handler, char const* args)
    {
        std::string const line = args ? args : "";
        std::size_t const start = line.find_first_not_of(' ');
        if (start == std::string::npos)
            return SendBrainUsage(handler);
        std::size_t const end = line.find(' ', start);
        std::string const word = line.substr(start, end == std::string::npos ? std::string::npos : end - start);
        std::size_t const rest = end == std::string::npos ? std::string::npos : line.find_first_not_of(' ', end);
        std::string message = rest == std::string::npos ? "" : line.substr(rest);
        while (!message.empty() && message.back() == ' ')
            message.pop_back();

        // A bot named Selftest is still reachable: with a message, the word is a name.
        if (message.empty())
        {
            if (AsciiLower(word) == "selftest")
                return SendBrainLines(handler, BrainAdmin::SelfTest());
            return SendBrainUsage(handler);
        }

        if (!BrainService::Current()->Active(BrainUse::Test))
        {
            SendBrainLine(handler, "CoA brain is off");
            return true;
        }

        std::string name = word;
        Player* const bot = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
        if (!bot || !GET_PLAYERBOT_AI(bot))
        {
            SendBrainLine(handler, "brain test " + word + ": not a bot in the world");
            return true;
        }

        Player* const speaker = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        BrainTestJob test;
        std::string error;
        if (!BrainStateBuilder::BuildTestJob(bot, speaker, message, test, error))
        {
            SendBrainLine(handler, "brain test " + name + ": " + error);
            return true;
        }
        return SendBrainLines(handler, BrainAdmin::StartTest(RequesterAccount(handler), std::move(test)));
    }

    // .playerbots brain probe [engine] [record] : the question set's canned messages, sent to one engine.
    static bool HandleBrainProbeCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> words = BrainWords(args);
        for (std::string& word : words)
            word = AsciiLower(word);
        bool record = false;
        if (!words.empty() && words.back() == "record")
        {
            record = true;
            words.pop_back();
        }
        if (words.size() > 1)
            return SendBrainUsage(handler);

        if (!BrainService::Current()->Active(BrainUse::Probe))
        {
            SendBrainLine(handler, "CoA brain is off");
            return true;
        }

        std::string const engine = words.empty() ? "" : words[0];
        std::vector<BrainProbeJob> jobs = BrainStateBuilder::BuildProbeJobs(
            *QuestionRegistry::Current(), PlayerbotTextMgr::instance().GetLocalePriority());
        return SendBrainLines(handler,
                              BrainAdmin::StartProbe(RequesterAccount(handler), engine, record, std::move(jobs)));
    }

    static uint32 RequesterAccount(ChatHandler* handler)
    {
        return handler->GetSession() ? handler->GetSession()->GetAccountId() : 0;
    }

    static std::vector<std::string> BrainWords(char const* args)
    {
        std::vector<std::string> words;
        std::istringstream in(args ? args : "");
        for (std::string word; in >> word;)
            words.push_back(std::move(word));
        return words;
    }

    static std::string AsciiLower(std::string text)
    {
        for (char& c : text)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    // The brain's lines use '|' as a separator, which the client reads as an escape code unless doubled (the core's
    // own escapeCharacters flag has no effect). The console prints them as they are.
    static void SendBrainLine(ChatHandler* handler, std::string const& line)
    {
        if (!handler->GetSession() || line.find('|') == std::string::npos)
        {
            handler->SendSysMessage(line);
            return;
        }
        std::string text;
        text.reserve(line.size() + 8);
        for (char const c : line)
        {
            text += c;
            if (c == '|')
                text += '|';
        }
        handler->SendSysMessage(text);
    }

    static bool SendBrainLines(ChatHandler* handler, std::vector<std::string> const& lines)
    {
        for (std::string const& line : lines)
            SendBrainLine(handler, line);
        return true;
    }

    // Handled: returning false would add the generic subcommand list to the usage line.
    static bool SendBrainUsage(ChatHandler* handler)
    {
        SendBrainLine(handler, "Usage: .playerbots brain status|on|off|reset|explain <bot>|test <bot> <message>|"
                               "test selftest|probe [engine] [record]");
        return true;
    }

    // .playerbots memory ...: what bots remember, CoaMemoryAdmin's. Chat and console commands run on the world
    // thread. Answers read from the database arrive ticks later, printed by CoaWorldScript to the requester's
    // account. Lines go through SendBrainLine for the same '|' escaping.
    // show <bot> [player]: the bot name is the first word and the player the rest ("Kaelis Dawnfire"); quotes
    // give a two-word bot name.
    static bool HandleMemoryShowCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const words = MemoryWords(args);
        if (words.empty())
            return SendMemoryUsage(handler);
        return SendBrainLines(handler,
                              CoaMemoryAdmin::Show(RequesterAccount(handler), words[0], JoinMemoryWords(words, 1)));
    }

    // forget <bot> <player>
    static bool HandleMemoryForgetCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const words = MemoryWords(args);
        if (words.size() < 2)
            return SendMemoryUsage(handler);
        return SendBrainLines(handler, CoaMemoryAdmin::Forget(words[0], JoinMemoryWords(words, 1)));
    }

    // wipe <bot>
    static bool HandleMemoryWipeCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const words = MemoryWords(args);
        if (words.empty())
            return SendMemoryUsage(handler);
        return SendBrainLines(handler, CoaMemoryAdmin::Wipe(JoinMemoryWords(words, 0)));
    }

    // purge <character or #account or account name>
    static bool HandleMemoryPurgeCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const words = MemoryWords(args);
        if (words.empty())
            return SendMemoryUsage(handler);
        return SendBrainLines(handler, CoaMemoryAdmin::Purge(JoinMemoryWords(words, 0)));
    }

    static bool HandleMemoryStatsCommand(ChatHandler* handler, char const* /*args*/)
    {
        return SendBrainLines(handler, CoaMemoryAdmin::Stats(RequesterAccount(handler)));
    }

    // mine [page]: players see what bots keep about their own account, never another's.
    static bool HandleMemoryMineCommand(ChatHandler* handler, char const* args)
    {
        WorldSession* const session = handler->GetSession();
        if (!session)
            return false;

        std::vector<std::string> const words = MemoryWords(args);
        uint32 page = 1;
        if (words.size() > 1 || (words.size() == 1 && !ParseMemoryPage(words[0], page)))
        {
            SendBrainLine(handler, VoiceText("coa_mem_mine_usage", "Usage: .playerbots memory mine [page]"));
            return true;
        }
        uint32 const accountId = session->GetAccountId();
        return SendBrainLines(handler, CoaMemoryAdmin::Mine(accountId, accountId, page));
    }

    // Words split on blanks; a double-quoted span is one word with its blanks collapsed ("Jean  Dupont" gives
    // "Jean Dupont"). An unclosed quote runs to the end.
    static std::vector<std::string> MemoryWords(char const* args)
    {
        std::vector<std::string> words;
        std::string const line = args ? args : "";
        std::size_t i = 0;
        while (i < line.size())
        {
            char const c = line[i];
            if (c == ' ' || c == '\t')
            {
                ++i;
                continue;
            }
            if (c != '"')
            {
                std::size_t const end = line.find_first_of(" \t", i);
                words.push_back(line.substr(i, end == std::string::npos ? std::string::npos : end - i));
                i = end == std::string::npos ? line.size() : end;
                continue;
            }

            std::size_t const close = line.find('"', i + 1);
            std::string const quoted =
                line.substr(i + 1, close == std::string::npos ? std::string::npos : close - i - 1);
            std::string const joined = JoinMemoryWords(BrainWords(quoted.c_str()), 0);
            if (!joined.empty())
                words.push_back(joined);
            i = close == std::string::npos ? line.size() : close + 1;
        }
        return words;
    }

    static std::string JoinMemoryWords(std::vector<std::string> const& words, std::size_t from)
    {
        std::string joined;
        for (std::size_t i = from; i < words.size(); ++i)
        {
            if (!joined.empty())
                joined += ' ';
            joined += words[i];
        }
        return joined;
    }

    // 1 to 100000, digits only.
    static bool ParseMemoryPage(std::string const& word, uint32& page)
    {
        if (word.empty() || word.size() > 6)
            return false;
        uint32 value = 0;
        for (char const c : word)
        {
            if (c < '0' || c > '9')
                return false;
            value = value * 10 + uint32(c - '0');
        }
        if (!value || value > 100000)
            return false;
        page = value;
        return true;
    }

    static bool SendMemoryUsage(ChatHandler* handler)
    {
        SendBrainLine(handler, "Usage: .playerbots memory show <bot> [player]|forget <bot> <player>|wipe <bot>|"
                               "purge <character or #account>|stats|mine [page]");
        return true;
    }

    // .playerbots activity ...: the activity director's (ActivityDirector). Chat and console commands run on the
    // world thread, where the director lives. GM lines are English and go through SendBrainLine ('|' doubled).
    static bool HandleActivityListCommand(ChatHandler* handler, char const* /*args*/)
    {
        return SendBrainLines(handler, ActivityDirector::Instance().List());
    }

    // info <id>
    static bool HandleActivityInfoCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const words = BrainWords(args);
        uint32 id = 0;
        if (words.size() != 1 || !ParseActivityNumber(words[0], 1, ACTIVITY_MAX_ID, id))
            return SendActivityUsage(handler);
        return SendBrainLines(handler, ActivityDirector::Instance().Info(id));
    }

    // start <kind> [target] [alliance|horde] [level] [size]: the words in any order; the first number is the level,
    // the second the size. Any other word is the target of the kinds that take one (ActivityStartTakesTarget); the
    // director checks it. For a dungeon run the GM's own character is the player the run is for.
    static bool HandleActivityStartCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> words = BrainWords(args);
        for (std::string& word : words)
            word = AsciiLower(word);
        std::optional<ActivityKindId> const kind = words.empty() ? std::nullopt : ActivityKindFromName(words[0]);
        if (!kind)
            return SendActivityUsage(handler);

        ActivityStartRequest request;
        request.kind = *kind;
        uint32 numbers = 0;
        for (std::size_t i = 1; i < words.size(); ++i)
        {
            std::string const& word = words[i];
            uint32 number = 0;
            if (word == "alliance" || word == "horde")
            {
                if (request.team)
                    return SendActivityUsage(handler);
                request.team = uint8(word == "alliance" ? TEAM_ALLIANCE : TEAM_HORDE);
            }
            else if (word.find_first_not_of("0123456789") == std::string::npos)
            {
                // Digits alone are a level or a size, never a spot's name.
                if (numbers >= 2 || !ParseActivityNumber(word, 0, 255, number))
                    return SendActivityUsage(handler);
                if (numbers++ == 0)
                    request.level = uint8(number);
                else
                    request.size = number;
            }
            else if (ActivityStartTakesTarget(*kind) && request.target.empty())
                request.target = word;
            else
                return SendActivityUsage(handler);
        }

        Player* const gmPlayer = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        return SendBrainLines(handler, ActivityDirector::Instance().Start(std::move(request), gmPlayer));
    }

    // stop <id>|all
    static bool HandleActivityStopCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const words = BrainWords(args);
        uint32 id = 0;
        if (words.size() != 1 ||
            (AsciiLower(words[0]) != "all" && !ParseActivityNumber(words[0], 1, ACTIVITY_MAX_ID, id)))
            return SendActivityUsage(handler);
        return SendBrainLines(handler, ActivityDirector::Instance().Stop(id));
    }

    static bool HandleActivityPauseCommand(ChatHandler* handler, char const* /*args*/)
    {
        SendBrainLine(handler, ActivityDirector::Instance().Pause());
        return true;
    }

    static bool HandleActivityResumeCommand(ChatHandler* handler, char const* /*args*/)
    {
        SendBrainLine(handler, ActivityDirector::Instance().Resume());
        return true;
    }

    // reload: the config files again, then only AiPlayerbot.CoaActivity.* (and the brain keys it reads) applied, as
    // CoaWorldScript::OnAfterConfigLoad does for them; the rest waits for .reload config.
    static bool HandleActivityReloadCommand(ChatHandler* handler, char const* /*args*/)
    {
        if (!sConfigMgr->Reload())
        {
            SendBrainLine(handler, "could not reload the config files");
            return true;
        }

        std::shared_ptr<ActivityConfig const> const config = ActivityConfig::Load();
        ActivityConfig::Publish(config);
        // The duel rules read the players' opt-outs, which nothing loaded while voice and memory are off.
        if (config->enable)
            CoaPlayerPrefs::Instance().LoadAsync();
        ActivityDirector::Instance().OnConfigReload();
        SendBrainLine(handler, "AiPlayerbot.CoaActivity.* reloaded (other settings wait for .reload config)");
        return true;
    }

    // census [hours]: the director clamps the hours (1 to 168).
    static bool HandleActivityCensusCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const words = BrainWords(args);
        uint32 hours = 24;
        if (words.size() > 1 || (words.size() == 1 && !ParseActivityNumber(words[0], 1, 100000, hours)))
            return SendActivityUsage(handler);
        return SendBrainLines(handler, ActivityDirector::Instance().Census(hours));
    }

    // join [id]: players join an activity of their faction and level; no id picks the nearest one. The answer is a
    // localized line (coa_act_join_*).
    static bool HandleActivityJoinCommand(ChatHandler* handler, char const* args)
    {
        WorldSession* const session = handler->GetSession();
        Player* const player = session ? session->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::vector<std::string> const words = BrainWords(args);
        uint32 id = 0;
        if (words.size() > 1 || (words.size() == 1 && !ParseActivityNumber(words[0], 1, ACTIVITY_MAX_ID, id)))
        {
            SendBrainLine(handler, VoiceText("coa_act_join_usage",
                                             "Usage: .playerbots activity join [id] (no id: the nearest one)"));
            return true;
        }

        std::string const line = ActivityDirector::Instance().Join(player, id);
        if (!line.empty())
            SendBrainLine(handler, line);
        return true;
    }

    // Activity ids fit the playerbots_coa_activity `id` column (INT UNSIGNED); nine digits are plenty.
    static constexpr uint32 ACTIVITY_MAX_ID = 999999999;

    // Digits only, from `lowest` to `highest`.
    static bool ParseActivityNumber(std::string const& word, uint32 lowest, uint32 highest, uint32& value)
    {
        if (word.empty() || word.size() > 9 || word.find_first_not_of("0123456789") != std::string::npos)
            return false;
        uint32 const number = uint32(std::stoul(word));
        if (number < lowest || number > highest)
            return false;
        value = number;
        return true;
    }

    // The kinds whose start takes a target word: a duel spot (Duel.Spots), a Hunt.Targets or Raid.Targets name, a
    // Party.Spots or Loiter.Spots name. Dungeon runs, gangs and booty runs take none.
    static bool ActivityStartTakesTarget(ActivityKindId kind)
    {
        switch (kind)
        {
            case ActivityKindId::Duel:
            case ActivityKindId::Hunt:
            case ActivityKindId::Raid:
            case ActivityKindId::Party:
            case ActivityKindId::Loiter:
                return true;
            default:
                return false;
        }
    }

    static bool SendActivityUsage(ChatHandler* handler)
    {
        SendBrainLine(handler, "Usage: .playerbots activity list|info <id>|start duel [spot] [alliance|horde] [level] "
                               "[size]|start gang|dungeon|gurubashi [alliance|horde] [level] [size]|start "
                               "hunt|raid|party|loiter [target] [alliance|horde] [level] [size]|stop <id>|all|pause|"
                               "resume|reload|census [hours]|join [id]");
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
            std::string const brainNotice = BrainDisclosure();
            if (!brainNotice.empty())
                handler->SendSysMessage(brainNotice);
            else
                handler->SendSysMessage(VoiceText("coa_voice_disclosure",
                                                  "Bots may answer your messages with ready-made lines. Nothing "
                                                  "leaves the server and only your preferences are stored. "
                                                  ".playerbots optout voice makes them ignore you, .playerbots "
                                                  "optout shows your settings."));
            // "" while memory is off.
            std::string const memoryNotice = CoaMemoryAdmin::Disclosure();
            if (!memoryNotice.empty())
                handler->SendSysMessage(memoryNotice);
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

        // What bots already remember goes now. PurgeAccount is false at once while memory is not running; an
        // opt-out made then is purged when memory starts (CoaPlayerPrefs::MemoryOptOuts).
        if (optOut && (*topic == CoaPrefTopic::Memory || *topic == CoaPrefTopic::All) &&
            CoaMemoryMgr::Instance().PurgeAccount(accountId))
            handler->SendSysMessage(
                VoiceText("coa_mem_optout_purged", "The bots have forgotten everything about you."));
        return true;
    }

    // M1's disclosure says nothing leaves the server, which stops being true while the brain asks a hosted engine:
    // it then gets features of the messages, and their words when Privacy.SendPlayerText allows it. "" when no
    // hosted engine is asked. The brain rows are not in coa_texts.sql yet, so their English default is shown.
    static std::string BrainDisclosure()
    {
        std::string host;
        bool words = false;
        if (BrainService::ChatActive())
        {
            std::shared_ptr<BrainConfig const> const brain = BrainConfig::Current();
            for (BrainEngineConfig const& engine : brain->engines)
            {
                if (!engine.usable || engine.loopback || engine.dialect == BrainDialect::Mock || engine.host.empty())
                    continue;
                if (host.empty())
                    host = engine.host;
                words = words || engine.sendPlayerText;
            }
        }

        if (host.empty())
            return "";

        std::map<std::string, std::string> const values = {{"%host", host}};
        if (words)
            return VoiceText("coa_voice_disclosure_brain_text",
                             "Bots may answer your messages with ready-made lines, chosen with the help of %host, "
                             "which receives the messages bots may answer, without your character's name. Only "
                             "your preferences are stored. .playerbots optout ai stops this for your account, "
                             ".playerbots optout voice makes the bots ignore you.",
                             values);
        return VoiceText("coa_voice_disclosure_brain",
                         "Bots may answer your messages with ready-made lines, chosen with the help of %host, which "
                         "receives a description of the messages bots may answer (language, kind, length; never your "
                         "name or your words). Only your preferences are stored. .playerbots optout ai stops this for "
                         "your account, .playerbots optout voice makes the bots ignore you.",
                         values);
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
