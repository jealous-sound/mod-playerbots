/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainJson.h"
#include <boost/version.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <utility>

// The only translation unit that compiles Boost.JSON (header-only). The auto-link macros keep MSVC from asking
// for a boost_json/boost_container library that the core does not build.
#if BOOST_VERSION >= 107500
#if !defined(BOOST_JSON_NO_LIB)
#define BOOST_JSON_NO_LIB
#endif
#if !defined(BOOST_CONTAINER_NO_LIB)
#define BOOST_CONTAINER_NO_LIB
#endif
#include <boost/json/src.hpp>
#define COA_BRAIN_HAS_BOOST_JSON 1
#else
#define COA_BRAIN_HAS_BOOST_JSON 0
#endif

namespace
{
#if COA_BRAIN_HAS_BOOST_JSON
void Convert(boost::json::value const& in, BrainJsonValue& out)
{
    switch (in.kind())
    {
        case boost::json::kind::null:
            out.kind = BrainJsonKind::Null;
            break;
        case boost::json::kind::bool_:
            out.kind = BrainJsonKind::Bool;
            out.boolean = in.get_bool();
            break;
        case boost::json::kind::int64:
            out.kind = BrainJsonKind::Number;
            out.number = double(in.get_int64());
            break;
        case boost::json::kind::uint64:
            out.kind = BrainJsonKind::Number;
            out.number = double(in.get_uint64());
            break;
        case boost::json::kind::double_:
            out.kind = BrainJsonKind::Number;
            out.number = in.get_double();
            break;
        case boost::json::kind::string:
        {
            boost::json::string const& s = in.get_string();
            out.kind = BrainJsonKind::String;
            out.string.assign(s.data(), s.size());
            break;
        }
        case boost::json::kind::array:
        {
            boost::json::array const& a = in.get_array();
            out.kind = BrainJsonKind::Array;
            out.items.resize(a.size());
            for (std::size_t i = 0; i < a.size(); ++i)
                Convert(a[i], out.items[i]);
            break;
        }
        case boost::json::kind::object:
        {
            // Boost.JSON already keeps one member per key (the last value), so members are appended as they are.
            boost::json::object const& o = in.get_object();
            out.kind = BrainJsonKind::Object;
            out.members.resize(o.size());
            std::size_t i = 0;
            for (auto const& kv : o)
            {
                BrainJsonMember& member = out.members[i++];
                member.key.assign(kv.key().data(), kv.key().size());
                Convert(kv.value(), member.value);
            }
            break;
        }
    }
}
#endif

// Shortest of %.15g, %.16g, %.17g that reads back to the same double. snprintf and strtod follow the same
// C locale, so the round trip holds even under a comma locale; the comma is then turned into a point.
void AppendNumber(std::string& out, double value)
{
    if (!std::isfinite(value))
    {
        out += "null";
        return;
    }

    constexpr double exactLimit = 9007199254740992.0;  // 2^53
    if (value == std::floor(value) && std::fabs(value) <= exactLimit)
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
        out += buffer;
        return;
    }

    char buffer[40];
    for (int precision = 15; precision <= 17; ++precision)
    {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        if (precision == 17 || std::strtod(buffer, nullptr) == value)
            break;
    }

    for (char* c = buffer; *c; ++c)
        if (*c == ',')
            *c = '.';
    out += buffer;
}

void AppendValue(std::string& out, BrainJsonValue const& value)
{
    switch (value.kind)
    {
        case BrainJsonKind::Null:
            out += "null";
            break;
        case BrainJsonKind::Bool:
            out += value.boolean ? "true" : "false";
            break;
        case BrainJsonKind::Number:
            AppendNumber(out, value.number);
            break;
        case BrainJsonKind::String:
            BrainJson::AppendString(out, value.string);
            break;
        case BrainJsonKind::Array:
        {
            out += '[';
            bool first = true;
            for (BrainJsonValue const& item : value.items)
            {
                if (!first)
                    out += ',';
                first = false;
                AppendValue(out, item);
            }
            out += ']';
            break;
        }
        case BrainJsonKind::Object:
        {
            out += '{';
            bool first = true;
            for (BrainJsonMember const& member : value.members)
            {
                if (!first)
                    out += ',';
                first = false;
                BrainJson::AppendString(out, member.key);
                out += ':';
                AppendValue(out, member.value);
            }
            out += '}';
            break;
        }
    }
}
}  // namespace

BrainJsonValue BrainJsonValue::MakeNull() { return BrainJsonValue(); }

BrainJsonValue BrainJsonValue::MakeBool(bool value)
{
    BrainJsonValue v;
    v.kind = BrainJsonKind::Bool;
    v.boolean = value;
    return v;
}

BrainJsonValue BrainJsonValue::MakeNumber(double value)
{
    BrainJsonValue v;
    v.kind = BrainJsonKind::Number;
    v.number = value;
    return v;
}

BrainJsonValue BrainJsonValue::MakeString(std::string value)
{
    BrainJsonValue v;
    v.kind = BrainJsonKind::String;
    v.string = std::move(value);
    return v;
}

BrainJsonValue BrainJsonValue::MakeArray()
{
    BrainJsonValue v;
    v.kind = BrainJsonKind::Array;
    return v;
}

BrainJsonValue BrainJsonValue::MakeObject()
{
    BrainJsonValue v;
    v.kind = BrainJsonKind::Object;
    return v;
}

BrainJsonValue const* BrainJsonValue::Find(std::string_view key) const
{
    if (kind != BrainJsonKind::Object)
        return nullptr;

    for (BrainJsonMember const& member : members)
        if (member.key == key)
            return &member.value;

    return nullptr;
}

std::string BrainJsonValue::GetString(std::string_view key, std::string const& def) const
{
    BrainJsonValue const* v = Find(key);
    return v && v->IsString() ? v->string : def;
}

double BrainJsonValue::GetNumber(std::string_view key, double def) const
{
    BrainJsonValue const* v = Find(key);
    return v && v->IsNumber() ? v->number : def;
}

bool BrainJsonValue::GetBool(std::string_view key, bool def) const
{
    BrainJsonValue const* v = Find(key);
    return v && v->IsBool() ? v->boolean : def;
}

BrainJsonValue& BrainJsonValue::Set(std::string key, BrainJsonValue value)
{
    if (kind == BrainJsonKind::Null)
        kind = BrainJsonKind::Object;

    for (BrainJsonMember& member : members)
    {
        if (member.key == key)
        {
            member.value = std::move(value);
            return member.value;
        }
    }

    members.push_back(BrainJsonMember{std::move(key), std::move(value)});
    return members.back().value;
}

BrainJsonValue& BrainJsonValue::Push(BrainJsonValue value)
{
    if (kind == BrainJsonKind::Null)
        kind = BrainJsonKind::Array;

    items.push_back(std::move(value));
    return items.back();
}

bool BrainJson::Available() { return COA_BRAIN_HAS_BOOST_JSON != 0; }

bool BrainJson::Parse(std::string_view text, BrainJsonValue& out, std::string& error, std::size_t maxBytes,
                      uint32 maxDepth)
{
    out = BrainJsonValue();
    error.clear();

#if COA_BRAIN_HAS_BOOST_JSON
    if (text.size() > maxBytes)
    {
        error = "JSON too large (" + std::to_string(text.size()) + " bytes, limit " + std::to_string(maxBytes) + ")";
        return false;
    }

    try
    {
        boost::json::parse_options options;
        options.max_depth = maxDepth ? maxDepth : 1;

        boost::system::error_code ec;
        boost::json::value parsed =
            boost::json::parse(boost::json::string_view(text.data(), text.size()), ec, boost::json::storage_ptr(),
                               options);
        if (ec)
        {
            error = "bad JSON: " + ec.message();
            return false;
        }

        Convert(parsed, out);
        return true;
    }
    catch (std::bad_alloc const&)
    {
        out = BrainJsonValue();
        error = "out of memory while parsing JSON";
    }
    catch (std::exception const& e)
    {
        out = BrainJsonValue();
        error = std::string("JSON parser failure: ") + e.what();
    }
    catch (...)
    {
        out = BrainJsonValue();
        error = "JSON parser failure";
    }
    return false;
#else
    (void)text;
    (void)maxBytes;
    (void)maxDepth;
    error = "Boost.JSON unavailable";
    return false;
#endif
}

std::string BrainJson::Serialize(BrainJsonValue const& value)
{
    std::string out;
    AppendValue(out, value);
    return out;
}

void BrainJson::AppendString(std::string& out, std::string_view text)
{
    static char const hex[] = "0123456789abcdef";

    out += '"';
    for (char c : text)
    {
        unsigned char const u = static_cast<unsigned char>(c);
        switch (c)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (u < 0x20)
                {
                    out += "\\u00";
                    out += hex[u >> 4];
                    out += hex[u & 0x0F];
                }
                else
                    out += c;
                break;
        }
    }
    out += '"';
}
