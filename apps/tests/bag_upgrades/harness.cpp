/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <set>
#include <string>
#include <sstream>
#include <vector>

using uint8 = std::uint8_t;
using int8 = std::int8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
constexpr uint8 INVENTORY_SLOT_BAG_0 = 255;
constexpr uint8 INVENTORY_SLOT_BAG_START = 19;
constexpr uint8 INVENTORY_SLOT_BAG_END = 23;
constexpr uint8 INVENTORY_SLOT_ITEM_START = 23;
constexpr uint8 INVENTORY_SLOT_ITEM_END = 39;
constexpr uint32 ITEM_CLASS_CONTAINER = 1;
constexpr uint32 ITEM_SUBCLASS_CONTAINER = 0;
constexpr uint32 ITERATE_ITEMS_IN_BAGS = 1;
constexpr uint32 EQUIP_ERR_OK = 0;

struct ItemTemplate
{
    uint32 Class = ITEM_CLASS_CONTAINER;
    uint32 SubClass = ITEM_SUBCLASS_CONTAINER;
    uint32 ContainerSlots = 0;
    uint32 InventoryType = 18;
};

struct Item
{
    ItemTemplate proto;
    ItemTemplate const* sharedTemplate = nullptr;
    uint8 bag = INVENTORY_SLOT_BAG_0;
    uint8 slot = 0;
    bool inTrade = false;
    bool usable = true;
    bool IsBag() const { return proto.ContainerSlots != 0; }
    bool IsInTrade() const { return inTrade; }
    ItemTemplate const* GetTemplate() const
    {
        return sharedTemplate ? sharedTemplate : &proto;
    }
    uint8 GetBagSlot() const { return bag; }
    uint8 GetSlot() const { return slot; }
    uint16 GetPos() const { return static_cast<uint16>((bag << 8) | slot); }
    uint32 GetBagSize() const { return GetTemplate()->ContainerSlots; }
};
using Bag = Item;

struct Player
{
    std::map<uint16, Item*> inventory;
    std::vector<Item*> received;
    uint32 upgrades = 0;
    bool denyEquipSwap = false;
    Item* GetItemByPos(uint8 bag, uint8 slot) const
    {
        auto const it = inventory.find(static_cast<uint16>((bag << 8) | slot));
        return it == inventory.end() ? nullptr : it->second;
    }
    void Put(Item& item, uint8 bag, uint8 slot)
    {
        item.bag = bag;
        item.slot = slot;
        inventory[item.GetPos()] = &item;
    }
    uint32 CanEquipItem(uint8 slot, uint16& destination, Item* item, bool) const
    {
        destination = static_cast<uint16>((INVENTORY_SLOT_BAG_0 << 8) | slot);
        return item->usable ? EQUIP_ERR_OK : 1;
    }
    void SwapItem(uint16 source, uint16 destination)
    {
        Item* item = inventory[source];
        Item* old = inventory[destination];
        uint8 const slot = static_cast<uint8>(destination);
        bool const equipping = slot >= INVENTORY_SLOT_BAG_START && slot < INVENTORY_SLOT_BAG_END;
        if (equipping && denyEquipSwap)
            return;
        if (equipping && item->bag == slot)
            throw std::runtime_error("attempted to equip a bag from inside itself");
        if (equipping && old && old->GetBagSize() >= item->GetBagSize())
            throw std::runtime_error("attempted to downgrade a bag");
        if (equipping)
            ++upgrades;
        inventory.erase(source);
        inventory.erase(destination);
        if (old)
            Put(*old, static_cast<uint8>(source >> 8), static_cast<uint8>(source));
        Put(*item, static_cast<uint8>(destination >> 8), slot);
    }
};

struct CollectItemsVisitor { std::vector<Item*> items; };
struct ChatHelper
{
    std::string FormatItem(ItemTemplate const*, uint32 count = 0)
    {
        return "[Bag]x" + std::to_string(count);
    }
} chatHelper;
struct BotAI
{
    std::vector<std::string> messages;
    void TellMaster(std::string const& text) { messages.push_back(text); }
} testAI;
struct PlayerbotTextMgr
{
    static PlayerbotTextMgr& instance()
    {
        static PlayerbotTextMgr manager;
        return manager;
    }
    std::string GetBotTextOrDefault(std::string const& name,
        std::string text, std::map<std::string, std::string> const& placeholders)
    {
        if (name != "equip_command" || text != "Equipping %item")
            throw std::runtime_error("reuse the existing localized equipment text");
        text.replace(text.find("%item"), 5, placeholders.at("%item"));
        return text;
    }
};
struct ItemUsageValue
{
    Player* bot;
    uint32 GetSmallestBagSize();
};
struct EquipAction
{
    Player* bot;
    ChatHelper* chat = &chatHelper;
    BotAI* botAI = &testAI;
    uint8 GetSmallestBagSlot();
    void EquipBagUpgrades();
    std::set<uint32> SelectInventoryItemsToEquip() { return {}; }
    void EquipItems(std::set<uint32>) { }
    void IterateItems(CollectItemsVisitor* visitor, uint32) { visitor->items = bot->received; }
};

using ItemIds = std::set<uint32>;
using ObjectGuid = std::uint64_t;
constexpr uint32 TRADE_STATUS_TRADE_ACCEPT = 4;
constexpr uint32 TRADE_STATUS_TRADE_COMPLETE = 8;
constexpr uint32 INVTYPE_NON_EQUIP = 0;

struct WorldPacket
{
    std::vector<std::uint64_t> fields;
    std::size_t cursor = 0;
    void rpos(std::size_t position) { cursor = position; }
    template <typename T> WorldPacket& operator>>(T& value)
    {
        value = static_cast<T>(fields.at(cursor++));
        return *this;
    }
};
struct Event
{
    std::string source;
    WorldPacket packet;
    std::string GetSource() const { return source; }
    WorldPacket getPacket() const { return packet; }
};
struct EquipUpgradesPacketAction : EquipAction
{
    bool Execute(Event event);
};
struct EquipUpgradeAction : EquipAction
{
    bool Execute(Event event);
};
struct BotConfig { bool autoEquipUpgradeLoot = true; } sPlayerbotAIConfig;
struct RandomBots
{
    bool random = false;
    bool IsRandomBot(Player*) const { return random; }
} sRandomPlayerbotMgr;
struct ObjectMgr
{
    ItemTemplate item;
    ItemTemplate const* GetItemTemplate(uint32) const { return &item; }
} objectMgr;
ObjectMgr* sObjectMgr = &objectMgr;

// ACTUAL_METHODS

void Check(bool condition, char const* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

int main()
{
    try
    {
        Player bot;
        std::array<Item, 4> equipped;
        for (uint8 index = 0; index < equipped.size(); ++index)
        {
            equipped[index].proto.ContainerSlots = index == 0 ? 6 : 16;
            bot.Put(equipped[index], INVENTORY_SLOT_BAG_0, INVENTORY_SLOT_BAG_START + index);
        }
        ItemUsageValue usage{&bot};
        Check(usage.GetSmallestBagSize() == 6, "first bag slot must be considered (6,16,16,16)");
        bot.inventory.erase(equipped[0].GetPos());
        Check(usage.GetSmallestBagSize() == 0, "empty first bag slot must be filled");
        bot.Put(equipped[0], INVENTORY_SLOT_BAG_0, INVENTORY_SLOT_BAG_START);
        std::cout << "PASS: first slot and empty first slot upgrade detection\n";

#ifdef HAS_BAG_UPGRADES
        EquipAction action{&bot};
        std::array<Item, 4> received;
        for (uint8 index = 0; index < received.size(); ++index)
        {
            received[index].proto.ContainerSlots = 20;
            received[index].sharedTemplate = &received[0].proto;
            bot.Put(received[index], INVENTORY_SLOT_BAG_0, INVENTORY_SLOT_ITEM_START + index);
            bot.received.push_back(&received[index]);
        }
        EquipUpgradesPacketAction automatic;
        automatic.bot = &bot;
        Check(!automatic.Execute({"trade status", {{TRADE_STATUS_TRADE_ACCEPT}}}),
            "accepting a trade must not trigger equipment changes");
        Check(bot.upgrades == 0, "bags must not move before trade completion");
        sPlayerbotAIConfig.autoEquipUpgradeLoot = false;
        Check(!automatic.Execute({"trade status", {{TRADE_STATUS_TRADE_COMPLETE}}}),
            "disabled automatic upgrades must remain disabled for alt bots");
        Check(bot.upgrades == 0, "disabled auto-equip must not move bags");
        sPlayerbotAIConfig.autoEquipUpgradeLoot = true;
        Check(automatic.Execute({"trade status", {{TRADE_STATUS_TRADE_COMPLETE}}}),
            "completed trades must trigger upgrades");
        Check(bot.upgrades == 4, "all four identical new bags must equip in one pass");
        Check(usage.GetSmallestBagSize() == 20, "all four slots must have larger bags");
        Check(testAI.messages.size() == 1,
            "a successful batch must send one equipment notification");
        Check(testAI.messages.back() == "Equipping [Bag]x4",
            "equipment notification must group identical bags with their count");

        bot.received.clear();
        for (auto& item : equipped)
            bot.received.push_back(&item);
        action.EquipBagUpgrades();
        Check(bot.upgrades == 4, "old bags must not downgrade the equipped bags");

        Item restricted;
        restricted.proto.ContainerSlots = 24;
        restricted.usable = false;
        bot.Put(restricted, INVENTORY_SLOT_BAG_0, 30);
        bot.received = {&restricted};
        action.EquipBagUpgrades();
        Check(bot.upgrades == 4, "unusable bags must remain in inventory");
        restricted.usable = true;
        restricted.inTrade = true;
        action.EquipBagUpgrades();
        Check(bot.upgrades == 4, "bags offered in a trade must not move");
        restricted.inTrade = false;
        restricted.proto.SubClass = 1;
        action.EquipBagUpgrades();
        Check(bot.upgrades == 4, "specialized bags must not auto-equip");
        restricted.proto.SubClass = 0;
        restricted.proto.ContainerSlots = 20;
        action.EquipBagUpgrades();
        Check(bot.upgrades == 4, "equal-size bags must not churn");
        Check(testAI.messages.size() == 1,
            "skipped bags must not produce success notifications");

        restricted.proto.ContainerSlots = 24;
        bot.denyEquipSwap = true;
        action.EquipBagUpgrades();
        Check(bot.upgrades == 4 && testAI.messages.size() == 1,
            "a rejected native swap must not produce a success notification");
        bot.denyEquipSwap = false;

        restricted.proto.ContainerSlots = 24;
        bot.inventory.erase(restricted.GetPos());
        uint8 const target = action.GetSmallestBagSlot();
        bot.Put(restricted, target, 0);
        EquipUpgradeAction manual;
        manual.bot = &bot;
        Check(manual.Execute({}), "manual equip upgrade must process existing bags");
        Check(bot.upgrades == 5, "bag inside the replaced bag must move via a free backpack slot");
        Check(restricted.GetSlot() == target && restricted.GetBagSlot() == INVENTORY_SLOT_BAG_0,
            "bag must end up equipped");

        restricted.proto.ContainerSlots = 28;
        bot.inventory.erase(restricted.GetPos());
        uint8 const emptySlot = restricted.GetSlot();
        Item blocked;
        blocked.proto.ContainerSlots = 32;
        bot.Put(restricted, INVENTORY_SLOT_BAG_0, emptySlot);
        uint8 const blockedSlot = action.GetSmallestBagSlot();
        bot.Put(blocked, blockedSlot, 1);
        std::array<Item, 16> filler;
        for (uint8 index = 0; index < filler.size(); ++index)
            bot.Put(filler[index], INVENTORY_SLOT_BAG_0, INVENTORY_SLOT_ITEM_START + index);
        bot.received = {&blocked};
        action.EquipBagUpgrades();
        Check(bot.upgrades == 5 && blocked.GetBagSlot() == blockedSlot,
            "full backpack must leave self-contained upgrade untouched");
        bot.inventory.erase(static_cast<uint16>((INVENTORY_SLOT_BAG_0 << 8) | 38));
        Check(automatic.Execute({"item push result", {{1, 1, 0, 1, 0, 38, 1}}}),
            "receiving an equippable item must trigger inventory upgrades");
        Check(bot.upgrades == 6, "item push must upgrade an existing bag");
        Check(testAI.messages.size() == 3,
            "only the two further successful passes should notify");
        std::cout << "PASS: duplicate bags, no downgrades, restrictions and staging\n";
        std::cout << "PASS: trade completion, auto-equip config, item push and manual command\n";
        std::cout << "PASS: one localized notice per successful pass, none on failed swaps\n";
#else
        throw std::runtime_error("inventory-wide bag upgrade pass is absent");
#endif
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
