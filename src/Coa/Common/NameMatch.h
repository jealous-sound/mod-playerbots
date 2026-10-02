/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NAMEMATCH_H
#define PLAYERBOTS_NAMEMATCH_H

#include <string>
#include <string_view>
#include <vector>

// Whether a chat line speaks to someone by name, ignoring case and accents ("aelys", "AELYS" and "Aélys" all
// name Aëlys). Pure functions: any thread.
namespace NameMatch
{
// Lower-cases ASCII, folds the Latin-1 letters (U+00C0-U+00FF) and oe/ae ligatures to their base letters,
// keeps any other character as it is. Invalid UTF-8 bytes are dropped.
std::string Fold(std::string_view utf8);

// A chat line cut once into folded words, to test many names against it.
class Prepared
{
public:
    explicit Prepared(std::string_view text);

    // Whether the line names `foldedName` (a name already passed through Fold): one word equals it, or, for a
    // name of 6 characters or more, one word of 4 or more is its beginning ("aely" for "aelysandra"). Names
    // shorter than 3 characters never match. A stopword (function words, greetings, MMO jargon: "bon", "mais",
    // "elle", "tank"...) names no one: a bot called Bon is never named, and "elle" does not name Elleaan.
    bool Mentions(std::string_view foldedName) const;

    // Words split on everything that is neither an ASCII letter or digit nor a multi-byte character, folded.
    std::vector<std::string> const& Words() const { return words; }

    // The first word as written (not folded; ASCII letters and multi-byte characters, 3 bytes or more), when
    // the line starts with it and it is followed by ',' ':' a space or the end: "Aelys, tu viens ?" gives
    // "Aelys". Empty otherwise, and when that word is a stopword ("Bon, on y va", "Mais pourquoi"). Used to see
    // whether a line speaks to someone else.
    std::string const& Vocative() const { return vocative; }

private:
    std::vector<std::string> words;
    std::vector<bool> stopwords;  // per word: never a name (Mentions skips it)
    std::string vocative;
};

// One-off form of Prepared(text).Mentions(Fold(name)).
bool Mentions(std::string_view text, std::string_view name);
}  // namespace NameMatch

#endif
