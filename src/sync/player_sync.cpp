// Player Synchronization - ACTUAL GAME MEMORY READS
//
// This file reads player position/health/state from the game's memory
// using the resolved GameManagerImp base pointer and verified offsets
// from the Bob Edition cheat table.
//
// Pointer chain: GameManagerImp -> +0x38 (PlayerData) -> offsets

#include "../../include/sync.h"
#include "../../include/mod.h"
#include "../../include/session.h"
#include "../../include/ui.h"
#include "../../include/network.h"
#include "../../include/addresses.h"
#include "../../include/address_resolver.h"
#include "../../include/hooks.h"
#include "../../include/pattern_scanner.h"
#include "../../include/utils.h"
#include <chrono>
#include <cfloat>
#include <cmath>

using namespace DS2Coop::Sync;
using namespace DS2Coop::Utils;
using namespace DS2Coop::Addresses;

// ── ItemGive globals ─────────────────────────────────────────────────────────
// Defined here (before Initialize) so they are visible throughout the entire TU.
// Declared extern in game_state_hooks.cpp and progress_sync.cpp.
// Stored as void* to avoid cross-TU function-pointer type conflicts; cast to
// ItemGiveFunc at each call site within this file.
void* g_itemGiveFunc    = nullptr;   // set by PlayerSync::Initialize() via AOB scan
bool  g_itemGiveScanned = false;     // guards the one-shot AOB scan
bool  g_ourItemGiveCall = false;     // reentrancy guard: true while the mod calls ItemGive

PlayerSync& PlayerSync::GetInstance() {
    static PlayerSync instance;
    return instance;
}

// ============================================================================
// NOP the boss-kill phantom return call in the exe.
//
// Ghidra analysis (2026-04-04):
//   FUN_14044ef30 is the event dispatch. Line 12 calls FUN_140191bb0
//   which creates EventPhantomReturn objects and sends all phantoms home.
//   The CALL instruction is at exe+0x44ef7b (5 bytes: e8 30 2c d4 ff).
//   NOPing it prevents the game from dismissing phantoms on boss death.
// ============================================================================
static void PatchPhantomReturnOnBossKill() {
    uintptr_t exeBase = (uintptr_t)GetModuleHandle(nullptr);
    uintptr_t callAddr = exeBase + 0x44ef7b;

    // Verify the bytes match the expected CALL instruction
    uint8_t expected[] = { 0xe8, 0x30, 0x2c, 0xd4, 0xff };
    uint8_t actual[5] = {};

    __try {
        memcpy(actual, (void*)callAddr, 5);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("PatchPhantomReturn: cannot read exe at 0x%llX", callAddr);
        return;
    }

    if (memcmp(actual, expected, 5) != 0) {
        // Bytes don't match — might be a different exe version or already patched
        LOG_WARNING("PatchPhantomReturn: bytes at exe+0x44ef7b don't match expected CALL "
                    "(got %02X %02X %02X %02X %02X, expected e8 30 2c d4 ff) — skipping",
                    actual[0], actual[1], actual[2], actual[3], actual[4]);
        // Check if already NOPed
        uint8_t nops[] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
        if (memcmp(actual, nops, 5) == 0) {
            LOG_INFO("PatchPhantomReturn: already patched (NOPs)");
        }
        return;
    }

    // Make the page writable, write NOPs, restore protection
    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)callAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("PatchPhantomReturn: VirtualProtect failed (error %u)", GetLastError());
        return;
    }

    memset((void*)callAddr, 0x90, 5);  // 5x NOP
    VirtualProtect((void*)callAddr, 5, oldProtect, &oldProtect);

    LOG_INFO("PatchPhantomReturn: PATCHED exe+0x44ef7b — boss kill will no longer dismiss phantoms");
}

// ============================================================================
// NOP the per-phantom dismissal CALLs inside FUN_140191bb0.
//
// Ghidra analysis (2026-04-06):
//   The previous PatchPhantomReturnOnBossKill NOPed the entire CALL to
//   FUN_140191bb0 at exe+0x44ef7b. That broke death/respawn because
//   FUN_140191bb0 sets a completion flag at [RSI+0x24]=1 in its epilogue
//   that the death state machine waits for. Skipping the whole call left
//   the player permanently dead-but-not-respawning, with the pause menu
//   unable to open.
//
//   The fix is more surgical: NOP only the per-phantom dismissal CALLs
//   INSIDE FUN_140191bb0's two iteration loops. The function still runs,
//   the loops still iterate (they just do nothing per phantom), and the
//   epilogue still sets the completion flag. Death proceeds normally,
//   boss kills no longer dismiss phantoms.
//
//   FUN_140191bb0 structure (Ghidra disassembly):
//     +0x191bb0  prologue, allocates EventPhantomReturn objects
//     +0x191c80  loop 1: for each phantom in [RSI+0x28..+0x30]:
//     +0x191c87    CALL FUN_140190410       <-- DISMISSAL CALL #1
//     +0x191c8c    advance pointer
//     +0x191c93    loop back
//     +0x191d10  loop 2: for each phantom in [RSI+0x48..+0x50]:
//     +0x191d17    CALL FUN_14018dea0       <-- DISMISSAL CALL #2
//     +0x191d1c    advance pointer
//     +0x191d23    loop back
//     +0x191d8d  MOV byte ptr [RSI+0x24],0x1  <-- COMPLETION FLAG (must run)
//     +0x191d98  RET
//
//   Both CALLs are 5-byte near calls (E8 + 4-byte rel32). NOPing them
//   leaves the loops intact but turns each iteration into a no-op.
//
// Confirmed by ghidra_phantom_return_results.txt (Apr 6 run).
// ============================================================================
static void PatchPhantomDismissalLoops() {
    uintptr_t exeBase = (uintptr_t)GetModuleHandle(nullptr);

    struct PatchSite {
        const char* label;
        uintptr_t offset;
    };

    PatchSite sites[] = {
        { "loop1 dismissal", 0x191c87 },  // CALL FUN_140190410
        { "loop2 dismissal", 0x191d17 },  // CALL FUN_14018dea0
    };

    for (auto& site : sites) {
        uintptr_t addr = exeBase + site.offset;
        uint8_t actual[5] = {};

        __try {
            memcpy(actual, (void*)addr, 5);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERROR("PatchDismissal: cannot read exe at 0x%llX (%s)", addr, site.label);
            continue;
        }

        // Already-patched check (5x NOP)
        uint8_t nops[] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
        if (memcmp(actual, nops, 5) == 0) {
            LOG_INFO("PatchDismissal: %s at exe+0x%llX already NOPed", site.label, site.offset);
            continue;
        }

        // Verify first byte is a near CALL (E8). We don't verify the
        // full 4-byte offset because Ghidra base relocation may differ
        // from runtime — but the opcode E8 is the discriminator.
        if (actual[0] != 0xe8) {
            LOG_WARNING("PatchDismissal: %s at exe+0x%llX expected CALL (E8), got %02X — skipping",
                        site.label, site.offset, actual[0]);
            continue;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect((void*)addr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG_ERROR("PatchDismissal: VirtualProtect failed at 0x%llX (error %u)",
                      addr, GetLastError());
            continue;
        }

        memset((void*)addr, 0x90, 5);
        VirtualProtect((void*)addr, 5, oldProtect, &oldProtect);

        LOG_INFO("PatchDismissal: PATCHED %s at exe+0x%llX (NOPed dismissal call)",
                 site.label, site.offset);
    }
}

// ============================================================================
// Increase the player cap from 3 to 6.
//
// Ghidra analysis (2026-04-04):
//   FUN_1406ab050 is the JoinGuestPlayer message handler.
//   At exe+0x6ab0b6: MOV dword ptr [RBP+local_6c], 0x3 (c7 45 c3 03 00 00 00)
//   The 0x03 byte at exe+0x6ab0b9 is the player cap.
//   Changing it to 0x06 allows up to 6 players.
// ============================================================================
static void PatchPlayerCap() {
    // Lire max_players depuis la config, clamped entre 2 et 6 (limite physique DS2)
    uint8_t targetCap = static_cast<uint8_t>(
        DS2Coop::SeamlessCoopMod::GetInstance().GetConfig().max_players);
    if (targetCap < 2) targetCap = 2;
    if (targetCap > 6) targetCap = 6;

    uintptr_t exeBase = (uintptr_t)GetModuleHandle(nullptr);
    uintptr_t instrAddr = exeBase + 0x6ab0b6;

    // Instruction attendue : c7 45 c3 03 00 00 00 (MOV dword ptr [RBP+local_6c], 0x3)
    uint8_t expected[] = { 0xc7, 0x45, 0xc3, 0x03, 0x00, 0x00, 0x00 };
    uint8_t actual[7] = {};

    __try {
        memcpy(actual, (void*)instrAddr, 7);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("PatchPlayerCap: cannot read exe at 0x%llX", instrAddr);
        return;
    }

    // Vérifier si déjà patché avec la valeur cible
    if (actual[0] == 0xc7 && actual[1] == 0x45 && actual[2] == 0xc3 &&
        actual[3] == targetCap && actual[4] == 0x00 && actual[5] == 0x00 && actual[6] == 0x00) {
        LOG_INFO("PatchPlayerCap: already patched (cap=%u)", targetCap);
    } else if (memcmp(actual, expected, 7) != 0 && actual[3] != 0x03) {
        LOG_WARNING("PatchPlayerCap: bytes at exe+0x6ab0b6 don't match expected "
                    "(got %02X %02X %02X %02X %02X %02X %02X) - skipping",
                    actual[0], actual[1], actual[2], actual[3],
                    actual[4], actual[5], actual[6]);
    } else {
        DWORD oldProtect = 0;
        if (!VirtualProtect((void*)instrAddr, 7, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG_ERROR("PatchPlayerCap: VirtualProtect failed (error %u)", GetLastError());
            return;
        }
        *((uint8_t*)(instrAddr + 3)) = targetCap;
        VirtualProtect((void*)instrAddr, 7, oldProtect, &oldProtect);
        LOG_INFO("PatchPlayerCap: PATCHED exe+0x6ab0b9 (local_6c 3->%u)", targetCap);
    }

    // Second patch : valeur écrite dans le struct protobuf à [RBX+0x1c]
    // 1406ab15b: c7 43 1c 03 00 00 00  MOV dword ptr [RBX+0x1c], 0x3
    uintptr_t msgAddr = exeBase + 0x6ab15b;
    uint8_t expected2[] = { 0xc7, 0x43, 0x1c, 0x03, 0x00, 0x00, 0x00 };
    uint8_t actual2[7] = {};

    __try {
        memcpy(actual2, (void*)msgAddr, 7);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARNING("PatchPlayerCap: cannot read second patch addr 0x%llX", msgAddr);
        return;
    }

    if (actual2[0] == 0xc7 && actual2[1] == 0x43 && actual2[2] == 0x1c &&
        actual2[3] == targetCap) {
        LOG_INFO("PatchPlayerCap: second patch already applied (cap=%u)", targetCap);
    } else if (memcmp(actual2, expected2, 7) == 0 || actual2[3] == 0x03) {
        DWORD oldProtect2 = 0;
        VirtualProtect((void*)msgAddr, 7, PAGE_EXECUTE_READWRITE, &oldProtect2);
        *((uint8_t*)(msgAddr + 3)) = targetCap;
        VirtualProtect((void*)msgAddr, 7, oldProtect2, &oldProtect2);
        LOG_INFO("PatchPlayerCap: PATCHED exe+0x6ab15e ([RBX+0x1c] 3->%u) - protobuf MaxPlayers=%u",
                 targetCap, targetCap);
    } else {
        LOG_WARNING("PatchPlayerCap: second patch bytes mismatch at exe+0x6ab15b - skipping");
    }
}

bool PlayerSync::Initialize() {
    if (m_initialized) return true;

    LOG_INFO("Initializing player sync...");

    // Verify we have the GameManagerImp address
    auto& resolver = DS2Coop::AddressResolver::GetInstance();
    if (!resolver.GetGameManagerImp()) {
        LOG_WARNING("GameManagerImp not resolved - player sync will use session data only");
    } else {
        LOG_INFO("Player sync will read from GameManagerImp at 0x%p",
                 reinterpret_cast<void*>(resolver.GetGameManagerImp()));
    }

    // Patch out boss-kill phantom dismissal — surgical version.
    // The old PatchPhantomReturnOnBossKill() NOPed the whole call to
    // FUN_140191bb0 and broke death/respawn because that function's
    // epilogue sets a completion flag the death state machine waits for.
    // PatchPhantomDismissalLoops() instead NOPs only the per-phantom
    // dismissal CALLs inside FUN_140191bb0's two iteration loops, so
    // the function still runs to completion and sets the flag.
    PatchPhantomDismissalLoops();

    // Increase player cap from 3 to 6
    PatchPlayerCap();

    // Résoudre ItemGive dès maintenant (scan AOB) pour que GameState::InstallHooks()
    // qui est appelé juste après puisse immédiatement accrocher la fonction.
    // On ne fait que le scan — pas besoin du bag pointer pour ça.
    if (!g_itemGiveScanned) {
        g_itemGiveScanned = true;
        uintptr_t addr = DS2Coop::Utils::PatternScanner::FindPattern(
            ItemGib::ITEM_GIVE_PATTERN,
            ItemGib::ITEM_GIVE_MASK,
            nullptr);
        if (addr) {
            g_itemGiveFunc = reinterpret_cast<void*>(addr);
            LOG_INFO("[ITEM] ItemGive trouvé à %p — hook disponible", reinterpret_cast<void*>(addr));
        } else {
            LOG_WARNING("[ITEM] ItemGive introuvable — item pickup broadcast indisponible");
        }
    }

    m_initialized = true;
    LOG_INFO("Player sync initialized");
    return true;
}

void PlayerSync::Shutdown() {
    if (!m_initialized) return;
    LOG_INFO("Shutting down player sync...");
    m_initialized = false;
}

// ============================================================================
// Read player data directly from game memory
// ============================================================================

static bool ReadPlayerDataBase(uintptr_t& outPlayerData) {
    auto& resolver = DS2Coop::AddressResolver::GetInstance();
    uintptr_t gmImp = resolver.GetGameManagerImp();
    if (!gmImp) return false;

    // GameManagerImp -> +0x38 -> PlayerData pointer
    uintptr_t playerDataPtr = 0;
    if (!Memory::Read<uintptr_t>(gmImp + Offsets::GameManager::PlayerData, &playerDataPtr)) {
        return false;
    }

    if (!playerDataPtr) return false;
    outPlayerData = playerDataPtr;
    return true;
}

static bool ReadCharacterNameRaw(uintptr_t playerData, wchar_t* nameBuf, int maxChars) {
    __try {
        for (int i = 0; i < maxChars; i++) {
            wchar_t ch = 0;
            if (!Memory::Read<wchar_t>(playerData + Offsets::GameManager::CharacterName + i * 2, &ch))
                break;
            if (ch == 0) break;
            nameBuf[i] = ch;
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Try reading a wchar name from an address, convert to UTF-8. Returns "" on failure.
static std::string WcharToUtf8(const wchar_t* buf) {
    if (!buf || buf[0] == 0) return "";
    if (buf[0] < 0x20) return ""; // not printable — likely a pointer, not a name
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "";
    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, &result[0], len, nullptr, nullptr);
    return result;
}

// SEH helper: read name wchars into a plain array, no C++ objects
static bool TryReadNameBuffer(uintptr_t addr, wchar_t* buf, int maxChars) {
    __try {
        for (int i = 0; i < maxChars; i++) {
            wchar_t ch = 0;
            if (!Memory::Read<wchar_t>(addr + i * 2, &ch) || ch == 0) break;
            if (ch < 0x20 || ch > 0x9FFF) return false;
            buf[i] = ch;
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static std::string TryReadNameFrom(uintptr_t addr, const char* source) {
    wchar_t nameBuf[32] = {};
    if (!TryReadNameBuffer(addr, nameBuf, 31)) return "";
    if (nameBuf[0] == 0) return "";
    std::string result = WcharToUtf8(nameBuf);
    if (!result.empty())
        LOG_INFO("[NAME] Character name from %s: %s", source, result.c_str());
    return result;
}

static std::string ReadCharacterName() {
    auto& resolver = DS2Coop::AddressResolver::GetInstance();
    uintptr_t gmImp = resolver.GetGameManagerImp();
    uintptr_t netSession = resolver.GetNetSessionManager();

    // PATH 1: GMImp → [+0xA8] → +0x114 (wchar_t, LOCAL player's own name)
    // Confirmed by Bob Edition CT + DS2S-META: OFLD(ANYSOTFS, STRBASEA, 0xa8, 0x114)
    // This is the GameDataManager path — stores YOUR character name, not the host's.
    if (gmImp) {
        uintptr_t gdm = 0;
        if (Memory::Read<uintptr_t>(gmImp + 0xA8, &gdm) && gdm) {
            std::string name = TryReadNameFrom(gdm + 0x114, "GameDataMgr+0x114");
            if (!name.empty()) return name;
        }
    }

    // PATH 2: NSM → [+0x20] → +0x234 (host/opponent name — fallback only)
    if (netSession) {
        uintptr_t pp = 0;
        if (Memory::Read<uintptr_t>(netSession + 0x20, &pp) && pp) {
            std::string name = TryReadNameFrom(pp + 0x234, "NetSession+0x234");
            if (!name.empty()) return name;
        }
    }

    // PATH 3: GMImp → [+0x38] → +0x24 (PlayerData path — legacy fallback)
    if (gmImp) {
        uintptr_t pd = 0;
        if (Memory::Read<uintptr_t>(gmImp + 0x38, &pd) && pd) {
            std::string name = TryReadNameFrom(pd + 0x24, "PlayerData+0x24");
            if (!name.empty()) return name;
        }
    }

    return "";
}

static bool ReadPlayerPosition(float& x, float& y, float& z, float& rotY) {
    uintptr_t playerData = 0;
    if (!ReadPlayerDataBase(playerData)) return false;

    bool ok = true;
    ok &= Memory::Read<float>(playerData + Offsets::GameManager::PositionX, &x);
    ok &= Memory::Read<float>(playerData + Offsets::GameManager::PositionY, &y);
    ok &= Memory::Read<float>(playerData + Offsets::GameManager::PositionZ, &z);
    // Rotation is optional — don't fail the whole read if it's wrong
    if (!Memory::Read<float>(playerData + Offsets::GameManager::RotationY, &rotY))
        rotY = 0.0f;
    return ok;
}

static bool ReadPlayerHealth(int32_t& health, int32_t& maxHealth) {
    // HP is on PlayerCtrl (GMImp+0xD0), NOT PlayerData (GMImp+0x38).
    // PlayerCtrl + 0x168 = current HP, +0x170 = max HP (SotFS, from DS2S-META).
    auto& resolver = DS2Coop::AddressResolver::GetInstance();
    uintptr_t gmImp = resolver.GetGameManagerImp();
    if (!gmImp) return false;

    uintptr_t playerCtrl = 0;
    if (!Memory::Read<uintptr_t>(gmImp + 0xD0, &playerCtrl) || !playerCtrl) return false;

    bool ok = true;
    ok &= Memory::Read<int32_t>(playerCtrl + 0x168, &health);
    ok &= Memory::Read<int32_t>(playerCtrl + 0x170, &maxHealth);
    return ok;
}

static bool ReadPlayerLevel(uint32_t& level) {
    uintptr_t playerData = 0;
    if (!ReadPlayerDataBase(playerData)) return false;

    return Memory::Read<uint32_t>(playerData + Offsets::GameManager::Level, &level);
}

static bool ReadPlayerStamina(float& stamina) {
    uintptr_t playerData = 0;
    if (!ReadPlayerDataBase(playerData)) return false;

    return Memory::Read<float>(playerData + Offsets::GameManager::Stamina, &stamina);
}

// ============================================================================
// Sync update loop
// ============================================================================

void PlayerSync::Update(float deltaTime) {
    if (!m_initialized) return;

    __try {
        m_positionSyncTimer += deltaTime;
        m_stateSyncTimer += deltaTime;
        m_phantomTimerRefresh += deltaTime;

        if (m_positionSyncTimer >= POSITION_SYNC_INTERVAL) {
            SyncLocalPlayerPosition();
            m_positionSyncTimer = 0.0f;
        }

        if (m_stateSyncTimer >= STATE_SYNC_INTERVAL) {
            SyncLocalPlayerState();
            m_stateSyncTimer = 0.0f;
        }

        // Keep phantom timer maxed every 5s (infrequent is fine)
        if (m_phantomTimerRefresh >= 5.0f) {
            MaxPhantomTimer();
            m_phantomTimerRefresh = 0.0f;
        }

        // Keep permission patches active every 1s — bonfire bits are checked
        // every frame by the game, so 5s gaps cause intermittent blocking.
        static float s_summoningTimer = 0.0f;
        s_summoningTimer += deltaTime;
        if (s_summoningTimer >= 1.0f) {
            EnableSummoning();
            s_summoningTimer = 0.0f;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("PlayerSync::Update CRASHED (exception 0x%08X) — disabling sync",
                  GetExceptionCode());
        m_initialized = false;
    }
}

// ============================================================================
// Position sync - reads ACTUAL game memory
// ============================================================================
void PlayerSync::SyncLocalPlayerPosition() {
    auto& sessionMgr = Session::SessionManager::GetInstance();
    if (!sessionMgr.IsActive()) return;

    // Copy local player data under lock to avoid racing with network thread
    auto players = sessionMgr.GetPlayers();
    Session::SessionPlayer* localPlayer = nullptr;
    uint64_t localId = Network::PeerManager::GetInstance().GetLocalPlayerId();
    for (auto& p : players) {
        if (p.playerId == localId) { localPlayer = &p; break; }
    }
    if (!localPlayer) return;

    float x = 0, y = 0, z = 0, rotY = 0;

    // Try reading from actual game memory first
    if (ReadPlayerPosition(x, y, z, rotY)) {
        // Store back to session manager (the copy is discarded, so write directly)
        sessionMgr.UpdatePlayerPosition(localId, x, y, z);
    } else {
        x = localPlayer->x;
        y = localPlayer->y;
        z = localPlayer->z;
        //LOG_DEBUG("Could not read player position from game memory, using cached");
    }

    // Build and broadcast position packet
    static uint32_t posSequence = 0;
    Network::PlayerPositionPacket packet{};
    packet.header.magic = 0x44533243;
    packet.header.type = Network::PacketType::PlayerPosition;
    packet.header.size = sizeof(Network::PlayerPositionPacket);
    packet.header.sequence = ++posSequence;
    packet.header.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    packet.playerId = localPlayer->playerId;
    packet.x = x;
    packet.y = y;
    packet.z = z;
    packet.rotX = 0.0f;
    packet.rotY = rotY;
    packet.rotZ = 0.0f;
    packet.animation = 0;

    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.BroadcastPacket(&packet.header);

    //LOG_DEBUG("Synced position: (%.2f, %.2f, %.2f)", x, y, z);
}

// ============================================================================
// State sync - reads ACTUAL game memory
// ============================================================================
// Lire les âmes courantes (dépensables) depuis la mémoire du jeu.
static bool ReadPlayerSouls(int32_t& souls) {
    uintptr_t playerData = 0;
    if (!ReadPlayerDataBase(playerData)) return false;
    return Memory::Read<int32_t>(playerData + Offsets::GameManager::Souls, &souls);
}

// Lire le dernier feu de camp (LastBonfire, PlayerData+0x16C)
static bool ReadLastBonfire(uint32_t& bonfireId) {
    uintptr_t playerData = 0;
    if (!ReadPlayerDataBase(playerData)) return false;
    return Memory::Read<uint32_t>(playerData + Offsets::GameManager::LastBonfire, &bonfireId);
}

void PlayerSync::SyncLocalPlayerState() {
    auto& sessionMgr = Session::SessionManager::GetInstance();
    if (!sessionMgr.IsActive()) return;

    auto players = sessionMgr.GetPlayers();
    Session::SessionPlayer* localPlayer = nullptr;
    uint64_t localId = Network::PeerManager::GetInstance().GetLocalPlayerId();
    for (auto& p : players) {
        if (p.playerId == localId) { localPlayer = &p; break; }
    }
    if (!localPlayer) return;

    int32_t health = 0, maxHealth = 0;
    float   stamina = 0;
    uint32_t soulLevel = 0;
    int32_t  souls = 0;

    // Read actual values from game memory
    bool gotHealth = ReadPlayerHealth(health, maxHealth);
    bool gotLevel  = ReadPlayerLevel(soulLevel);
    bool gotSouls  = ReadPlayerSouls(souls);
    ReadPlayerStamina(stamina);

    if (gotHealth) {
        sessionMgr.UpdatePlayerHealth(localId, health, maxHealth);
    } else {
        health = localPlayer->health;
        maxHealth = localPlayer->maxHealth;
    }

    if (gotLevel) {
        sessionMgr.UpdatePlayerLevel(localId, soulLevel);
    } else {
        soulLevel = localPlayer->soulLevel;
    }

    // -----------------------------------------------------------------------
    // Soul-gain detection — "Yui-style" souls distribution
    //
    // We track the previous souls value across calls (static).  When souls
    // increase by more than SOULS_SHARE_MIN (avoids spamming for every
    // small enemy kill while still catching boss rewards), we broadcast a
    // SoulsGrantedPacket so peers also receive the same amount.
    //
    // This fires on the LOCAL player who did the killing; peers apply the
    // delta via ApplySoulsGainToMemory() without re-broadcasting.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // Âmes — détection du delta, Yui-style
    //
    // On partage TOUTE augmentation d'âmes (seuil = 1) :
    //   • petits ennemis → quelques dizaines → partagés
    //   • boss           → milliers          → partagés
    //   • marchands / dépenses → delta négatif → on met à jour le baseline
    //     sans broadcaster (la perte est locale au joueur qui a dépensé).
    //
    // Les paquets sont envoyés au rythme de SyncLocalPlayerState (0.5 s),
    // donc plusieurs kills en 0.5 s sont regroupés en un seul paquet delta.
    // -----------------------------------------------------------------------
    static int32_t s_prevSouls  = -1;  // -1 = non initialisé
    static bool    s_soulsReady = false;

    if (gotSouls) {
        // Sanity check : les âmes DS2 vont de 0 à 99 999 999.
        // Une valeur hors plage indique un mauvais offset mémoire.
        constexpr int32_t SOULS_MAX_SANE = 99999999;
        if (souls < 0 || souls > SOULS_MAX_SANE) {
            static bool s_warnedOffset = false;
            if (!s_warnedOffset) {
                LOG_WARNING("[SOULS] Valeur hors plage (%d) — offset PlayerData+0x%X "
                            "probablement incorrect. Vérifie Bob Edition CT.",
                            souls, Offsets::GameManager::Souls);
                s_warnedOffset = true;
            }
            // Ne pas mettre à jour s_prevSouls sur une lecture invalide
        } else if (!s_soulsReady) {
            // Premier tick valide — on initialise le baseline sans broadcaster
            s_prevSouls  = souls;
            s_soulsReady = true;
            LOG_INFO("[SOULS] Baseline initialisé : %d âmes (PlayerData+0x%X)",
                     souls, Offsets::GameManager::Souls);
        } else if (souls > s_prevSouls) {
            int32_t delta = souls - s_prevSouls;
            s_prevSouls = souls;

            // Partager TOUT gain, même 1 âme
            LOG_DEBUG("[SOULS] +%d âmes détectées — broadcast aux pairs", delta);

            Network::SoulsGrantedPacket sp{};
            sp.header.magic = 0x44533243;
            sp.header.type  = Network::PacketType::SoulsGranted;
            sp.header.size  = sizeof(Network::SoulsGrantedPacket);
            sp.souls        = static_cast<uint32_t>(delta);
            Network::PeerManager::GetInstance().BroadcastPacket(&sp.header);
        } else {
            // Âmes stables ou diminuées (dépense chez un marchand, mort)
            s_prevSouls = souls;
        }
    }

    // -----------------------------------------------------------------------
    // Zone transition detection — "loading screen sync"
    //
    // We monitor two things each tick (0.5 s interval):
    //   1. LastBonfire (PlayerData+0x16C) — changes when the player warps
    //      to a new bonfire OR dies and respawns at the current one.
    //   2. Player position — a warp moves the player by tens/hundreds of
    //      units instantaneously. A respawn at the same bonfire produces a
    //      small jump (< WARP_DISTANCE_THRESHOLD); a warp elsewhere is large.
    //
    // When BOTH change (new bonfire + large position jump), we broadcast a
    // ZoneTransitionPacket so peers can follow the host to the new area.
    //
    // Death respawns produce a small position jump (back to bonfire exit) and
    // do NOT change LastBonfire — so they are NOT broadcast. Each player
    // dies and respawns independently.
    //
    // The ZONE_SYNC_COOLDOWN prevents double-broadcasting if the position is
    // read a second time while still on the loading screen.
    // -----------------------------------------------------------------------
    m_zoneSyncTimer += STATE_SYNC_INTERVAL; // we're called from stateSyncTimer tick

    uint32_t lastBonfire = 0;
    bool gotBonfire = ReadLastBonfire(lastBonfire);

    if (gotBonfire && gotHealth) {
        if (!m_zoneTrackingReady) {
            // First valid read — seed baseline, don't broadcast
            m_prevLastBonfire = lastBonfire;
            float rotDummyInit = 0.0f;
            ReadPlayerPosition(m_prevPosX, m_prevPosY, m_prevPosZ, rotDummyInit);
            m_zoneTrackingReady = true;
        } else if (lastBonfire != m_prevLastBonfire && m_zoneSyncTimer >= ZONE_SYNC_COOLDOWN) {
            // Bonfire changed — check if position also jumped (= real warp, not just
            // a different bonfire being read while standing next to one)
            float cx = 0, cy = 0, cz = 0, rotDummy = 0;
            if (ReadPlayerPosition(cx, cy, cz, rotDummy)) {
                float dx = cx - m_prevPosX;
                float dy = cy - m_prevPosY;
                float dz = cz - m_prevPosZ;
                float dist = sqrtf(dx*dx + dy*dy + dz*dz);

                if (dist > WARP_DISTANCE_THRESHOLD) {
                    LOG_INFO("[ZONE] Warp detected: bonfire %u -> %u, dist=%.1f — broadcasting",
                             m_prevLastBonfire, lastBonfire, dist);

                    // Only host (or any player) broadcasts their own zone change
                    // Peers receive it and execute the warp on their side
                    DS2Coop::Sync::ProgressSync::GetInstance().SyncZoneTransition(lastBonfire, 0);
                    m_zoneSyncTimer = 0.0f;
                } else {
                    LOG_DEBUG("[ZONE] LastBonfire changed but small jump (%.1f) — not a warp", dist);
                }
                m_prevPosX = cx; m_prevPosY = cy; m_prevPosZ = cz;
            }
            m_prevLastBonfire = lastBonfire;
        } else {
            // No bonfire change — just update position baseline for next check
            float cx = 0, cy = 0, cz = 0, rotDummy = 0;
            if (ReadPlayerPosition(cx, cy, cz, rotDummy)) {
                m_prevPosX = cx; m_prevPosY = cy; m_prevPosZ = cz;
            }
        }
    }

    // Build and broadcast standard state packet (HP/stamina/SL for HUD)
    Network::PlayerStatePacket packet{};
    packet.header.magic     = 0x44533243;
    packet.header.type      = Network::PacketType::PlayerState;
    packet.header.size      = sizeof(Network::PlayerStatePacket);
    packet.header.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    packet.playerId   = localPlayer->playerId;
    packet.health     = health;
    packet.maxHealth  = maxHealth;
    packet.stamina    = static_cast<int32_t>(stamina);
    packet.maxStamina = 100;
    packet.souls      = gotSouls ? static_cast<uint32_t>(souls) : 0;
    packet.soulLevel  = soulLevel;

    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.BroadcastPacket(&packet.header);
}

// ============================================================================
// Remote player updates (from network)
// ============================================================================

void PlayerSync::ApplyRemotePlayerPosition(uint64_t playerId, float x, float y, float z,
                                           float rotX, float rotY, float rotZ) {
    //LOG_DEBUG("Remote player position: (%.2f, %.2f, %.2f)", playerId, x, y, z);
    auto& sessionMgr = Session::SessionManager::GetInstance();
    sessionMgr.UpdatePlayerPosition(playerId, x, y, z);
}

void PlayerSync::ApplyRemotePlayerState(uint64_t playerId, int32_t health, int32_t maxHealth,
                                        int32_t stamina, int32_t maxStamina) {
    //LOG_DEBUG("Remote player state: HP %d/%d", playerId, health, maxHealth);
    auto& sessionMgr = Session::SessionManager::GetInstance();
    sessionMgr.UpdatePlayerHealth(playerId, health, maxHealth);
}

void PlayerSync::SyncAnimation(uint64_t playerId, uint32_t animationId) {
    LOG_DEBUG("Animation sync for player %llu: %u", playerId, animationId);
}

void PlayerSync::SyncEquipment(uint64_t playerId) {
    LOG_DEBUG("Equipment sync for player %llu", playerId);
}

// ============================================================================
// Item struct for the game's internal ItemGive function (16 bytes)
// ============================================================================
#pragma pack(push, 1)
struct DS2ItemStruct {
    int32_t  type;       // 3 = consumable
    int32_t  itemId;     // e.g. 0x03B280B0
    float    durability; // FLT_MAX
    int16_t  quantity;   // count
    uint8_t  upgrade;    // 0-10
    uint8_t  infusion;   // 0 = none
};
#pragma pack(pop)

// x64 fastcall: void ItemGive(void* bag, DS2ItemStruct* items, int count, int mode)
typedef void (__fastcall *ItemGiveFunc)(void* bag, DS2ItemStruct* items, int count, int mode);

// (g_itemGiveFunc, g_itemGiveScanned, g_ourItemGiveCall defined at top of file)

// ============================================================================
// Resolve the ItemGive function and AvailableItemBag pointer
// ============================================================================
static bool ResolveItemGive(uintptr_t& outBag) {
    // AOB scan already done in PlayerSync::Initialize() — g_itemGiveScanned guards it.
    // If Initialize() ran first, g_itemGiveFunc is already set (or null if not found).
    if (!g_itemGiveScanned) {
        // Fallback scan (Initialize not yet called — should not normally happen)
        g_itemGiveScanned = true;
        uintptr_t addr = DS2Coop::Utils::PatternScanner::FindPattern(
            ItemGib::ITEM_GIVE_PATTERN,
            ItemGib::ITEM_GIVE_MASK,
            nullptr);
        if (addr) {
            g_itemGiveFunc = reinterpret_cast<void*>(addr);
            LOG_INFO("[ITEM] ItemGive trouvé (scan tardif) à %p", reinterpret_cast<void*>(addr));
        } else {
            LOG_WARNING("[ITEM] ItemGive introuvable — soapstone grant indisponible");
        }
    }

    if (!g_itemGiveFunc) return false;

    auto* typedItemGive = reinterpret_cast<ItemGiveFunc>(g_itemGiveFunc);

    // Resolve AvailableItemBag: [BaseA] -> +0xA8 -> +0x10 -> +0x10
    auto& resolver = DS2Coop::AddressResolver::GetInstance();
    uintptr_t baseA = resolver.GetGameManagerImp();
    if (!baseA) { LOG_WARNING("ResolveItemGive: BaseA is null"); return false; }

    uintptr_t ptr1 = 0, ptr2 = 0, bag = 0;
    if (!Memory::Read<uintptr_t>(baseA + ItemGib::AvailItemBag_Off1, &ptr1) || !ptr1) {
        LOG_WARNING("ResolveItemGive: ptr1 failed (BaseA=%p +0x%X)", (void*)baseA, ItemGib::AvailItemBag_Off1);
        return false;
    }
    if (!Memory::Read<uintptr_t>(ptr1 + ItemGib::AvailItemBag_Off2, &ptr2) || !ptr2) {
        LOG_WARNING("ResolveItemGive: ptr2 failed (ptr1=%p +0x%X)", (void*)ptr1, ItemGib::AvailItemBag_Off2);
        return false;
    }
    if (!Memory::Read<uintptr_t>(ptr2 + ItemGib::AvailItemBag_Off3, &bag) || !bag) {
        LOG_WARNING("ResolveItemGive: bag failed (ptr2=%p +0x%X)", (void*)ptr2, ItemGib::AvailItemBag_Off3);
        return false;
    }

    LOG_INFO("ResolveItemGive: BaseA=%p -> ptr1=%p -> ptr2=%p -> bag=%p",
             (void*)baseA, (void*)ptr1, (void*)ptr2, (void*)bag);
    outBag = bag;
    return true;
}

// ============================================================================
// Grant White Sign Soapstone + Small White Sign Soapstone
// Calls the game's internal ItemGive function
// ============================================================================
bool PlayerSync::GrantSoapstones() {
    uintptr_t bag = 0;
    if (!ResolveItemGive(bag)) {
        LOG_WARNING("GrantSoapstones: could not resolve ItemGive or AvailableItemBag");
        return false;
    }

    DS2ItemStruct items[2] = {};

    // White Sign Soapstone
    items[0].type       = ItemCategory::Consumable;
    items[0].itemId     = ItemIDs::WhiteSignSoapstone;
    items[0].durability = FLT_MAX;
    items[0].quantity   = 1;
    items[0].upgrade    = 0;
    items[0].infusion   = 0;

    // Small White Sign Soapstone
    items[1].type       = ItemCategory::Consumable;
    items[1].itemId     = ItemIDs::SmallWhiteSignSoapstone;
    items[1].durability = FLT_MAX;
    items[1].quantity   = 1;
    items[1].upgrade    = 0;
    items[1].infusion   = 0;

    auto* typedItemGive = reinterpret_cast<ItemGiveFunc>(g_itemGiveFunc);
    LOG_INFO("GrantSoapstones: calling ItemGive at %p with bag=%p, 2 items",
             g_itemGiveFunc, reinterpret_cast<void*>(bag));

    __try {
        // Give items one at a time, flagged so the item-pickup hook ignores our calls
        g_ourItemGiveCall = true;
        typedItemGive(reinterpret_cast<void*>(bag), &items[0], 1, 0);
        LOG_INFO("GrantSoapstones: White Sign Soapstone given");

        typedItemGive(reinterpret_cast<void*>(bag), &items[1], 1, 0);
        LOG_INFO("GrantSoapstones: Small White Sign Soapstone given");
        g_ourItemGiveCall = false;

        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("GrantSoapstones: ItemGive crashed (exception 0x%08X)",
                  GetExceptionCode());
        // Disable further attempts
        g_ourItemGiveCall = false;
        g_itemGiveFunc = nullptr;
        return false;
    }
}

// ============================================================================
// Max out phantom AllottedTime so the summon never expires
// ============================================================================
bool PlayerSync::MaxPhantomTimer() {
    auto& resolver = DS2Coop::AddressResolver::GetInstance();
    uintptr_t netSession = resolver.GetNetSessionManager();
    if (!netSession) {
        LOG_ERROR("MaxPhantomTimer: NetSessionManager not resolved");
        return false;
    }

    // NetSessionManager -> +0x18 (SessionPointer) -> +0x17C (AllottedTime)
    uintptr_t sessionPtr = 0;
    if (!Memory::Read<uintptr_t>(netSession + Offsets::NetSession::SessionPointer, &sessionPtr) || !sessionPtr) {
        LOG_WARNING("MaxPhantomTimer: no active session pointer");
        return false;
    }

    // Set AllottedTime to a huge value (float, in seconds)
    float maxTime = 99999.0f;
    if (Memory::Write<float>(sessionPtr + Offsets::NetSession::AllottedTime, maxTime)) {
        LOG_DEBUG("MaxPhantomTimer: AllottedTime set to %.0f", maxTime);
        return true;
    }

    LOG_DEBUG("MaxPhantomTimer: failed to write AllottedTime");
    return false;
}

// ============================================================================
// Enable summoning regardless of hollow state, and force host-equivalent
// permissions for all players (bonfire, NPC, chest, fog wall access).
// Runs every 5 seconds while seamless is active.
// ============================================================================
void PlayerSync::EnableSummoning() {
    if (!DS2Coop::Hooks::ProtobufHooks::IsSeamlessActive()) return;

    auto& resolver = DS2Coop::AddressResolver::GetInstance();
    uintptr_t gmImp = resolver.GetGameManagerImp();
    if (!gmImp) return;

    // ==========================================================================
    // 1. TeamType patch (CE-verified, 2026-04-02)
    //
    // Heap-allocated uint16 written by DarkSoulsII.exe+0xDF1719.
    // 0=Host, 513=WhitePhantom, 515=Sunbro, 1799=DarkSpirit.
    // Scan for value 513 near player data, cache address, keep writing 0.
    // ==========================================================================
    static uintptr_t s_teamTypeAddr = 0;
    static int s_scanAttempts = 0;
    static bool s_teamTypeLogged = false;

    if (s_teamTypeAddr != 0) {
        __try {
            uint16_t val = 0;
            if (Memory::Read<uint16_t>(s_teamTypeAddr, &val)) {
                if (val == 513 || val == 514 || val == 515 || val == 516) {
                    Memory::Write<uint16_t>(s_teamTypeAddr, (uint16_t)0);
                    if (!s_teamTypeLogged) {
                        LOG_INFO("TeamType PATCHED to Host (was %u) at 0x%llX", val, s_teamTypeAddr);
                        s_teamTypeLogged = true;
                    }
                }
            } else {
                s_teamTypeAddr = 0;
                s_teamTypeLogged = false;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            s_teamTypeAddr = 0;
            s_teamTypeLogged = false;
        }
    } else if (s_scanAttempts <= 10) {
        s_scanAttempts++;
        __try {
            // TeamType est un uint16 dans la structure du joueur, probablement
            // dans les premiers 0x800 octets du bloc PlayerData ou NPM.
            // On réduit la plage pour limiter les faux positifs.
            // Vérification supplémentaire : on ne valide l'adresse que si elle
            // est alignée sur 2 octets ET si la valeur reste stable à 0 après écriture.
            constexpr uintptr_t SCAN_RANGE = 0x800;

            uintptr_t searchBases[4] = {0};
            int baseCount = 0;

            uintptr_t pd = 0;
            if (Memory::Read<uintptr_t>(gmImp + 0x38, &pd) && pd)
                searchBases[baseCount++] = pd;

            uintptr_t npm = 0;
            if (Memory::Read<uintptr_t>(gmImp + 0x10, &npm) && npm) {
                searchBases[baseCount++] = npm;
                uintptr_t lp = 0;
                if (Memory::Read<uintptr_t>(npm + 0x18, &lp) && lp)
                    searchBases[baseCount++] = lp;
            }

            for (int b = 0; b < baseCount && s_teamTypeAddr == 0; b++) {
                uintptr_t base = searchBases[b];
                for (uintptr_t offset = 0; offset < SCAN_RANGE; offset += 2) {
                    uint16_t val = 0;
                    if (!Memory::Read<uint16_t>(base + offset, &val) || val != 513)
                        continue;

                    // Écrire 0, relire — si une autre valeur revient immédiatement
                    // c'est un faux positif (champ vivant écrit en permanence par le jeu)
                    Memory::Write<uint16_t>(base + offset, (uint16_t)0);
                    uint16_t check = 0;
                    Memory::Read<uint16_t>(base + offset, &check);
                    if (check != 0) {
                        // Le jeu a réécrit la valeur immédiatement — mauvaise adresse
                        LOG_DEBUG("TeamType: false positive at base+0x%llX (val restored to %u)", offset, check);
                        continue;
                    }

                    s_teamTypeAddr = base + offset;
                    LOG_INFO("TeamType FOUND at 0x%llX (base+0x%llX), set to 0",
                             s_teamTypeAddr, offset);
                    break;
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ==========================================================================
    // 2. Hollowing — DISABLED: caused crash (wrote 0 to wrong field, value was
    // 244 which suggests offset 0x1AC is not hollowing in this build).
    // Needs CE re-verification before re-enabling.
    // ==========================================================================

    // ==========================================================================
    // 3. Phantom field — Bob Edition CT: NetSessionManager → [+0x20] → +0x1F4
    //
    // This is the field the game checks to determine if the local player is a
    // phantom. Zeroing it makes the game treat the local player as the host
    // for permission checks (bonfire, NPC, chest, fog wall).
    // ==========================================================================
    uintptr_t netSession = resolver.GetNetSessionManager();
    if (netSession) {
        __try {
            uintptr_t playerPtr = 0;
            if (Memory::Read<uintptr_t>(netSession + Offsets::NetSession::PlayerPointer, &playerPtr) && playerPtr) {
                uint32_t phantomField = 0;
                if (Memory::Read<uint32_t>(playerPtr + 0x1F4, &phantomField) && phantomField != 0) {
                    Memory::Write<uint32_t>(playerPtr + 0x1F4, (uint32_t)0);
                    LOG_INFO("EnableSummoning: Phantom field zeroed (was %u) via [NSM+0x20]+0x1F4", phantomField);
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ==========================================================================
    // 4. Session-slot TeamType — DISABLED: value 127 at [NSM+0x20][+0x1E8][slot*8][+0xB0]+0x4D
    // is not TeamType — wrong pointer chain. Zeroing it corrupted game state → crash.
    // Needs CE re-verification before re-enabling.
    // ==========================================================================

    // ==========================================================================
    // 5. Bonfire access bits — Bob Edition CT:
    //    GameManagerImp → [+0xD0] → [+0xB8] → +0x4C8, bits 4+5
    //
    // bit 4 = isBonfireStart, bit 5 = isBonfireLoop.
    // The host bonfire restriction checks phantom count > 0. Separately,
    // the phantom-side restriction checks these bits. Setting both forces
    // the game to allow bonfire interaction regardless of session state.
    // ==========================================================================
    __try {
        uintptr_t ptr_d0 = 0, ptr_b8 = 0;
        if (Memory::Read<uintptr_t>(gmImp + 0xD0, &ptr_d0) && ptr_d0) {
            if (Memory::Read<uintptr_t>(ptr_d0 + 0xB8, &ptr_b8) && ptr_b8) {
                uint32_t flags = 0;
                if (Memory::Read<uint32_t>(ptr_b8 + 0x4C8, &flags)) {
                    uint32_t newFlags = flags | (1u << 4) | (1u << 5);
                    if (newFlags != flags) {
                        Memory::Write<uint32_t>(ptr_b8 + 0x4C8, newFlags);
                        LOG_INFO("EnableSummoning: isBonfireStart/Loop bits set (0x%X -> 0x%X)", flags, newFlags);
                    }
                }
            }

            // ==========================================================================
            // 4. ChrNetworkPhantomId — Bob Edition CT:
            //    GameManagerImp → [+0xD0] → [+0xB0] → +0x3C (byte)
            //
            // This byte controls phantom rendering (ghostly white appearance).
            // 0 = normal player rendering, non-zero = phantom appearance.
            // Zeroing it makes all players appear as solid/normal.
            // ==========================================================================
            uintptr_t ptr_b0 = 0;
            if (Memory::Read<uintptr_t>(ptr_d0 + 0xB0, &ptr_b0) && ptr_b0) {
                uint8_t phantomId = 0;
                if (Memory::Read<uint8_t>(ptr_b0 + 0x3C, &phantomId) && phantomId != 0) {
                    Memory::Write<uint8_t>(ptr_b0 + 0x3C, (uint8_t)0);
                    LOG_INFO("EnableSummoning: ChrNetworkPhantomId zeroed (was %u) — solid appearance", phantomId);
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

std::string PlayerSync::GetLocalCharacterName() {
    return ReadCharacterName();
}

// ============================================================================
// TeleportToHost
//
// Writes the host's last-known position directly into the local player's
// PlayerData position fields (X/Y/Z at +0x30/+0x34/+0x38).
//
// This is a "soft teleport" — the game physics engine will accept the new
// coordinates on the next frame. Works within the same area; if the host is
// in a different zone, the player should use the zone-sync / bonfire warp
// instead. We add +1.0 on Y to avoid embedding the player in the ground.
//
// Built-in cooldown: 10 seconds between uses to prevent spam-clicking.
// ============================================================================
bool PlayerSync::TeleportToHost() {
    // ── Cooldown ─────────────────────────────────────────────────────────────
    static DWORD s_lastTeleportMs = 0;
    constexpr DWORD COOLDOWN_MS = 10000; // 10 seconds
    DWORD now = GetTickCount();
    if (now - s_lastTeleportMs < COOLDOWN_MS) {
        DWORD remaining = (COOLDOWN_MS - (now - s_lastTeleportMs)) / 1000 + 1;
        LOG_INFO("[TP] Emergency teleport on cooldown (%us remaining)", remaining);
        DS2Coop::UI::Overlay::GetInstance().ShowNotification(
            std::string("Téléportation : patientez encore ")
            + std::to_string(remaining) + "s.", 2.5f);
        return false;
    }

    // ── Find host in session ──────────────────────────────────────────────────
    auto& sessionMgr = DS2Coop::Session::SessionManager::GetInstance();
    if (!sessionMgr.IsActive()) {
        LOG_WARNING("[TP] TeleportToHost: no active session");
        return false;
    }

    auto players = sessionMgr.GetPlayers();
    uint64_t localId = Network::PeerManager::GetInstance().GetLocalPlayerId();

    // Find the host: the first player that is not us, or the one marked as host.
    // SessionManager stores players in join order — host is at index 0 if present.
    const DS2Coop::Session::SessionPlayer* hostPlayer = nullptr;
    for (const auto& p : players) {
        if (p.playerId != localId && p.playerId != 0) {
            hostPlayer = &p;
            break;
        }
    }

    if (!hostPlayer) {
        LOG_WARNING("[TP] TeleportToHost: host not found in player list");
        DS2Coop::UI::Overlay::GetInstance().ShowNotification(
            "Téléportation impossible : hôte introuvable.", 4.0f);
        return false;
    }

    float tx = hostPlayer->x;
    float ty = hostPlayer->y;
    float tz = hostPlayer->z;

    // Reject if host position is uninitialized (all zeros = not received yet)
    if (tx == 0.0f && ty == 0.0f && tz == 0.0f) {
        LOG_WARNING("[TP] TeleportToHost: host position not yet received");
        DS2Coop::UI::Overlay::GetInstance().ShowNotification(
            "Position de l'hôte inconnue — réessayez dans quelques secondes.", 4.0f);
        return false;
    }

    // ── Write position to local PlayerData ───────────────────────────────────
    uintptr_t playerData = 0;
    if (!ReadPlayerDataBase(playerData)) {
        LOG_WARNING("[TP] TeleportToHost: PlayerData not readable");
        DS2Coop::UI::Overlay::GetInstance().ShowNotification(
            "Téléportation impossible : données joueur non accessibles.", 4.0f);
        return false;
    }

    // Slight Y offset so we land on top of the host rather than inside them
    float writeY = ty + 1.2f;

    bool ok = true;
    ok &= Memory::Write<float>(playerData + Offsets::GameManager::PositionX, tx);
    ok &= Memory::Write<float>(playerData + Offsets::GameManager::PositionY, writeY);
    ok &= Memory::Write<float>(playerData + Offsets::GameManager::PositionZ, tz);

    if (ok) {
        s_lastTeleportMs = now;
        LOG_INFO("[TP] Teleported to host '%s' at (%.1f, %.1f, %.1f)",
                 hostPlayer->playerName.c_str(), tx, writeY, tz);
        DS2Coop::UI::Overlay::GetInstance().ShowNotification(
            std::string("Téléporté sur ") + hostPlayer->playerName + " !", 3.0f);
        return true;
    } else {
        LOG_ERROR("[TP] TeleportToHost: position write failed (pd=0x%llX)", playerData);
        DS2Coop::UI::Overlay::GetInstance().ShowNotification(
            "Téléportation échouée — écriture mémoire refusée.", 4.0f);
        return false;
    }
}
