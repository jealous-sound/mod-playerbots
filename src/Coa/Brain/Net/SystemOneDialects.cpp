/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SystemOneDialects.h"
#include "BrainJson.h"
#include "Log.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <mutex>
#include <utility>

namespace
{
constexpr std::size_t MaxDialectFileBytes = 64 * 1024;

using DialectPtr = std::shared_ptr<SystemOneDialect const>;
using DialectTable = std::vector<DialectPtr>;

DialectTable MakeBuiltInTable()
{
    DialectTable table;

    {
        SystemOneDialect d;
        d.name = "openrouter";
        d.kind = BrainDialect::OpenRouter;
        d.defaultUrl = "https://openrouter.ai/api/alpha/decisions";
        d.defaultModel = "~typesafe/jev-latest";
        d.hosted = true;
        d.requiresKey = true;
        d.loopbackOnly = false;
        d.calibratedByDefault = true;
        d.network = true;
        d.built = true;
        table.push_back(std::make_shared<SystemOneDialect const>(std::move(d)));
    }
    {
        SystemOneDialect d;
        d.name = "typesafe";
        d.kind = BrainDialect::TypeSafe;
        d.defaultUrl = "https://api.typesafe.ai/v1/systemone";
        d.alternateUrls = {"https://openrouter.ai/api/v1/systemone"};
        d.defaultModel = "jev-latest";
        d.hosted = true;
        d.requiresKey = true;
        d.loopbackOnly = false;
        d.calibratedByDefault = true;
        d.network = true;
        d.built = true;
        table.push_back(std::make_shared<SystemOneDialect const>(std::move(d)));
    }
    {
        SystemOneDialect d;
        d.name = "jevlocal";
        d.kind = BrainDialect::JevLocal;
        d.defaultUrl = "http://127.0.0.1:8090/v1/systemone";
        d.defaultModel = "jev-latest";
        d.hosted = false;
        d.requiresKey = false;
        d.loopbackOnly = true;
        d.calibratedByDefault = true;
        d.network = true;
        d.built = true;
        table.push_back(std::make_shared<SystemOneDialect const>(std::move(d)));
    }
    {
        SystemOneDialect d;
        d.name = "mock";
        d.kind = BrainDialect::Mock;
        d.defaultUrl = "";
        d.defaultModel = "mock";
        d.hosted = false;
        d.requiresKey = false;
        d.loopbackOnly = false;
        d.calibratedByDefault = true;
        d.network = false;
        d.built = true;
        table.push_back(std::make_shared<SystemOneDialect const>(std::move(d)));
    }
    {
        SystemOneDialect d;
        d.name = "openai-constrained";
        d.kind = BrainDialect::OpenAiConstrained;
        d.defaultUrl = "http://127.0.0.1:8080/v1/chat/completions";
        d.defaultModel = "";
        d.hosted = false;
        d.requiresKey = false;
        d.loopbackOnly = true;
        d.calibratedByDefault = false;
        d.network = true;
        d.built = false;
        table.push_back(std::make_shared<SystemOneDialect const>(std::move(d)));
    }

    return table;
}

DialectTable const& BuiltInTable()
{
    static DialectTable const table = MakeBuiltInTable();
    return table;
}

DialectPtr FindIn(DialectTable const& table, std::string_view name)
{
    for (DialectPtr const& d : table)
        if (d->name == name)
            return d;
    return nullptr;
}

struct Registry
{
    std::mutex lock;
    DialectTable table = BuiltInTable();
    std::string source = "built-in";
};

Registry& GetRegistry()
{
    static Registry registry;
    return registry;
}

void Publish(DialectTable table, std::string source)
{
    Registry& r = GetRegistry();
    std::lock_guard<std::mutex> guard(r.lock);
    r.table = std::move(table);
    r.source = std::move(source);
}

// The members a dialect file may set, by their names in SystemOneDialects.h. `name`, `kind`, `network` and
// `built` describe what this build can do, not the provider, so they are not overridable.
struct StringMember
{
    char const* name;
    std::string SystemOneDialect::*member;
};

struct BoolMember
{
    char const* name;
    bool SystemOneDialect::*member;
};

struct IntListMember
{
    char const* name;
    std::vector<int32> SystemOneDialect::*member;
};

struct StringListMember
{
    char const* name;
    std::vector<std::string> SystemOneDialect::*member;
};

StringMember const StringMembers[] = {
    {"defaultUrl", &SystemOneDialect::defaultUrl},
    {"defaultModel", &SystemOneDialect::defaultModel},
    {"authHeader", &SystemOneDialect::authHeader},
    {"authPrefix", &SystemOneDialect::authPrefix},
    {"fModel", &SystemOneDialect::fModel},
    {"fState", &SystemOneDialect::fState},
    {"fQuestions", &SystemOneDialect::fQuestions},
    {"fType", &SystemOneDialect::fType},
    {"fInstructions", &SystemOneDialect::fInstructions},
    {"fCriteria", &SystemOneDialect::fCriteria},
    {"typeChoice", &SystemOneDialect::typeChoice},
    {"typeYesNo", &SystemOneDialect::typeYesNo},
    {"typeScore", &SystemOneDialect::typeScore},
    {"keyTrue", &SystemOneDialect::keyTrue},
    {"keyFalse", &SystemOneDialect::keyFalse},
    {"fAnswers", &SystemOneDialect::fAnswers},
    {"fChoice", &SystemOneDialect::fChoice},
    {"fYesNo", &SystemOneDialect::fYesNo},
    {"fScore", &SystemOneDialect::fScore},
    {"fConfidence", &SystemOneDialect::fConfidence},
    {"fProbabilities", &SystemOneDialect::fProbabilities},
    {"fLegend", &SystemOneDialect::fLegend},
    {"fUsage", &SystemOneDialect::fUsage},
    {"fInputTokens", &SystemOneDialect::fInputTokens},
    {"fOutputTokens", &SystemOneDialect::fOutputTokens},
    {"fCost", &SystemOneDialect::fCost},
    {"fId", &SystemOneDialect::fId},
    {"fResponseModel", &SystemOneDialect::fResponseModel},
    {"fProvider", &SystemOneDialect::fProvider},
    {"fError", &SystemOneDialect::fError},
    {"fErrorMessage", &SystemOneDialect::fErrorMessage},
    {"fErrorCode", &SystemOneDialect::fErrorCode},
    {"fDetail", &SystemOneDialect::fDetail},
    {"fDetailMessage", &SystemOneDialect::fDetailMessage},
};

BoolMember const BoolMembers[] = {
    {"hosted", &SystemOneDialect::hosted},
    {"requiresKey", &SystemOneDialect::requiresKey},
    {"loopbackOnly", &SystemOneDialect::loopbackOnly},
    {"calibratedByDefault", &SystemOneDialect::calibratedByDefault},
};

IntListMember const IntListMembers[] = {
    {"exhaustedStatuses", &SystemOneDialect::exhaustedStatuses},
    {"badRequestStatuses", &SystemOneDialect::badRequestStatuses},
    {"authStatuses", &SystemOneDialect::authStatuses},
    {"misconfiguredStatuses", &SystemOneDialect::misconfiguredStatuses},
};

StringListMember const StringListMembers[] = {
    {"alternateUrls", &SystemOneDialect::alternateUrls},
    {"exhaustedMarkers", &SystemOneDialect::exhaustedMarkers},
    {"misconfiguredMarkers", &SystemOneDialect::misconfiguredMarkers},
};

std::string Lower(std::string text)
{
    for (char& c : text)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// Applies one member of the file to `d`. False, with `why`, when the name is unknown or the value has the wrong
// type (the member then keeps its value).
bool ApplyMember(SystemOneDialect& d, std::string const& key, BrainJsonValue const& value, std::string& why)
{
    for (StringMember const& m : StringMembers)
    {
        if (key != m.name)
            continue;
        if (!value.IsString())
        {
            why = "expects a string";
            return false;
        }
        d.*(m.member) = value.string;
        return true;
    }

    for (BoolMember const& m : BoolMembers)
    {
        if (key != m.name)
            continue;
        if (!value.IsBool())
        {
            why = "expects true or false";
            return false;
        }
        d.*(m.member) = value.boolean;
        return true;
    }

    for (IntListMember const& m : IntListMembers)
    {
        if (key != m.name)
            continue;
        if (!value.IsArray())
        {
            why = "expects an array of numbers";
            return false;
        }
        std::vector<int32> list;
        for (BrainJsonValue const& item : value.items)
        {
            if (!item.IsNumber() || !std::isfinite(item.number) || item.number < 0.0 || item.number > 999.0 ||
                item.number != std::floor(item.number))
            {
                why = "expects an array of HTTP statuses";
                return false;
            }
            list.push_back(int32(item.number));
        }
        d.*(m.member) = std::move(list);
        return true;
    }

    for (StringListMember const& m : StringListMembers)
    {
        if (key != m.name)
            continue;
        if (!value.IsArray())
        {
            why = "expects an array of strings";
            return false;
        }
        std::vector<std::string> list;
        for (BrainJsonValue const& item : value.items)
        {
            if (!item.IsString())
            {
                why = "expects an array of strings";
                return false;
            }
            list.push_back(item.string);
        }
        // Markers are searched in a lower-case copy of the error text.
        if (m.member == &SystemOneDialect::exhaustedMarkers || m.member == &SystemOneDialect::misconfiguredMarkers)
            for (std::string& marker : list)
                marker = Lower(marker);
        d.*(m.member) = std::move(list);
        return true;
    }

    why = "is not a dialect member that can be set";
    return false;
}

bool ReadFile(std::string const& path, std::string& text, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "cannot open it";
        return false;
    }

    text.assign(MaxDialectFileBytes + 1, '\0');
    in.read(text.data(), std::streamsize(text.size()));
    std::size_t const got = std::size_t(in.gcount());
    if (got > MaxDialectFileBytes)
    {
        error = "larger than 64 KiB";
        return false;
    }
    text.resize(got);
    return true;
}
}  // namespace

void SystemOneDialects::Reload(std::string const& file)
{
    if (file.empty())
    {
        Publish(BuiltInTable(), "built-in");
        return;
    }

    std::string text;
    std::string error;
    BrainJsonValue root;
    if (!ReadFile(file, text, error) || !BrainJson::Parse(text, root, error, MaxDialectFileBytes))
    {
        LOG_ERROR("playerbots.brain", "CoA brain: dialect file {} ignored ({}); using the compiled-in table", file,
                  error);
        Publish(BuiltInTable(), "built-in");
        return;
    }
    if (!root.IsObject())
    {
        LOG_ERROR("playerbots.brain", "CoA brain: dialect file {} ignored (not a JSON object); using the compiled-in "
                                      "table", file);
        Publish(BuiltInTable(), "built-in");
        return;
    }

    // Copies of the compiled-in entries, changed member by member; the pointers already handed out stay as they
    // were (an engine keeps its dialect until it is rebuilt).
    DialectTable table;
    std::vector<SystemOneDialect> edited;
    for (DialectPtr const& d : BuiltInTable())
        edited.push_back(*d);

    uint32 changed = 0;
    for (BrainJsonMember const& entry : root.members)
    {
        std::string const name = Lower(entry.key);
        auto it = std::find_if(edited.begin(), edited.end(),
                               [&name](SystemOneDialect const& d) { return d.name == name; });
        if (it == edited.end())
        {
            LOG_ERROR("playerbots.brain", "CoA brain: dialect file {}: unknown dialect '{}' ignored", file, entry.key);
            continue;
        }
        if (!entry.value.IsObject())
        {
            LOG_ERROR("playerbots.brain", "CoA brain: dialect file {}: '{}' is not an object, ignored", file,
                      entry.key);
            continue;
        }

        for (BrainJsonMember const& member : entry.value.members)
        {
            std::string why;
            if (ApplyMember(*it, member.key, member.value, why))
                ++changed;
            else
                LOG_ERROR("playerbots.brain", "CoA brain: dialect file {}: {}.{} {}, ignored", file, name, member.key,
                          why);
        }
    }

    for (SystemOneDialect& d : edited)
        table.push_back(std::make_shared<SystemOneDialect const>(std::move(d)));

    LOG_INFO("playerbots.brain", "CoA brain: dialect file {} loaded ({} members changed)", file, changed);
    Publish(std::move(table), file);
}

std::shared_ptr<SystemOneDialect const> SystemOneDialects::Find(std::string_view name)
{
    Registry& r = GetRegistry();
    std::lock_guard<std::mutex> guard(r.lock);
    return FindIn(r.table, name);
}

std::shared_ptr<SystemOneDialect const> SystemOneDialects::BuiltIn(std::string_view name)
{
    return FindIn(BuiltInTable(), name);
}

std::vector<std::string> SystemOneDialects::Names()
{
    Registry& r = GetRegistry();
    std::lock_guard<std::mutex> guard(r.lock);
    std::vector<std::string> names;
    names.reserve(r.table.size());
    for (DialectPtr const& d : r.table)
        names.push_back(d->name);
    return names;
}

std::string SystemOneDialects::Source()
{
    Registry& r = GetRegistry();
    std::lock_guard<std::mutex> guard(r.lock);
    return r.source;
}
