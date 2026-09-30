/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaChannels.h"
#include "Channel.h"
#include "Player.h"
#include <algorithm>

namespace
{
// Player::m_channels, the channels Channel::JoinChannel and LeaveChannel keep for the player, is protected and has
// no getter (Channel::IsOn is private). A pointer to it formed in a derived class reads it; never instantiated.
struct JoinedChannels final : Player
{
    JoinedChannels() = delete;

    static auto const& Of(Player const* player) { return player->*(&JoinedChannels::m_channels); }
};
}  // namespace

bool CoaChannels::IsOn(Player const* player, Channel const* channel)
{
    if (!player || !channel)
        return false;
    auto const& joined = JoinedChannels::Of(player);
    return std::find(joined.begin(), joined.end(), channel) != joined.end();
}
