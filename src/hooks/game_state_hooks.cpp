// Game State Hooks - Local event detection
//
// PRIMARY hook implemented here: SetEventFlag
//   Fires whenever the game sets any persistent event flag (boss kills,
//   area discoveries, NPC interactions, etc.).  We use it to detect boss
//   kills and broadcast them to co-op peers so they receive the same flag
//   in their own save data.
//
// SECONDARY hooks (PlayerDeath, BossDefeated) remain stubs — the protobuf
// interception layer handles disconnect prevention without them.
//
// Boss kill sync flow:
//   1. Host kills boss → game calls SetEventFlag(flagMan, flagId, true)
//   2. SetEventFlagHook fires, calls g_originalSetEventFlag first
//   3. If seamless mode active + flagId >= threshold → ProgressSync::SyncBossDefeat(flagId)
//   4. SyncBossDefeat broadcasts BossDefeatedPacket to all peers
//   5. Each peer receives packet → PacketHandler::HandleBossDefeated
//   6. → ProgressSync::SyncBossDefeat (peer side) → ApplyEventFlagToMemory
//   7. ApplyEventFlagToMemory writes the bit directly to EventFlagManager
//      (does NOT call SetEventFlag, avoiding a broadcast loop)

#include "../../include/hooks.h"
#include "../../include/sync.h"
#include "../../include/session.h"
#include "../../include/utils.h"
#include "../../include/addresses.h"
#include "../../include/pattern_scanner.h"
#include "../../include/network.h"
#include "MinHook.h"
#include <unordered_set>
#include <mutex>
#include <cfloat>

using namespace DS2Coop::Hooks;
using namespace DS2Coop::Utils;
using namespace DS2Coop::Addresses;
namespace Network = DS2Coop::Network;  // alias so Network::Foo resolves to DS2Coop::Network::Foo

// Original function pointers
static GameState::PlayerDeathFunc  g_originalPlayerDeath   = nullptr;
static GameState::BossDefeatedFunc g_originalBossDefeated  = nullptr;
static GameState::SetEventFlagFunc g_originalSetEventFlag  = nullptr;
static GameState::ItemGiveFunc     g_originalItemGive      = nullptr;

// Externals from player_sync.cpp
extern bool g_ourItemGiveCall;
extern void* g_itemGiveFunc;   // same pointer, cast below when installing hook

// ============================================================================
// SetEventFlag hook
//
// Called every time the game sets any event flag — including boss kills,
// bonfire rests, item pick-ups, NPC dialogue, and many combat-state flags.
//
// We:
//   1. Always call the original first (game state must not be disrupted).
//   2. Only act if seamless mode is active (session running).
//   3. Only act on flags >= EVENT_FLAG_SYNC_THRESHOLD (skip per-frame runtime flags).
//   4. Only act on value=true (setting, not clearing — boss kills are always set).
//   5. Guard against re-entrant calls: ApplyEventFlagToMemory writes directly
//      to memory without going through SetEventFlag, so there is no loop.
// ============================================================================

// Thread-local reentrancy guard: prevents the hook from triggering again
// if ProgressSync::SyncBossDefeat somehow causes a flag set (shouldn't happen
// with the direct-memory-write approach, but belt-and-suspenders).
static thread_local bool g_inSetEventFlagHook = false;

// ── Pure SEH wrapper (C2712 fix) ────────────────────────────────────────────
// MSVC C2712: __try cannot coexist with C++ objects that have destructors in
// the same function scope (even in nested blocks that have been exited).
// Solution: isolate every __try/__except in a plain function with no C++ objects.

static void SyncBossDefeatSafe(uint32_t flagId) {
    // No C++ objects here — only POD locals are allowed alongside __try.
    __try {
        DS2Coop::Sync::ProgressSync::GetInstance().SyncBossDefeat(flagId);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[BOSS-SYNC] Exception in SyncBossDefeat for flag %u: 0x%08X",
                  flagId, GetExceptionCode());
    }
}

// Per-session set of already-broadcast flag IDs.
// Prevents spamming peers if the game re-calls SetEventFlag for the same flag.
static std::unordered_set<uint32_t> g_broadcastFlags;
static std::mutex g_broadcastMutex;

static void __fastcall SetEventFlagHook(void* flagMan, uint32_t flagId, bool value) {
    // Always let the original run first — game state takes priority.
    g_originalSetEventFlag(flagMan, flagId, value);

    // Reentrancy guard
    if (g_inSetEventFlagHook) return;

    // Only sync "set" operations and only above the threshold
    if (!value || flagId < EVENT_FLAG_SYNC_THRESHOLD) return;

    // Only act when a co-op session is active
    if (!DS2Coop::Hooks::ProtobufHooks::IsSeamlessActive()) return;

    // Log all persistent flag sets at DEBUG level.
    // To identify boss-kill flag IDs: set log level to DEBUG and watch
    // for "[EFLAG] SetEventFlag: flagId=XXXXX" entries when killing bosses.
    LOG_DEBUG("[EFLAG] SetEventFlag: flagId=%u value=1 (flagMan=%p)", flagId, flagMan);

    // Deduplicate: don't broadcast the same flag twice in one session
    {
        std::lock_guard<std::mutex> lk(g_broadcastMutex);
        if (!g_broadcastFlags.insert(flagId).second) {
            // Already broadcast this flag in this session
            return;
        }
    }

    LOG_INFO("[BOSS-SYNC] Detected new flag set: %u — broadcasting to peers", flagId);

    g_inSetEventFlagHook = true;
    // Call through SEH-isolated wrapper (C2712: __try cannot live in a function
    // that also has C++ objects with destructors such as std::lock_guard above).
    SyncBossDefeatSafe(flagId);
    g_inSetEventFlagHook = false;
}

// ============================================================================
// ItemGive hook — item pickup broadcast
//
// The game calls ItemGive whenever an item is added to the player's inventory
// (world pickups, chest opens, boss rewards, NPC rewards, etc.).
// We intercept these calls and broadcast an ItemPickupPacket to peers so
// they receive the same item via ItemGive on their side.
//
// Guard: g_ourItemGiveCall is true when WE (the mod) call ItemGive for
// soapstones, received-item grants, etc.  In that case we skip the hook.
//
// DS2ItemStruct layout (16 bytes, packed):
//   int32  type       (item category)
//   int32  itemId
//   float  durability
//   int16  quantity
//   uint8  upgrade
//   uint8  infusion
// ============================================================================

#pragma pack(push, 1)
struct ItemGiveHookStruct {
    int32_t  type;
    int32_t  itemId;
    float    durability;
    int16_t  quantity;
    uint8_t  upgrade;
    uint8_t  infusion;
};
#pragma pack(pop)

static void __fastcall ItemGiveHook(void* bag, void* items, int count, int mode) {
    // Always call the original first — item must be in inventory before we broadcast
    g_originalItemGive(bag, items, count, mode);

    // Skip if this is our own mod call (soapstones, soul grants, received items)
    if (g_ourItemGiveCall) return;

    // Only broadcast when a co-op session is active
    if (!DS2Coop::Hooks::ProtobufHooks::IsSeamlessActive()) return;

    // Safety: items pointer must be valid and count reasonable
    if (!items || count <= 0 || count > 64) return;

    auto* itemArr = reinterpret_cast<ItemGiveHookStruct*>(items);

    for (int i = 0; i < count; i++) {
        const auto& it = itemArr[i];

        // Filter: skip obviously invalid items (itemId 0 or negative)
        if (it.itemId <= 0) continue;

        LOG_INFO("[ITEM] ItemGive hook: id=0x%08X cat=%d qty=%d upg=%u inf=%u — broadcasting",
                 it.itemId, it.type, it.quantity, it.upgrade, it.infusion);

        Network::ItemPickupPacket pkt{};
        pkt.header.magic    = 0x44533243;
        pkt.header.type     = Network::PacketType::ItemPickup;
        pkt.header.size     = sizeof(Network::ItemPickupPacket);
        pkt.category        = it.type;
        pkt.itemId          = it.itemId;
        pkt.durability      = it.durability;
        pkt.quantity        = it.quantity;
        pkt.upgrade         = it.upgrade;
        pkt.infusion        = it.infusion;

        // Mark OUR broadcast as a mod call so peers' ItemGiveHook ignores it
        // (peers set g_ourItemGiveCall=true in ApplyItemPickupToInventory)
        Network::PeerManager::GetInstance().BroadcastPacket(&pkt.header);
    }
}

// ============================================================================
// Clear broadcast cache at session start/end
// ============================================================================
void GameState::ClearBroadcastFlagCache() {
    std::lock_guard<std::mutex> lk(g_broadcastMutex);
    g_broadcastFlags.clear();
    LOG_INFO("[BOSS-SYNC] Broadcast flag cache cleared");
}

// ============================================================================
// Stub hooks (remain for future use)
// ============================================================================

static void __fastcall PlayerDeathHook(void* playerPtr) {
    LOG_INFO("[GAME] Player death detected");

    auto& sessionMgr = DS2Coop::Session::SessionManager::GetInstance();
    auto* localPlayer = sessionMgr.GetLocalPlayer();
    if (localPlayer) {
        sessionMgr.NotifyPlayerDeath(localPlayer->playerId);
    }

    g_originalPlayerDeath(playerPtr);
    LOG_INFO("[GAME] Player died - protobuf hooks will block disconnect");
}

static void __fastcall BossDefeatedHook(void* bossPtr) {
    LOG_INFO("[GAME] BossDefeatedHook fired (secondary hook)");
    g_originalBossDefeated(bossPtr);
}

// ============================================================================
// Installation
// ============================================================================
bool GameState::InstallHooks() {
    LOG_INFO("Installing game state hooks...");

    int hooked = 0;

    // --- SetEventFlag (primary boss-kill detection hook) ---
    LOG_INFO("Scanning for SetEventFlag...");
    uintptr_t setFlagAddr = DS2Coop::Utils::PatternScanner::FindPattern(
        SET_EVENT_FLAG.pattern,
        SET_EVENT_FLAG.mask,
        nullptr
    );

    if (setFlagAddr) {
        LOG_INFO("  SetEventFlag found at: 0x%p", reinterpret_cast<void*>(setFlagAddr));
        if (HookManager::GetInstance().InstallHook(
            reinterpret_cast<void*>(setFlagAddr),
            reinterpret_cast<void*>(&SetEventFlagHook),
            reinterpret_cast<void**>(&g_originalSetEventFlag)
        )) {
            LOG_INFO("  HOOKED SetEventFlag — boss kill sync active");
            hooked++;
        } else {
            LOG_ERROR("  Failed to hook SetEventFlag");
        }
    } else {
        LOG_WARNING("  SetEventFlag pattern NOT FOUND — boss kill sync disabled");
        LOG_WARNING("  Peers will NOT receive boss kill flags from this player.");
        LOG_WARNING("  AOB: 48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 C1 EB 05");
    }

    // --- ItemGive hook (item pickup broadcast) ---
    // g_itemGiveFunc is resolved lazily in player_sync.cpp (first call to GrantSoapstones).
    // At InstallHooks time it may be null; we install the hook only if already resolved.
    // If not yet resolved, player_sync.cpp will resolve it on first use and we miss
    // the window — acceptable: hook is best-effort and installs correctly when present.
    if (g_itemGiveFunc) {
        if (HookManager::GetInstance().InstallHook(
            g_itemGiveFunc,
            reinterpret_cast<void*>(&ItemGiveHook),
            reinterpret_cast<void**>(&g_originalItemGive)
        )) {
            LOG_INFO("  HOOKED ItemGive — item pickup broadcast active");
            hooked++;
        } else {
            LOG_WARNING("  ItemGive hook failed (non-critical)");
        }
    } else {
        LOG_INFO("  ItemGive not yet resolved — hook deferred until first soapstone grant");
        // Deferred hook install: will be attempted again after PlayerSync::Initialize()
        // triggers ResolveItemGive via GrantSoapstones().
        // For now, item pickup broadcast is unavailable until the first grant attempt.
    }

    // --- PlayerDeath and BossDefeated (stub — no AOB yet) ---
    void* playerDeathAddr   = nullptr;
    void* bossDefeatedAddr  = nullptr;

    if (playerDeathAddr) {
        if (HookManager::GetInstance().InstallHook(
            playerDeathAddr,
            reinterpret_cast<void*>(&PlayerDeathHook),
            reinterpret_cast<void**>(&g_originalPlayerDeath)
        )) {
            LOG_INFO("  HOOKED PlayerDeath");
            hooked++;
        }
    }

    if (bossDefeatedAddr) {
        if (HookManager::GetInstance().InstallHook(
            bossDefeatedAddr,
            reinterpret_cast<void*>(&BossDefeatedHook),
            reinterpret_cast<void**>(&g_originalBossDefeated)
        )) {
            LOG_INFO("  HOOKED BossDefeated");
            hooked++;
        }
    }

    LOG_INFO("Game state hooks: %d installed", hooked);
    return true; // non-critical — mod works without these
}

void GameState::UninstallHooks() {
    LOG_INFO("Uninstalling game state hooks...");
    GameState::ClearBroadcastFlagCache();
}

// ============================================================================
// HookManager implementation (shared by all hook types)
// ============================================================================
HookManager& HookManager::GetInstance() {
    static HookManager instance;
    return instance;
}

bool HookManager::Initialize() {
    if (m_initialized) return true;

    LOG_INFO("Initializing MinHook...");

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        LOG_ERROR("Failed to initialize MinHook: %s", MH_StatusToString(status));
        return false;
    }

    m_initialized = true;
    LOG_INFO("MinHook initialized");
    return true;
}

void HookManager::Shutdown() {
    if (!m_initialized) return;

    LOG_INFO("Shutting down MinHook...");
    MH_Uninitialize();
    m_initialized = false;
}

bool HookManager::InstallHook(void* targetFunc, void* detourFunc, void** originalFunc) {
    if (!m_initialized) {
        LOG_ERROR("HookManager not initialized");
        return false;
    }

    MH_STATUS status = MH_CreateHook(targetFunc, detourFunc, originalFunc);
    if (status != MH_OK) {
        LOG_ERROR("MH_CreateHook failed: %s (target: %p)", MH_StatusToString(status), targetFunc);
        return false;
    }

    status = MH_EnableHook(targetFunc);
    if (status != MH_OK) {
        LOG_ERROR("MH_EnableHook failed: %s (target: %p)", MH_StatusToString(status), targetFunc);
        return false;
    }

    LOG_DEBUG("Hook installed at %p", targetFunc);
    return true;
}

bool HookManager::RemoveHook(void* targetFunc) {
    if (!m_initialized) return false;

    MH_DisableHook(targetFunc);
    MH_RemoveHook(targetFunc);
    return true;
}

bool HookManager::EnableHooks() {
    if (!m_initialized) return false;
    return MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
}

bool HookManager::DisableHooks() {
    if (!m_initialized) return false;
    return MH_DisableHook(MH_ALL_HOOKS) == MH_OK;
}
