/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINJSON_H
#define PLAYERBOTS_BRAINJSON_H

#include "Define.h"
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// A small JSON tree of our own, so no Boost type leaves BrainJson.cpp: that file alone includes
// <boost/json/src.hpp> (header-only Boost.JSON, compiled once) and converts from and to boost::json::value.
// Pure values, no exceptions; any thread.

enum class BrainJsonKind : uint8
{
    Null = 0,
    Bool,
    Number,  // every number is held as a double (exact for integers up to 2^53)
    String,
    Array,
    Object
};

struct BrainJsonMember;

struct BrainJsonValue
{
    BrainJsonKind kind = BrainJsonKind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<BrainJsonValue> items;     // Array
    std::vector<BrainJsonMember> members;  // Object, in document order (keys unique: Set replaces)

    static BrainJsonValue MakeNull();
    static BrainJsonValue MakeBool(bool value);
    static BrainJsonValue MakeNumber(double value);
    static BrainJsonValue MakeString(std::string value);
    static BrainJsonValue MakeArray();
    static BrainJsonValue MakeObject();

    bool IsNull() const { return kind == BrainJsonKind::Null; }
    bool IsBool() const { return kind == BrainJsonKind::Bool; }
    bool IsNumber() const { return kind == BrainJsonKind::Number; }
    bool IsString() const { return kind == BrainJsonKind::String; }
    bool IsArray() const { return kind == BrainJsonKind::Array; }
    bool IsObject() const { return kind == BrainJsonKind::Object; }

    // Object: the member's value, null when absent or when this is not an object.
    BrainJsonValue const* Find(std::string_view key) const;

    // Typed reads of an object member, `def` when absent or of another kind.
    std::string GetString(std::string_view key, std::string const& def = std::string()) const;
    double GetNumber(std::string_view key, double def = 0.0) const;
    bool GetBool(std::string_view key, bool def = false) const;

    // Object: appends the member, or replaces the value of an existing key. Turns a Null into an Object.
    BrainJsonValue& Set(std::string key, BrainJsonValue value);

    // Array: appends. Turns a Null into an Array.
    BrainJsonValue& Push(BrainJsonValue value);
};

struct BrainJsonMember
{
    std::string key;
    BrainJsonValue value;
};

namespace BrainJson
{
constexpr std::size_t DefaultMaxBytes = 64 * 1024;
constexpr uint32 DefaultMaxDepth = 32;

// False when the module was built against a Boost older than 1.75 (no Boost.JSON): every Parse then fails and
// the brain refuses to start (NullBrain), with one error line.
bool Available();

// Strict RFC 8259 JSON (no comments, no trailing commas, valid UTF-8). False, with `error`, when `text` is longer
// than maxBytes, nested deeper than maxDepth, malformed, or when memory runs out. Never throws.
bool Parse(std::string_view text, BrainJsonValue& out, std::string& error, std::size_t maxBytes = DefaultMaxBytes,
           uint32 maxDepth = DefaultMaxDepth);

// Compact JSON, members in their order. Numbers that are whole and within 2^53 are written without a fraction.
std::string Serialize(BrainJsonValue const& value);

// Appends `text` as a quoted JSON string ('"', '\\' and control characters escaped; UTF-8 passed through).
// Does not need Boost: the status file and the logs use it.
void AppendString(std::string& out, std::string_view text);
}  // namespace BrainJson

#endif
