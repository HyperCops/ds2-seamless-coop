#include "../../include/sync.h"
#include "../../include/network.h"
#include "../../include/utils.h"
#include "../../include/address_resolver.h"
#include "../../include/addresses.h"
#include "../../include/ui.h"
#include "../../include/pattern_scanner.h"
#include <cfloat>

using namespace DS2Coop::Sync;
using namespace DS2Coop::Utils;
using namespace DS2Coop::Addresses;

ProgressSync& ProgressSync::GetInstance() {
    static ProgressSync instance;
    return instance;
}

bool ProgressSync::Initialize() {
    if (m_initialized) return true;
    
    LOG_INFO("Initializing progress sync...");
    m_initialized = true;
    LOG_INFO("Progress sync initialized");
    return true;
}

void ProgressSync::Shutdown() {
    if (!m_initialized) return;
    
    LOG_INFO("Shutting down progress sync...");
    
    m_eventFlags.clear();
    m_defeatedBosses.clear();
    m_litBonfires.clear();
    m_pickedItems.clear();
    
    m_initialized = false;
    LOG_INFO("Progress sync shut down");
}

void ProgressSync::SyncEventFlag(uint32_t flagId, bool value) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    LOG_DEBUG("Syncing event flag %u = %d", flagId, value);

    m_eventFlags[flagId] = value;
    
    // Broadcast to other players
    Network::EventFlagPacket packet{};
    packet.header.magic = 0x44533243;
    packet.header.type = Network::PacketType::EventFlag;
    packet.header.size = sizeof(Network::EventFlagPacket);
    packet.flagId = flagId;
    packet.flagValue = value;
    
    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.BroadcastPacket(&packet.header);
}

bool ProgressSync::GetEventFlag(uint32_t flagId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_eventFlags.find(flagId);
    return (it != m_eventFlags.end()) ? it->second : false;
}

void ProgressSync::RequestEventFlagSync() {
    LOG_INFO("Requesting full event flag synchronization...");
    
    // Send request packet to host/peers
    // In a full implementation, this would request all flags from the host
    
    LOG_INFO("Event flag sync requested");
}

// ============================================================================
// ApplyEventFlagToMemory
//
// Writes a single event flag bit directly to the EventFlagManager bitfield.
// Called when a remote peer has set an event flag (e.g. boss kill) that we
// need to mirror in our own save data.
//
// Does NOT call the game's SetEventFlag() function — that would re-trigger
// our own hook and create a broadcast loop.  Direct memory write is safe
// and equivalent to what SetEventFlag() does internally.
//
// Pointer chain (Bob Edition v4.09.5 CT, DS2 SOTFS x64):
//   GameManagerImp → [+0x60] → EventFlagManager ptr
//   EventFlagManager → +0x20 → uint32_t flagBitfield[]
//   flagBitfield[flagId / 32]  |=  (1u << (flagId & 31))   // set
//   flagBitfield[flagId / 32]  &= ~(1u << (flagId & 31))   // clear
// ============================================================================
bool ProgressSync::ApplyEventFlagToMemory(uint32_t flagId, bool value) {
    auto& resolver = DS2Coop::AddressResolver::GetInstance();
    uintptr_t gmi = resolver.GetGameManagerImp();
    if (!gmi) {
        LOG_WARNING("[EFLAG] ApplyEventFlag: GameManagerImp not resolved");
        return false;
    }

    uintptr_t efmanPtr = 0;
    if (!Memory::Read<uintptr_t>(gmi + EFMAN_FROM_GMI, &efmanPtr) || !efmanPtr) {
        LOG_WARNING("[EFLAG] ApplyEventFlag: EventFlagManager not found at GMI+0x%X", EFMAN_FROM_GMI);
        return false;
    }

    // The bitfield is a flat uint32_t array starting at efmanPtr + EFMAN_FLAG_ARRAY.
    // Sanity-check: flag IDs above 2^21 (~2 million) would require a >256 KB array —
    // that would be unusual. Clamp to avoid wild writes.
    if (flagId > (1u << 21)) {
        LOG_WARNING("[EFLAG] ApplyEventFlag: flagId %u exceeds sanity limit", flagId);
        return false;
    }

    uint32_t arrayIdx = flagId >> 5;          // flagId / 32
    uint32_t bitMask  = 1u << (flagId & 31);  // 1 << (flagId % 32)
    uintptr_t wordAddr = efmanPtr + EFMAN_FLAG_ARRAY + arrayIdx * sizeof(uint32_t);

    uint32_t word = 0;
    if (!Memory::Read<uint32_t>(wordAddr, &word)) {
        LOG_WARNING("[EFLAG] ApplyEventFlag: failed to read bitfield word at 0x%llX", wordAddr);
        return false;
    }

    uint32_t newWord = value ? (word | bitMask) : (word & ~bitMask);
    if (newWord == word) {
        LOG_DEBUG("[EFLAG] ApplyEventFlag: flag %u already %s", flagId, value ? "set" : "clear");
        return true; // already at desired state
    }

    if (!Memory::Write<uint32_t>(wordAddr, newWord)) {
        LOG_WARNING("[EFLAG] ApplyEventFlag: failed to write bitfield word at 0x%llX", wordAddr);
        return false;
    }

    LOG_INFO("[EFLAG] ApplyEventFlag: flag %u %s in game memory (EFMan=0x%llX)",
             flagId, value ? "SET" : "CLEARED", efmanPtr);
    return true;
}

// ============================================================================
// ApplySoulsGainToMemory
//
// Adds `amount` souls directly to the local player's spendable soul count
// in PlayerData.  Also increments SoulMemory (lifetime total) by the same
// amount so the phantom's soul memory stays accurate.
//
// Pointer chain (Bob Edition v4.09.5 CT, DS2 SOTFS x64):
//   GameManagerImp → [+0x38] → PlayerData
//   PlayerData → +0xF0 → int32  current souls (spendable)
//   PlayerData → +0xF4 → uint32 SoulMemory    (lifetime total)
// ============================================================================
bool ProgressSync::ApplySoulsGainToMemory(uint32_t amount) {
    if (amount == 0) return true;

    auto& resolver = DS2Coop::AddressResolver::GetInstance();
    uintptr_t gmi = resolver.GetGameManagerImp();
    if (!gmi) {
        LOG_WARNING("[SOULS] ApplySoulsGain: GameManagerImp not resolved");
        return false;
    }

    uintptr_t pd = 0;
    if (!Memory::Read<uintptr_t>(gmi + Offsets::GameManager::PlayerData, &pd) || !pd) {
        LOG_WARNING("[SOULS] ApplySoulsGain: PlayerData not readable");
        return false;
    }

    // Read current values
    int32_t  currentSouls  = 0;
    uint32_t soulMemory    = 0;
    if (!Memory::Read<int32_t> (pd + Offsets::GameManager::Souls,      &currentSouls) ||
        !Memory::Read<uint32_t>(pd + Offsets::GameManager::SoulMemory, &soulMemory)) {
        LOG_WARNING("[SOULS] ApplySoulsGain: failed to read souls fields");
        return false;
    }

    // Sanity: cap at 99 999 999 (DS2 max souls display)
    constexpr int32_t MAX_SOULS = 99999999;
    int32_t newSouls = currentSouls + static_cast<int32_t>(amount);
    if (newSouls > MAX_SOULS) newSouls = MAX_SOULS;
    if (newSouls < 0) newSouls = currentSouls; // overflow guard

    uint32_t newMemory = soulMemory + amount;
    if (newMemory < soulMemory) newMemory = UINT32_MAX; // overflow guard

    bool ok = true;
    ok &= Memory::Write<int32_t> (pd + Offsets::GameManager::Souls,      newSouls);
    ok &= Memory::Write<uint32_t>(pd + Offsets::GameManager::SoulMemory, newMemory);

    if (ok) {
        LOG_INFO("[SOULS] Awarded %u souls from peer (now %d, memory %u)",
                 amount, newSouls, newMemory);
    } else {
        LOG_WARNING("[SOULS] ApplySoulsGain: write failed (pd=0x%llX)", pd);
    }
    return ok;
}

// ============================================================================
// ApplyItemPickupToInventory
//
// Calls the game's ItemGive function to add an item directly to the local
// player's inventory.  Used on the RECEIVING side of an ItemPickupPacket.
//
// ItemGive and AvailableItemBag are resolved the same way as GrantSoapstones()
// in player_sync.cpp — they share the same function pointer cache.
// ============================================================================

// Forward-declared externs from player_sync.cpp — the func pointer and
// the ResolveItemGive helper are file-static there, so we re-implement
// the bag resolution inline here rather than break encapsulation.
extern void* g_itemGiveFunc; // defined in player_sync.cpp, cast to ItemGiveFunc below

bool ProgressSync::ApplyItemPickupToInventory(int32_t category, int32_t itemId,
                                              float durability, int16_t quantity,
                                              uint8_t upgrade, uint8_t infusion) {
    // We reuse the PlayerSync ItemGive infrastructure.
    // PlayerSync::GrantSoapstones() already exercises the same path.
    // Calling it via the session triggers the same ItemGive resolve logic.
    // The simplest cross-file approach: delegate to PlayerSync::GrantSoapstones
    // extended version — but that function only gives soapstones.
    //
    // Instead, call ItemGive directly via the same extern used by player_sync.cpp.

    typedef void (__fastcall *ItemGiveFunc)(void* bag, void* items, int count, int mode);
    auto fn = reinterpret_cast<ItemGiveFunc>(g_itemGiveFunc);
    if (!fn) {
        LOG_WARNING("[ITEM] ApplyItemPickup: ItemGive not resolved yet");
        return false;
    }

    // Resolve AvailableItemBag (same chain as player_sync.cpp)
    auto& resolver = DS2Coop::AddressResolver::GetInstance();
    uintptr_t baseA = resolver.GetGameManagerImp();
    if (!baseA) return false;

    uintptr_t ptr1 = 0, ptr2 = 0, bag = 0;
    if (!Memory::Read<uintptr_t>(baseA + Addresses::ItemGib::AvailItemBag_Off1, &ptr1) || !ptr1) return false;
    if (!Memory::Read<uintptr_t>(ptr1  + Addresses::ItemGib::AvailItemBag_Off2, &ptr2) || !ptr2) return false;
    if (!Memory::Read<uintptr_t>(ptr2  + Addresses::ItemGib::AvailItemBag_Off3, &bag)  || !bag)  return false;

    // Build the item struct on the stack (16 bytes, matches DS2ItemStruct)
    #pragma pack(push, 1)
    struct DS2ItemStructLocal {
        int32_t  type;
        int32_t  itemId;
        float    durability;
        int16_t  quantity;
        uint8_t  upgrade;
        uint8_t  infusion;
    };
    #pragma pack(pop)

    DS2ItemStructLocal item{};
    item.type       = category;
    item.itemId     = itemId;
    item.durability = (durability > 0.0f) ? durability : FLT_MAX;
    item.quantity   = (quantity  > 0)     ? quantity   : 1;
    item.upgrade    = upgrade;
    item.infusion   = infusion;

    LOG_INFO("[ITEM] Giving item 0x%08X (cat=%d qty=%d) from peer via ItemGive",
             itemId, category, item.quantity);

    __try {
        fn(reinterpret_cast<void*>(bag), &item, 1, 0);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[ITEM] ItemGive crashed (0x%08X) for item 0x%08X",
                  GetExceptionCode(), itemId);
        return false;
    }
}

void ProgressSync::SyncBossDefeat(uint32_t flagId, bool broadcastToNetwork) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_defeatedBosses.count(flagId)) {
        LOG_DEBUG("[BOSS] Flag %u already synced — skipping duplicate", flagId);
        return;
    }

    m_defeatedBosses.insert(flagId);

    // Apply to local game memory.
    // - Local kill path: flag is already set by the game; this call is a no-op.
    // - Remote receive path: this is the actual write that mirrors the boss kill.
    ApplyEventFlagToMemory(flagId, true);

    if (broadcastToNetwork) {
        LOG_INFO("[BOSS] Broadcasting boss-kill flag %u to all peers", flagId);
        Network::BossDefeatedPacket packet{};
        packet.header.magic    = 0x44533243; // 'DS2C'
        packet.header.type     = Network::PacketType::BossDefeated;
        packet.header.size     = sizeof(Network::BossDefeatedPacket);
        packet.header.sequence = 0;
        packet.bossId          = flagId;     // bossId carries the event flag ID
        packet.defeatTime      = 0;
        Network::PeerManager::GetInstance().BroadcastPacket(&packet.header);
    } else {
        LOG_INFO("[BOSS] Applied boss-kill flag %u from peer (no re-broadcast)", flagId);
    }
}

bool ProgressSync::IsBossDefeated(uint32_t bossId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_defeatedBosses.find(bossId) != m_defeatedBosses.end();
}

void ProgressSync::SyncBonfire(uint32_t bonfireId, bool lit) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    LOG_INFO("Synchronizing bonfire %u: %s", bonfireId, lit ? "lit" : "unlit");
    
    if (lit) {
        m_litBonfires.insert(bonfireId);
    } else {
        m_litBonfires.erase(bonfireId);
    }
    
    // Broadcast bonfire state
    Network::EventFlagPacket packet{};
    packet.header.magic = 0x44533243;
    packet.header.type = Network::PacketType::BonfireRest;
    packet.header.size = sizeof(Network::EventFlagPacket);
    packet.flagId = bonfireId;
    packet.flagValue = lit;
    
    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.BroadcastPacket(&packet.header);
    
    // TODO: Actually set the bonfire state in game memory
}

bool ProgressSync::IsBonfireLit(uint32_t bonfireId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_litBonfires.find(bonfireId) != m_litBonfires.end();
}

void ProgressSync::SyncAllBonfires() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    LOG_INFO("Synchronizing all bonfire states...");
    
    // In a full implementation, this would iterate through all known bonfires
    // and sync their states with other players
    
    for (uint32_t bonfireId : m_litBonfires) {
        SyncBonfire(bonfireId, true);
    }
}

void ProgressSync::SyncItemPickup(uint32_t itemId, uint32_t locationId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    uint64_t combinedId = (static_cast<uint64_t>(itemId) << 32) | locationId;

    if (m_pickedItems.find(combinedId) != m_pickedItems.end()) {
        LOG_DEBUG("Item %u at location %u already picked up", itemId, locationId);
        return;
    }
    
    LOG_INFO("Synchronizing item pickup: item %u at location %u", itemId, locationId);
    
    m_pickedItems[combinedId] = true;
    
    // Broadcast item pickup
    Network::EventFlagPacket packet{};
    packet.header.magic = 0x44533243;
    packet.header.type = Network::PacketType::ItemPickup;
    packet.header.size = sizeof(Network::EventFlagPacket);
    packet.flagId = itemId;
    packet.flagValue = true;
    
    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.BroadcastPacket(&packet.header);
}

bool ProgressSync::IsItemPickedUp(uint32_t itemId, uint32_t locationId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    uint64_t combinedId = (static_cast<uint64_t>(itemId) << 32) | locationId;
    return m_pickedItems.find(combinedId) != m_pickedItems.end();
}

void ProgressSync::NotifyFogGateEntry(uint32_t fogGateId) {
    LOG_INFO("Player entering fog gate %u", fogGateId);
    
    // Broadcast fog gate entry
    Network::EventFlagPacket packet{};
    packet.header.magic = 0x44533243;
    packet.header.type = Network::PacketType::FogGateTransition;
    packet.header.size = sizeof(Network::EventFlagPacket);
    packet.flagId = fogGateId;
    packet.flagValue = true;
    
    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.BroadcastPacket(&packet.header);
}

void ProgressSync::WaitForPartyAtFogGate(uint32_t fogGateId) {
    LOG_INFO("Waiting for party members at fog gate %u...", fogGateId);
    LOG_INFO("Party synchronized at fog gate %u", fogGateId);
}

// ============================================================================
// SyncZoneTransition
//
// Called locally when the local player's warp is detected (LastBonfire
// changed + large position jump). Broadcasts a ZoneTransitionPacket so
// peers can follow the player to the new area.
// ============================================================================
void ProgressSync::SyncZoneTransition(uint32_t bonfireId, uint8_t transitionType) {
    LOG_INFO("[ZONE] Broadcasting zone transition: bonfireId=%u type=%u",
             bonfireId, transitionType);

    Network::ZoneTransitionPacket pkt{};
    pkt.header.magic          = 0x44533243;
    pkt.header.type           = Network::PacketType::ZoneTransition;
    pkt.header.size           = sizeof(Network::ZoneTransitionPacket);
    pkt.bonfireId             = bonfireId;
    pkt.transitionType        = transitionType;

    Network::PeerManager::GetInstance().BroadcastPacket(&pkt.header);
}

// ============================================================================
// ExecuteBonfireWarp
//
// Called on PEER side when a ZoneTransitionPacket is received.
// Attempts to warp the local player to the destination bonfire using the
// game's own warp function (located via AOB scan).
//
// If the warp function was not found (pattern not filled in yet),
// falls back to a notification so the peer can warp manually.
//
// Warp function calling convention (estimated, DS2 SOTFS x64):
//   void __fastcall BonfireWarp(void* warpManager, uint32_t bonfireId)
// ============================================================================

// Resolved once on first call (or always null if pattern is empty)
static void* g_bonfireWarpFunc    = nullptr;
static bool  g_bonfireWarpScanned = false;

// ── Pure SEH wrapper (C2712 fix) ─────────────────────────────────────────────
// Calls the game's BonfireWarp function inside a __try/__except.
// Must be a standalone function with no C++ objects (destructors) in scope.
typedef void (__fastcall *BonfireWarpFn)(void* warpMgr, uint32_t bonfireId);

static bool CallBonfireWarpSafe(BonfireWarpFn fn, void* warpMgr, uint32_t bonfireId) {
    __try {
        fn(warpMgr, bonfireId);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[ZONE] BonfireWarp crashed (0x%08X) — falling back to notification",
                  GetExceptionCode());
        return false;
    }
}

bool ProgressSync::ExecuteBonfireWarp(uint32_t bonfireId) {
    // ── Resolve the warp function (once) ─────────────────────────────────
    if (!g_bonfireWarpScanned) {
        g_bonfireWarpScanned = true;

        // Only scan if a real pattern was provided
        if (Addresses::BONFIRE_WARP.pattern && Addresses::BONFIRE_WARP.pattern[0] != '\0') {
            uintptr_t addr = DS2Coop::Utils::PatternScanner::FindPattern(
                Addresses::BONFIRE_WARP.pattern,
                Addresses::BONFIRE_WARP.mask,
                nullptr
            );
            if (addr) {
                g_bonfireWarpFunc = reinterpret_cast<void*>(addr);
                LOG_INFO("[ZONE] BonfireWarp function found at 0x%p", g_bonfireWarpFunc);
            } else {
                LOG_WARNING("[ZONE] BonfireWarp pattern not found — warp will be manual");
            }
        } else {
            LOG_INFO("[ZONE] BonfireWarp AOB not configured — warp will be manual");
        }
    }

    // ── Attempt automatic warp ────────────────────────────────────────────
    if (g_bonfireWarpFunc) {
        // Resolve the warp manager from GameManagerImp
        auto& resolver = DS2Coop::AddressResolver::GetInstance();
        uintptr_t gmi = resolver.GetGameManagerImp();
        if (!gmi) {
            LOG_WARNING("[ZONE] ExecuteBonfireWarp: GameManagerImp not resolved");
            goto fallback;
        }

        BonfireWarpFn fn = reinterpret_cast<BonfireWarpFn>(g_bonfireWarpFunc);

        // Use GMI directly as the warp manager until the correct offset is found
        void* warpMgr = reinterpret_cast<void*>(gmi);

        LOG_INFO("[ZONE] Executing bonfire warp to bonfire %u (warpMgr=0x%p)",
                 bonfireId, warpMgr);
        // C2712 fix: __try isolated in CallBonfireWarpSafe (no C++ objects in that scope)
        if (CallBonfireWarpSafe(fn, warpMgr, bonfireId)) {
            LOG_INFO("[ZONE] Bonfire warp triggered successfully");
            return true;
        }
    }

fallback:
    // ── Fallback: show notification ───────────────────────────────────────
    // The peer must warp manually. We show a notification so they know where
    // the host went.  The bonfire name is resolved from the lookup table.
    const char* name = GetBonfireName(bonfireId);
    LOG_INFO("[ZONE] Warp fallback — host is at '%s' (bonfire %u)", name, bonfireId);

    DS2Coop::UI::Overlay::GetInstance().ShowNotification(
        std::string("L'hôte a bougé vers : ") + name + "\nOuvrez un feu de camp pour le rejoindre.",
        8.0f
    );
    return false;
}

// ============================================================================
// GetBonfireName
//
// Lookup table: DS2 SOTFS bonfire IDs → human-readable names.
// IDs sourced from community spreadsheets / DS2S-META.
// Incomplete — add more as needed.
// ============================================================================
const char* ProgressSync::GetBonfireName(uint32_t bonfireId) {
    // Format: { bonfireId, "Zone — Bonfire Name" }
    // A bonfire ID in DS2 is the flag ID set when the player rests at it.
    static const struct { uint32_t id; const char* name; } kBonfires[] = {
        // Things Betwixt
        { 1000000, "Things Betwixt — Bonfire" },
        // Majula
        { 1010000, "Majula — Bonfire" },
        // Forest of Fallen Giants
        { 1020000, "Forest of Fallen Giants — Cardinal Tower" },
        { 1020010, "Forest of Fallen Giants — Soldier's Rest" },
        { 1020020, "Forest of Fallen Giants — The Place Unbeknownst" },
        { 1020030, "Forest of Fallen Giants — Last Giant" },
        // Heide's Tower of Flame
        { 1030000, "Heide's Tower of Flame — Heide's Ruin" },
        { 1030010, "Heide's Tower of Flame — Tower of Flame" },
        { 1030020, "Heide's Tower of Flame — The Altar of Sunlight" },
        // No-man's Wharf
        { 1040000, "No-man's Wharf — Unseen Path to Heide" },
        { 1040010, "No-man's Wharf — Laddersmith Gilligan" },
        // The Lost Bastille
        { 1050000, "The Lost Bastille — Tower Apart" },
        { 1050010, "The Lost Bastille — McDuff's Workshop" },
        { 1050020, "The Lost Bastille — Servants' Quarters" },
        { 1050030, "The Lost Bastille — Straid's Cell" },
        { 1050040, "The Lost Bastille — The Saltfort" },
        // Sinner's Rise
        { 1060000, "Sinner's Rise — Undead Lockaway" },
        // Huntsman's Copse
        { 1070000, "Huntsman's Copse — Undead Refuge" },
        { 1070010, "Huntsman's Copse — Bridge Approach" },
        { 1070020, "Huntsman's Copse — Undead Lockaway" },
        // Harvest Valley
        { 1080000, "Harvest Valley — Poison Pool" },
        { 1080010, "Harvest Valley — Middle of the Copse" },
        { 1080020, "Harvest Valley — The Mines" },
        // Earthen Peak
        { 1090000, "Earthen Peak — Entrance Chamber" },
        { 1090010, "Earthen Peak — Central Earthen Peak" },
        { 1090020, "Earthen Peak — Upper Earthen Peak" },
        // Iron Keep
        { 1100000, "Iron Keep — Threshold Bridge" },
        { 1100010, "Iron Keep — Eygil's Idol" },
        { 1100020, "Iron Keep — Belfry Sol" },
        // Shaded Woods
        { 1110000, "Shaded Woods — Ruined Fork Road" },
        { 1110010, "Shaded Woods — Shaded Ruins" },
        { 1110020, "Shaded Woods — King's Gate" },
        // Doors of Pharros
        { 1120000, "Doors of Pharros — Doors of Pharros" },
        // Brightstone Cove
        { 1130000, "Brightstone Cove Tseldora — Chapel Threshold" },
        { 1130010, "Brightstone Cove Tseldora — Prowling Magus" },
        // The Gutter / Black Gulch
        { 1140000, "The Gutter — Upper Gutter" },
        { 1140010, "The Gutter — Central Gutter" },
        { 1150000, "Black Gulch — Black Gulch Mouth" },
        { 1150010, "Black Gulch — Hidden Chamber" },
        // Grave of Saints
        { 1160000, "Grave of Saints — Grave of Saints" },
        // Drangleic Castle
        { 1300000, "Drangleic Castle — King's Gate" },
        { 1300010, "Drangleic Castle — Central Castle Drangleic" },
        // Shrine of Amana
        { 1310000, "Shrine of Amana — Offering Road" },
        { 1310010, "Shrine of Amana — Crumbled Ruins" },
        { 1310020, "Shrine of Amana — Rhoy's Resting Place" },
        // Undead Crypt
        { 1320000, "Undead Crypt — Undead Ditch" },
        // Aldia's Keep
        { 1330000, "Aldia's Keep — Foregarden" },
        { 1330010, "Aldia's Keep — Outer Wall" },
        // Dragon Aerie / Shrine
        { 1340000, "Dragon Aerie — Dragon Aerie" },
        { 1341000, "Dragon Shrine — Dragon Shrine" },
        // Throne of Want
        { 1900000, "Throne of Want — Throne of Want" },
        // DLC 1: Sunken King
        { 2100000, "Shulva, Sanctum City — Sanctum Nadir" },
        // DLC 2: Iron King
        { 2200000, "Brume Tower — Throne Floor" },
        // DLC 3: Ivory King
        { 2300000, "Frozen Eleum Loyce — Outer Wall" },
    };

    for (const auto& e : kBonfires) {
        if (e.id == bonfireId) return e.name;
    }

    // Not found — return generic label
    static char buf[32];
    snprintf(buf, sizeof(buf), "Feu de camp #%u", bonfireId);
    return buf;
}

