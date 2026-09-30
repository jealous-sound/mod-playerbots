/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaVoiceSanitizer.h"
#include "CoaChatIntent.h"
#include "CoaText.h"
#include "NameMatch.h"
#include <array>
#include <string_view>
#include <utility>

namespace
{
using Words = std::vector<std::string>;

constexpr std::size_t MAX_LABEL_BYTES = 24;

bool StartsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void Trim(std::string& text)
{
    std::size_t const first = text.find_first_not_of(' ');
    if (first == std::string::npos)
    {
        text.clear();
        return;
    }
    text.erase(text.find_last_not_of(' ') + 1);
    text.erase(0, first);
}

// Runs of spaces become one space; no space is left at either end.
void CollapseSpaces(std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (char const c : text)
    {
        if (c == ' ' && (out.empty() || out.back() == ' '))
            continue;
        out += c;
    }
    if (!out.empty() && out.back() == ' ')
        out.pop_back();
    text.swap(out);
}

// Tabs become spaces. Dropped: other ASCII control bytes and DEL, the C1 controls (U+0080-U+009F), invalid
// UTF-8 (overlongs, surrogates, stray continuation bytes, cut sequences) and every 4-byte character, which the
// utf8mb3 texts table cannot hold and the 3.3.5 client cannot draw.
std::string CleanBytes(std::string_view in)
{
    std::string out;
    out.reserve(in.size());

    std::size_t i = 0;
    while (i < in.size())
    {
        unsigned char const lead = static_cast<unsigned char>(in[i]);
        if (lead < 0x80)
        {
            if (lead == '\t')
                out += ' ';
            else if (lead >= 0x20 && lead != 0x7F)
                out += char(lead);
            ++i;
            continue;
        }

        // Length and the allowed range of the second byte, per RFC 3629.
        std::size_t length = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF)
            length = 2;
        else if (lead == 0xE0)
        {
            length = 3;
            low = 0xA0;
        }
        else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF)
            length = 3;
        else if (lead == 0xED)
        {
            length = 3;
            high = 0x9F;
        }
        else if (lead == 0xF0)
        {
            length = 4;
            low = 0x90;
        }
        else if (lead >= 0xF1 && lead <= 0xF3)
            length = 4;
        else if (lead == 0xF4)
        {
            length = 4;
            high = 0x8F;
        }
        else
        {
            ++i;  // a continuation byte without a lead, or a byte that never starts a character
            continue;
        }

        bool valid = i + length <= in.size();
        for (std::size_t k = 1; valid && k < length; ++k)
        {
            unsigned char const next = static_cast<unsigned char>(in[i + k]);
            valid = k == 1 ? (next >= low && next <= high) : (next & 0xC0) == 0x80;
        }
        if (!valid)
        {
            ++i;  // the lead goes; its continuation bytes are dropped one by one above
            continue;
        }

        bool const c1Control = lead == 0xC2 && static_cast<unsigned char>(in[i + 1]) < 0xA0;
        if (length < 4 && !c1Control)
            out.append(in.substr(i, length));
        i += length;
    }

    return out;
}

// One pair of wrapping quotes: "..." « ... » “...”, only when the closing quote does not also appear inside.
bool StripWrappingQuotes(std::string& text)
{
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 3> QUOTES = {{
        {"\"", "\""},
        {"\xC2\xAB", "\xC2\xBB"},          // « »
        {"\xE2\x80\x9C", "\xE2\x80\x9D"},  // “ ”
    }};

    for (auto const& [open, close] : QUOTES)
    {
        if (text.size() < open.size() + close.size() || !StartsWith(text, open) || !EndsWith(text, close))
            continue;
        if (text.find(close, open.size()) != text.size() - close.size())
            continue;

        text = text.substr(open.size(), text.size() - open.size() - close.size());
        Trim(text);
        return true;
    }
    return false;
}

bool IsWordByte(char c)
{
    unsigned char const b = static_cast<unsigned char>(c);
    return b >= 0x80 || (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9');
}

bool IsLabelByte(char c) { return IsWordByte(c) || c == '-' || c == '\''; }

// "Aelys: salut" or "Aelys : salut" -> "salut". The label is one word of at most 24 bytes, and the ':' must be
// followed by a space or the end, so "12:30" and "https://" are left alone.
void StripLeadingLabel(std::string& text)
{
    std::size_t end = 0;
    while (end < text.size() && end <= MAX_LABEL_BYTES && IsLabelByte(text[end]))
        ++end;
    if (end == 0 || end > MAX_LABEL_BYTES)
        return;

    std::size_t colon = end;
    if (colon < text.size() && text[colon] == ' ')
        ++colon;
    if (colon >= text.size() || text[colon] != ':')
        return;
    if (colon + 1 < text.size() && text[colon + 1] != ' ')
        return;

    text.erase(0, colon + 1);
    Trim(text);
}

// Markdown emphasis and code marks anywhere; heading and quote marks at the start.
void StripMarkdown(std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (char const c : text)
        if (c != '*' && c != '_' && c != '`')
            out += c;

    std::size_t const start = out.find_first_not_of("#> ");
    if (start == std::string::npos)
        out.clear();
    else
        out.erase(0, start);

    text.swap(out);
    CollapseSpaces(text);
}

bool HasFence(std::string_view text)
{
    return text.find("<<<") != std::string_view::npos || text.find(">>>") != std::string_view::npos ||
           text.find("<|") != std::string_view::npos;
}

bool HasUrl(std::string const& folded)
{
    return folded.find("http://") != std::string::npos || folded.find("https://") != std::string::npos ||
           folded.find("www.") != std::string::npos;
}

// The next word of `text` from `pos`, split like NameMatch::Prepared (on every ASCII byte that is not a letter or
// a digit). False at the end.
bool NextWord(std::string_view text, std::size_t& pos, std::string_view& word)
{
    while (pos < text.size() && !IsWordByte(text[pos]))
        ++pos;
    if (pos >= text.size())
        return false;

    std::size_t const start = pos;
    while (pos < text.size() && IsWordByte(text[pos]))
        ++pos;
    word = text.substr(start, pos - start);
    return true;
}

// Whether `words` holds the words of `folded` (already folded) one after the other. No allocation: it runs for
// every blocked word on every candidate line.
bool ContainsPhrase(Words const& words, std::string_view folded)
{
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        std::size_t pos = 0;
        std::size_t k = i;
        std::string_view word;
        bool any = false;
        bool match = true;
        while (NextWord(folded, pos, word))
        {
            any = true;
            if (k >= words.size() || words[k] != word)
            {
                match = false;
                break;
            }
            ++k;
        }
        if (!any)
            return false;
        if (match)
            return true;
    }
    return false;
}

// Talk about being an AI, or model and provider names, as folded words.
bool HasAiTalk(Words const& words)
{
    static constexpr std::string_view AI_WORDS[] = {"as an ai", "en tant qu ia", "modele de langage", "language model"};
    static constexpr std::string_view NAMES[] = {"openai", "anthropic", "claude", "typesafe", "jev", "prompt"};

    for (std::string_view const phrase : AI_WORDS)
        if (ContainsPhrase(words, phrase))
            return true;
    for (std::string_view const phrase : NAMES)
        if (ContainsPhrase(words, phrase))
            return true;
    return false;
}

bool HasBlockedWord(Words const& words, std::vector<std::string> const& blocked)
{
    for (std::string const& entry : blocked)
        if (ContainsPhrase(words, entry))
            return true;
    return false;
}

void EscapePipes(std::string& text)
{
    if (text.find('|') == std::string::npos)
        return;

    std::string out;
    out.reserve(text.size() + 8);
    for (char const c : text)
    {
        out += c;
        if (c == '|')
            out += '|';
    }
    text.swap(out);
}

bool Reject(CoaVoiceReject* reason, CoaVoiceReject why)
{
    if (reason)
        *reason = why;
    return false;
}
}  // namespace

bool CoaVoiceSanitizer::Clean(std::string& line, CoaVoiceSanitizeOptions const& options, CoaVoiceReject* reason)
{
    std::size_t const lineEnd = line.find_first_of("\r\n");
    if (lineEnd != std::string::npos)
        line.resize(lineEnd);

    line = CleanBytes(line);
    CollapseSpaces(line);
    if (line.empty())
        return Reject(reason, CoaVoiceReject::Empty);

    if (options.untrusted)
    {
        // Fences are tested before and after the stripping: a leading ">>>" would otherwise be taken for a quote mark.
        if (HasFence(line))
            return Reject(reason, CoaVoiceReject::Fence);

        StripMarkdown(line);
        bool const quoted = StripWrappingQuotes(line);
        StripLeadingLabel(line);
        if (!quoted)
            StripWrappingQuotes(line);

        if (HasFence(line))
            return Reject(reason, CoaVoiceReject::Fence);
        if (HasUrl(NameMatch::Fold(line)))
            return Reject(reason, CoaVoiceReject::Url);
    }

    // One split into words serves the AI-talk and blocked-word tests; escaping '|' does not change the words.
    bool const hasBlocked = options.blockedWords && !options.blockedWords->empty();
    if (options.untrusted || hasBlocked)
    {
        NameMatch::Prepared const prepared(line);
        if (options.untrusted && HasAiTalk(prepared.Words()))
            return Reject(reason, CoaVoiceReject::AiTalk);
        if (options.untrusted && CoaChatIntent::LooksLikeCommand(line))
            return Reject(reason, CoaVoiceReject::Command);
        if (hasBlocked && HasBlockedWord(prepared.Words(), *options.blockedWords))
            return Reject(reason, CoaVoiceReject::Blocked);
    }

    if (options.untrusted)
        EscapePipes(line);

    CoaText::CutUtf8Bytes(line, options.maxBytes);
    Trim(line);
    if (options.untrusted)
    {
        // The cut may have split the last "||": a lone '|' at the end would start an escape sequence.
        std::size_t const lastNonPipe = line.find_last_not_of('|');
        std::size_t const pipes = line.size() - (lastNonPipe == std::string::npos ? 0 : lastNonPipe + 1);
        if (pipes % 2)
            line.pop_back();
        Trim(line);
    }

    if (line.empty())
        return Reject(reason, CoaVoiceReject::Empty);

    if (reason)
        *reason = CoaVoiceReject::None;
    return true;
}
