/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COACHANNELS_H
#define PLAYERBOTS_COACHANNELS_H

class Channel;
class Player;

namespace CoaChannels
{
// True when `player` is on this very channel. Player::IsInChannel compares channel ids only, and on Conquest of
// Azeroth every "Zone - <place>" channel has the same id, so a player on one of them looks joined to all of them.
// Reads the player's own list of joined channels: world thread, while the map threads are idle.
bool IsOn(Player const* player, Channel const* channel);
}  // namespace CoaChannels

#endif
