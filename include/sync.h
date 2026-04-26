#pragma once

#include <cstdint>
#include <string>
#include <mutex>
#include <unordered_set>
#include <unordered_map>

namespace DS2Coop::Sync {

// Progress synchronization manager
class ProgressSync {
public:
    static ProgressSync& GetInstance();
    
    bool Initialize();
    void Shutdown();
    
    // Event flags synchronization
    void SyncEventFlag(uint32_t flagId, bool value);
    bool GetEventFlag(uint32_t flagId);
    void RequestEventFlagSync();
    
    // Boss defeat synchronization.
    // broadcastToNetwork=true  → local kill path: write to memory + broadcast to peers.
    // broadcastToNetwork=false → receive path: write to memory only (no re-broadcast).
    void SyncBossDefeat(uint32_t flagId, bool broadcastToNetwork = true);
    bool IsBossDefeated(uint32_t bossId);
    
    // Bonfire synchronization
    void SyncBonfire(uint32_t bonfireId, bool lit);
    bool IsBonfireLit(uint32_t bonfireId);
    void SyncAllBonfires();
    
    // Item pickup synchronization (optional)
    void SyncItemPickup(uint32_t itemId, uint32_t locationId);
    bool IsItemPickedUp(uint32_t itemId, uint32_t locationId);
    
    // Fog gate synchronization
    void NotifyFogGateEntry(uint32_t fogGateId);
    void WaitForPartyAtFogGate(uint32_t fogGateId);

    // Write an event flag directly to the EventFlagManager bitfield in game memory.
    // Called when a peer sends us a boss kill / event-flag sync packet.
    // Returns true if the flag was written, false if the EFMan couldn't be resolved.
    static bool ApplyEventFlagToMemory(uint32_t flagId, bool value);

    // Award souls to the local player (direct PlayerData write).
    // Called when a peer's boss-kill packet is received.
    static bool ApplySoulsGainToMemory(uint32_t amount);

    // Give an item to the local player via ItemGive.
    // Called when a peer's item-pickup packet is received.
    // Returns false if ItemGive couldn't be resolved.
    static bool ApplyItemPickupToInventory(int32_t category, int32_t itemId,
                                           float durability, int16_t quantity,
                                           uint8_t upgrade, uint8_t infusion);

    // ── Zone transition (loading screen sync) ─────────────────────────────
    // SyncZoneTransition: called by the local player when a warp is detected.
    // Broadcasts a ZoneTransitionPacket to all peers.
    void SyncZoneTransition(uint32_t bonfireId, uint8_t transitionType = 0);

    // ExecuteBonfireWarp: called on peers when they receive a ZoneTransitionPacket.
    // Attempts to warp the local player to the given bonfire via the game's own
    // warp function (AOB scan). Falls back to a notification if the function
    // could not be located.
    // Returns true if the warp was successfully triggered.
    static bool ExecuteBonfireWarp(uint32_t bonfireId);

    // Human-readable name for a bonfire ID (for notifications).
    // Returns "Bonfire #XXXXX" if the ID is not in the lookup table.
    static const char* GetBonfireName(uint32_t bonfireId);

private:
    ProgressSync() = default;
    ~ProgressSync() = default;
    ProgressSync(const ProgressSync&) = delete;
    ProgressSync& operator=(const ProgressSync&) = delete;
    
    bool m_initialized = false;
    std::recursive_mutex m_mutex;
    std::unordered_map<uint32_t, bool> m_eventFlags;
    std::unordered_set<uint32_t> m_defeatedBosses;
    std::unordered_set<uint32_t> m_litBonfires;
    std::unordered_map<uint64_t, bool> m_pickedItems;
};

// Player synchronization manager
class PlayerSync {
public:
    static PlayerSync& GetInstance();
    
    bool Initialize();
    void Shutdown();
    
    void Update(float deltaTime);
    
    // Position synchronization
    void SyncLocalPlayerPosition();
    void ApplyRemotePlayerPosition(uint64_t playerId, float x, float y, float z, float rotX, float rotY, float rotZ);
    
    // State synchronization
    void SyncLocalPlayerState();
    void ApplyRemotePlayerState(uint64_t playerId, int32_t health, int32_t maxHealth, int32_t stamina, int32_t maxStamina);
    
    // Animation synchronization
    void SyncAnimation(uint64_t playerId, uint32_t animationId);
    
    // Equipment synchronization
    void SyncEquipment(uint64_t playerId);

    // Seamless helpers
    bool GrantSoapstones();
    bool MaxPhantomTimer();
    void EnableSummoning();
    std::string GetLocalCharacterName();

    // Emergency teleport — write host's position directly into local PlayerData.
    // Returns true if the write succeeded, false if the host's position is unknown
    // or if GameManagerImp couldn't be resolved.
    // cooldownSeconds: minimum delay between two calls (prevents spamming).
    bool TeleportToHost();

private:
    PlayerSync() = default;
    ~PlayerSync() = default;
    PlayerSync(const PlayerSync&) = delete;
    PlayerSync& operator=(const PlayerSync&) = delete;
    
    bool m_initialized = false;
    float m_positionSyncTimer    = 0.0f;
    float m_stateSyncTimer       = 0.0f;
    float m_phantomTimerRefresh  = 0.0f;
    float m_zoneSyncTimer        = 0.0f;  // cooldown between zone broadcasts

    // Previous values for warp detection
    uint32_t m_prevLastBonfire = 0;
    float    m_prevPosX = 0.0f;
    float    m_prevPosY = 0.0f;
    float    m_prevPosZ = 0.0f;
    bool     m_zoneTrackingReady = false; // true after first valid read

    static constexpr float POSITION_SYNC_INTERVAL = 0.05f; // 20 times per second
    static constexpr float STATE_SYNC_INTERVAL    = 0.5f;  // 2 times per second
    static constexpr float ZONE_SYNC_COOLDOWN     = 5.0f;  // min seconds between zone broadcasts
    static constexpr float WARP_DISTANCE_THRESHOLD = 50.0f; // units — minimum jump to count as warp
};

} // namespace DS2Coop::Sync

