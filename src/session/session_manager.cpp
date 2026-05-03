// Session manager - tracks players, drives sync loops
//
// Coordinates between PeerManager (networking) and PlayerSync/ProgressSync.
// Session creation/joining is fully async via Steam lobbies:
//   CreateSession → SteamMM::CreateLobby (async) → SetLobbyData → Connected
//   JoinSession   → RequestLobbyList (async) → JoinLobby (async) → P2P handshake → Connected

#include "../../include/session.h"
#include "../../include/network.h"
#include "../../include/hooks.h"
#include "../../include/sync.h"
#include "../../include/ui.h"
#include "../../include/utils.h"
#include "../../include/mod.h"
#include "../../third_party/steamworks/steam_minimal.h"
#include <algorithm>
#include <fstream>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <chrono>

using namespace DS2Coop::Session;
using namespace DS2Coop::Utils;

// ============================================================================
// SEH-safe Steam vtable wrappers
//
// The Steam vtable calls can crash if slot numbers are wrong for this
// specific steam_api64.dll version, or if the interface pointer is bad.
// These C-style helpers wrap each call in __try/__except so a bad vtable
// call is caught and logged rather than crashing the game.
//
// IMPORTANT: __try/__except cannot be used in C++ functions that have
// objects with destructors in scope (MSVC C2712). These are pure C-style.
// ============================================================================

// Safe uint64 vtable call — used for SteamAPICall_t return values
static uint64_t SafeVCall0(void* iface, int slot) {
    __try {
        using Fn = uint64_t(*)(void*);
        auto vtable = *reinterpret_cast<void***>(iface);
        return reinterpret_cast<Fn>(vtable[slot])(iface);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("SafeVCall0: crash at slot %d — vtable mismatch?", slot);
        return 0;
    }
}
static uint64_t SafeVCall1i(void* iface, int slot, int arg0) {
    __try {
        using Fn = uint64_t(*)(void*, int);
        auto vtable = *reinterpret_cast<void***>(iface);
        return reinterpret_cast<Fn>(vtable[slot])(iface, arg0);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("SafeVCall1i: crash at slot %d", slot);
        return 0;
    }
}
static uint64_t SafeVCall2u64(void* iface, int slot, uint64_t arg0) {
    __try {
        using Fn = uint64_t(*)(void*, uint64_t);
        auto vtable = *reinterpret_cast<void***>(iface);
        return reinterpret_cast<Fn>(vtable[slot])(iface, arg0);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("SafeVCall2u64: crash at slot %d", slot);
        return 0;
    }
}
static uint64_t SafeVCall2u64i(void* iface, int slot, uint64_t arg0, int arg1) {
    __try {
        using Fn = uint64_t(*)(void*, uint64_t, int);
        auto vtable = *reinterpret_cast<void***>(iface);
        return reinterpret_cast<Fn>(vtable[slot])(iface, arg0, arg1);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("SafeVCall2u64i: crash at slot %d", slot);
        return 0;
    }
}
static void SafeVCallStringFilter(void* iface, int slot,
                                  const char* key, const char* val, int cmp) {
    __try {
        using Fn = void(*)(void*, const char*, const char*, int);
        auto vtable = *reinterpret_cast<void***>(iface);
        reinterpret_cast<Fn>(vtable[slot])(iface, key, val, cmp);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("SafeVCallStringFilter: crash at slot %d", slot);
    }
}
static bool SafeVCallSetLobbyData(void* iface, int slot,
                                   uint64_t lobby, const char* key, const char* val) {
    __try {
        using Fn = bool(*)(void*, uint64_t, const char*, const char*);
        auto vtable = *reinterpret_cast<void***>(iface);
        return reinterpret_cast<Fn>(vtable[slot])(iface, lobby, key, val);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("SafeVCallSetLobbyData: crash at slot %d", slot);
        return false;
    }
}
static void SafeVCallLeaveLobby(void* iface, int slot, uint64_t lobby) {
    __try {
        using Fn = void(*)(void*, uint64_t);
        auto vtable = *reinterpret_cast<void***>(iface);
        reinterpret_cast<Fn>(vtable[slot])(iface, lobby);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("SafeVCallLeaveLobby: crash at slot %d", slot);
    }
}

// ============================================================================
// Persistance du dernier mot de passe pour le bouton "Rejoindre dernière session"
// ============================================================================
static const char* LAST_PASSWORD_FILE = "ds2_last_session.ini";

static void SaveLastPassword(const std::string& password) {
    std::ofstream f(LAST_PASSWORD_FILE);
    if (f.is_open()) {
        f << "password=" << password << "\n";
    }
}

static void ClearLastPassword() {
    std::remove(LAST_PASSWORD_FILE);
}

static bool LoadLastPassword(std::string& outPassword) {
    std::ifstream f(LAST_PASSWORD_FILE);
    if (!f.is_open()) return false;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "password") outPassword = val;
    }
    return !outPassword.empty();
}

SessionManager& SessionManager::GetInstance() {
    static SessionManager instance;
    return instance;
}

bool SessionManager::Initialize() {
    if (m_initialized) return true;

    LOG_INFO("Initializing session manager...");

    m_state = SessionState::Disconnected;
    m_initialized = true;

    LOG_INFO("Session manager initialized");
    return true;
}

void SessionManager::Shutdown() {
    if (!m_initialized) return;

    LOG_INFO("Shutting down session manager...");

    LeaveSession();
    m_initialized = false;

    LOG_INFO("Session manager shut down");
}

bool SessionManager::CreateSession(const std::string& password) {
    if (!m_initialized) return false;
    if (m_state == SessionState::Connecting || m_state == SessionState::Connected ||
        m_state == SessionState::InGame) {
        LOG_WARNING("CreateSession called while already in state %d", (int)m_state);
        return false;
    }

    LOG_INFO("Creating seamless co-op session (host)...");

    m_isHost          = true;
    m_sessionPassword = password;
    m_localPlayerId   = 0;

    // Initialize sync systems
    Sync::PlayerSync::GetInstance().Initialize();
    Sync::ProgressSync::GetInstance().Initialize();

    // Add local player entry
    SessionPlayer localPlayer{};
    localPlayer.playerId   = 0;
    localPlayer.isAlive    = true;
    localPlayer.isReady    = true;
    localPlayer.soulLevel  = 0;
    localPlayer.health = localPlayer.maxHealth = 0;
    localPlayer.x = localPlayer.y = localPlayer.z = 0.0f;
    std::string charName = Sync::PlayerSync::GetInstance().GetLocalCharacterName();
    localPlayer.playerName = charName.empty() ? "Host" : charName;
    {
        std::lock_guard<std::mutex> lock(m_playersMutex);
        m_players.push_back(localPlayer);
    }

    Hooks::ProtobufHooks::ClearSessionSteamIds();
    Hooks::GameState::ClearBroadcastFlagCache();

    // Activate disconnect blocking immediately
    Hooks::ProtobufHooks::SetSeamlessActive(true);

    // Try to create a Steam lobby for discovery (so guest can find us by password).
    // Uses SEH-safe wrappers — a vtable crash is caught and logged, not fatal.
    void* mm = SteamAPI::Matchmaking();
    if (mm) {
        LOG_INFO("  Steam available — creating invisible lobby (password='%s')...", password.c_str());
        // ISteamMatchmaking slot 12 = CreateLobby(ELobbyType, int maxMembers)
        // k_ELobbyTypeInvisible (3) = not listed publicly, only joinable by password
        m_createLobbyCall = SafeVCall2u64i(mm, 12, 3 /*k_ELobbyTypeInvisible*/,
                                           (int)SeamlessCoopMod::GetInstance().GetConfig().max_players);
        if (m_createLobbyCall != 0) {
            LOG_INFO("  Lobby creation async call started (handle=%llu)", m_createLobbyCall);
            // State stays at Connecting until LobbyCreated_t callback fires
            TransitionToState(SessionState::Connecting);
        } else {
            LOG_WARNING("  CreateLobby returned null handle — vtable slot wrong? Fallback to local mode");
            TransitionToState(SessionState::Connected);
        }
    } else {
        LOG_WARNING("  ISteamMatchmaking not available — seamless active without lobby discovery");
        TransitionToState(SessionState::Connected);
    }

    SaveLastPassword(password);
    LOG_INFO("Seamless co-op ACTIVE (host) — disconnect blocking ON");
    LOG_INFO("  Invite friend: same password '%s' on their side, then JOIN", password.c_str());
    LOG_INFO("  Until SpawnPhantom is found: use white soapstone once to connect");
    return true;
}

bool SessionManager::JoinSession(const std::string& password) {
    if (!m_initialized) return false;
    if (m_state == SessionState::Connecting || m_state == SessionState::Connected ||
        m_state == SessionState::InGame) {
        LOG_WARNING("JoinSession called while already in state %d", (int)m_state);
        return false;
    }

    LOG_INFO("Joining seamless co-op session (guest, password='%s')...", password.c_str());

    m_isHost          = false;
    m_sessionPassword = password;
    m_pendingPassword = password;

    // Initialize sync systems BEFORE joining so the handshake has the real name
    Sync::PlayerSync::GetInstance().Initialize();
    Sync::ProgressSync::GetInstance().Initialize();

    m_localPlayerId = 0;

    // Add local player entry
    SessionPlayer localPlayer{};
    localPlayer.playerId   = 0;
    localPlayer.isAlive    = true;
    localPlayer.isReady    = true;
    localPlayer.soulLevel  = 0;
    localPlayer.health = localPlayer.maxHealth = 0;
    localPlayer.x = localPlayer.y = localPlayer.z = 0.0f;
    std::string charName = Sync::PlayerSync::GetInstance().GetLocalCharacterName();
    localPlayer.playerName = charName.empty() ? "Player" : charName;
    {
        std::lock_guard<std::mutex> lock(m_playersMutex);
        m_players.push_back(localPlayer);
    }

    Hooks::ProtobufHooks::ClearSessionSteamIds();
    Hooks::GameState::ClearBroadcastFlagCache();

    // Activate disconnect blocking immediately
    Hooks::ProtobufHooks::SetSeamlessActive(true);

    // Try to find the host's Steam lobby by password.
    void* mm = SteamAPI::Matchmaking();
    if (mm) {
        LOG_INFO("  Steam available — searching for lobby with password '%s'...", password.c_str());
        // ISteamMatchmaking slot 5  = AddRequestLobbyListStringFilter(key, val, cmp)
        // ISteamMatchmaking slot 10 = AddRequestLobbyListResultCountFilter(count)
        // ISteamMatchmaking slot 4  = RequestLobbyList()
        SafeVCallStringFilter(mm, 5, "ds2coop_pw", password.c_str(), 0 /*k_ELobbyComparisonEqual*/);
        SafeVCall1i(mm, 10, 1); // limit to 1 result
        m_requestLobbyCall = SafeVCall0(mm, 4);
        if (m_requestLobbyCall != 0) {
            LOG_INFO("  Lobby search started (handle=%llu)", m_requestLobbyCall);
            TransitionToState(SessionState::Connecting);
        } else {
            LOG_WARNING("  RequestLobbyList returned null — vtable slot wrong? Fallback to local mode");
            TransitionToState(SessionState::Connected);
        }
    } else {
        LOG_WARNING("  ISteamMatchmaking not available — seamless active without lobby discovery");
        TransitionToState(SessionState::Connected);
    }

    SaveLastPassword(password);
    LOG_INFO("Seamless co-op ACTIVE (guest) — disconnect blocking ON");
    return true;
}

void SessionManager::LeaveSession() {
    if (m_state == SessionState::Disconnected) return;

    LOG_INFO("Leaving session...");

    // Disable seamless disconnect blocking
    Hooks::ProtobufHooks::SetSeamlessActive(false);

    // Leave Steam lobby if we're in one
    if (m_currentLobbyId != 0) {
        void* mm = SteamAPI::Matchmaking();
        if (mm) {
            SafeVCallLeaveLobby(mm, 14, m_currentLobbyId); // slot 14 = LeaveLobby
        }
    }

    m_createLobbyCall  = 0;
    m_requestLobbyCall = 0;
    m_joinLobbyCall    = 0;
    m_currentLobbyId   = 0;

    ClearLastPassword();
    Hooks::GameState::ClearBroadcastFlagCache();

    {
        std::lock_guard<std::mutex> lock(m_playersMutex);
        m_players.clear();
    }

    TransitionToState(SessionState::Disconnected);
    LOG_INFO("Left session");
}

// ── Safe Steam API-call polling helpers ──────────────────────────────────────
// Avoids putting C++ objects (LobbyCreated_t etc.) in scope during __try block.
static bool SafeIsCallCompleted(void* utils, uint64_t call, bool* failed) {
    *failed = false;
    __try {
        using Fn = bool(*)(void*, uint64_t, bool*);
        auto vtable = *reinterpret_cast<void***>(utils);
        return reinterpret_cast<Fn>(vtable[11])(utils, call, failed); // slot 11 = IsAPICallCompleted
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("SafeIsCallCompleted: vtable crash");
        *failed = true;
        return true; // treat as completed with failure
    }
}
static bool SafeGetCallResult(void* utils, uint64_t call,
                               void* outBuf, int bufSize, int cbId, bool* failed) {
    *failed = false;
    __try {
        using Fn = bool(*)(void*, uint64_t, void*, int, int, bool*);
        auto vtable = *reinterpret_cast<void***>(utils);
        return reinterpret_cast<Fn>(vtable[13])(utils, call, outBuf, bufSize, cbId, failed); // slot 13 = GetAPICallResult
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("SafeGetCallResult: vtable crash");
        *failed = true;
        return false;
    }
}

void SessionManager::Update(float deltaTime) {
    // Always pump Steam's callback queue
    SteamAPI::RunCallbacks();

    // Poll async Steam lobby operations
    if (m_state == SessionState::Connecting || m_state == SessionState::Connected ||
        m_state == SessionState::InGame) {
        void* utils = SteamAPI::Utils();
        void* mm    = SteamAPI::Matchmaking();

        if (utils && mm) {
            bool failed = false;

            // ── Create lobby result (host) ────────────────────────────────
            if (m_createLobbyCall != 0 &&
                SafeIsCallCompleted(utils, m_createLobbyCall, &failed)) {

                LobbyCreated_t result{};
                SafeGetCallResult(utils, m_createLobbyCall,
                    &result, sizeof(result), LobbyCreated_t::k_iCallback, &failed);
                m_createLobbyCall = 0;

                if (!failed && result.m_eResult == 1 /* k_EResultOK */) {
                    m_currentLobbyId = result.m_ulSteamIDLobby;
                    LOG_INFO("Steam lobby created: %llu", m_currentLobbyId);

                    // Tag lobby with password — slot 20 = SetLobbyData(lobby, key, val)
                    SafeVCallSetLobbyData(mm, 20, m_currentLobbyId,
                                         "ds2coop_pw", m_sessionPassword.c_str());

                    TransitionToState(SessionState::Connected);
                    UI::Overlay::GetInstance().ShowNotification(
                        "Lobby cree ! Ton ami peut rejoindre avec le meme mot de passe.", 6.0f);
                    UI::Overlay::GetInstance().ShowCenteredNotification(
                        "Mode seamless ACTIF", 4.0f, 1);
                } else {
                    LOG_ERROR("CreateLobby failed (eResult=%u, failed=%d)",
                              result.m_eResult, (int)failed);
                    // Don't go to Error — we still have protobuf hooks active
                    TransitionToState(SessionState::Connected);
                    UI::Overlay::GetInstance().ShowNotification(
                        "Lobby Steam indisponible — mode seamless actif sans decouverte.", 5.0f);
                }
            }

            // ── RequestLobbyList result (joiner) ──────────────────────────
            if (m_requestLobbyCall != 0 &&
                SafeIsCallCompleted(utils, m_requestLobbyCall, &failed)) {

                LobbyMatchList_t result{};
                SafeGetCallResult(utils, m_requestLobbyCall,
                    &result, sizeof(result), LobbyMatchList_t::k_iCallback, &failed);
                m_requestLobbyCall = 0;

                if (!failed && result.m_nLobbiesMatching > 0) {
                    // slot 11 = GetLobbyByIndex(int) → returns CSteamID as uint64
                    // We pass it as uint64 to avoid CSteamID hidden-pointer ABI issue
                    uint64_t lobbyId = SafeVCall1i(mm, 11, 0);
                    if (lobbyId != 0) {
                        // slot 13 = JoinLobby(CSteamID) → returns SteamAPICall_t
                        m_joinLobbyCall = SafeVCall2u64(mm, 13, lobbyId);
                        LOG_INFO("Found %u lobby(ies) — joining %llu...",
                                 result.m_nLobbiesMatching, lobbyId);
                    } else {
                        LOG_ERROR("GetLobbyByIndex returned 0");
                        TransitionToState(SessionState::Connected);
                    }
                } else {
                    LOG_WARNING("No lobby found for password '%s' (count=%u)",
                                m_sessionPassword.c_str(), result.m_nLobbiesMatching);
                    // Stay Connected — protobuf hooks are still active
                    TransitionToState(SessionState::Connected);
                    UI::Overlay::GetInstance().ShowNotification(
                        "Aucun lobby trouve (hote pas encore pret?) — seamless actif.", 5.0f);
                }
            }

            // ── JoinLobby result (joiner) ─────────────────────────────────
            if (m_joinLobbyCall != 0 &&
                SafeIsCallCompleted(utils, m_joinLobbyCall, &failed)) {

                LobbyEnter_t result{};
                SafeGetCallResult(utils, m_joinLobbyCall,
                    &result, sizeof(result), LobbyEnter_t::k_iCallback, &failed);
                m_joinLobbyCall = 0;

                if (!failed && result.m_EChatRoomEnterResponse == 1) {
                    m_currentLobbyId = result.m_ulSteamIDLobby;
                    LOG_INFO("Joined lobby %llu", m_currentLobbyId);
                    TransitionToState(SessionState::Connected);
                    UI::Overlay::GetInstance().ShowNotification(
                        "Lobby rejoint ! Utilise la pierre blanche pour invoquer.", 6.0f);
                    UI::Overlay::GetInstance().ShowCenteredNotification(
                        "Mode seamless ACTIF", 4.0f, 1);
                } else {
                    LOG_ERROR("JoinLobby failed (response=%u, failed=%d)",
                              result.m_EChatRoomEnterResponse, (int)failed);
                    TransitionToState(SessionState::Connected);
                    UI::Overlay::GetInstance().ShowNotification(
                        "Lobby introuvable — seamless actif, utilise la pierre blanche.", 5.0f);
                }
            }
        }
    }

    if (!IsActive()) return;

    // Update player sync (reads game memory: HP, position, etc.)
    auto& playerSync = Sync::PlayerSync::GetInstance();
    playerSync.Update(deltaTime);
}

// ============================================================================
// Player management
// ============================================================================

void SessionManager::AddPlayer(uint64_t playerId, const std::string& name) {
    if (playerId == m_localPlayerId) return;

    std::lock_guard<std::mutex> lock(m_playersMutex);

    for (const auto& p : m_players) {
        if (p.playerId == playerId) {
            LOG_DEBUG("Player %llu already in session", playerId);
            return;
        }
    }

    SessionPlayer newPlayer{};
    newPlayer.playerId    = playerId;
    newPlayer.playerName  = name;
    newPlayer.isAlive     = true;
    newPlayer.isReady     = true;
    newPlayer.soulLevel   = 0;
    newPlayer.health      = 0;
    newPlayer.maxHealth   = 0;
    newPlayer.x = newPlayer.y = newPlayer.z = 0.0f;

    m_players.push_back(newPlayer);

    LOG_INFO("Player joined session: %s (ID: %llu) [Total: %zu players]",
             name.c_str(), playerId, m_players.size());

    PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC);

    Hooks::ProtobufHooks::SetSeamlessActive(true);
}

void SessionManager::RemovePlayer(uint64_t playerId) {
    std::lock_guard<std::mutex> lock(m_playersMutex);

    std::string name;
    for (const auto& p : m_players) {
        if (p.playerId == playerId) { name = p.playerName; break; }
    }

    m_players.erase(
        std::remove_if(m_players.begin(), m_players.end(),
            [playerId](const SessionPlayer& p) { return p.playerId == playerId; }),
        m_players.end()
    );

    Network::PacketHandler::GetInstance().RemovePlayer(playerId);

    LOG_INFO("Player left session: %s (ID: %llu) [Total: %zu players]",
             name.c_str(), playerId, m_players.size());

    if (!name.empty())
        PlaySoundW(L"SystemExclamation", nullptr, SND_ALIAS | SND_ASYNC);

    if (m_players.size() <= 1) {
        LOG_INFO("All remote players gone — scheduling seamless mode deactivation in 5s");
        UI::Overlay::GetInstance().ShowNotification("Session empty. Waiting for players...", 5.0f);

        CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            Sleep(5000);
            auto& sm      = DS2Coop::Session::SessionManager::GetInstance();
            auto  players = sm.GetPlayers();
            auto* local   = sm.GetLocalPlayer();
            uint64_t localId = local ? local->playerId : 0;
            bool stillAlone  = true;
            for (const auto& p : players) {
                if (p.playerId != localId) { stillAlone = false; break; }
            }
            if (stillAlone) {
                DS2Coop::Hooks::ProtobufHooks::SetSeamlessActive(false);
                LOG_INFO("Seamless mode DEACTIVATED — session is empty");
            }
            return 0;
        }, nullptr, 0, nullptr);
    }
}

SessionPlayer* SessionManager::GetPlayer(uint64_t playerId) {
    for (auto& p : m_players)
        if (p.playerId == playerId) return &p;
    return nullptr;
}

SessionPlayer* SessionManager::GetLocalPlayer() {
    return GetPlayer(m_localPlayerId);
}

void SessionManager::UpdatePlayerPosition(uint64_t playerId, float x, float y, float z) {
    std::lock_guard<std::mutex> lock(m_playersMutex);
    SessionPlayer* player = GetPlayer(playerId);
    if (player) { player->x = x; player->y = y; player->z = z; }
}

void SessionManager::UpdatePlayerHealth(uint64_t playerId, int32_t health, int32_t maxHealth) {
    std::lock_guard<std::mutex> lock(m_playersMutex);
    SessionPlayer* player = GetPlayer(playerId);
    if (player) {
        player->health    = health;
        player->maxHealth = maxHealth;
        player->isAlive   = (health > 0);
    }
}

void SessionManager::UpdatePlayerLevel(uint64_t playerId, uint32_t soulLevel) {
    std::lock_guard<std::mutex> lock(m_playersMutex);
    SessionPlayer* player = GetPlayer(playerId);
    if (player) player->soulLevel = soulLevel;
}

void SessionManager::UpdatePlayerPing(uint64_t playerId, uint32_t ping_ms) {
    std::lock_guard<std::mutex> lock(m_playersMutex);
    SessionPlayer* player = GetPlayer(playerId);
    if (player) player->ping_ms = ping_ms;
}

void SessionManager::NotifyPlayerDeath(uint64_t playerId) {
    bool shouldBroadcast = false;
    {
        std::lock_guard<std::mutex> lock(m_playersMutex);
        SessionPlayer* player = GetPlayer(playerId);
        if (player) {
            player->isAlive = false;
            LOG_INFO("Player %s died", player->playerName.c_str());
            shouldBroadcast = true;
        }
    }
    if (shouldBroadcast) {
        Network::PacketHeader deathPacket{};
        deathPacket.magic  = 0x44533243;
        deathPacket.type   = Network::PacketType::PlayerDeath;
        deathPacket.size   = sizeof(Network::PacketHeader);
        deathPacket.timestamp = 0;
        Network::PeerManager::GetInstance().BroadcastPacket(&deathPacket);
    }
}

void SessionManager::NotifyPlayerRespawn(uint64_t playerId) {
    bool shouldBroadcast = false;
    {
        std::lock_guard<std::mutex> lock(m_playersMutex);
        SessionPlayer* player = GetPlayer(playerId);
        if (player) {
            player->isAlive = true;
            LOG_INFO("Player %s respawned", player->playerName.c_str());
            shouldBroadcast = true;
        }
    }
    if (shouldBroadcast) {
        Network::PacketHeader respawnPacket{};
        respawnPacket.magic  = 0x44533243;
        respawnPacket.type   = Network::PacketType::PlayerRespawn;
        respawnPacket.size   = sizeof(Network::PacketHeader);
        respawnPacket.timestamp = 0;
        Network::PeerManager::GetInstance().BroadcastPacket(&respawnPacket);
    }
}

void SessionManager::OnBossDefeated(uint32_t bossId) {
    LOG_INFO("Boss %u defeated in session", bossId);

    Network::BossDefeatedPacket packet{};
    packet.header.magic = 0x44533243;
    packet.header.type  = Network::PacketType::BossDefeated;
    packet.header.size  = sizeof(Network::BossDefeatedPacket);
    packet.bossId       = bossId;
    packet.defeatTime   = 0;

    Network::PeerManager::GetInstance().BroadcastPacket(&packet.header);
}

void SessionManager::OnBonfireRested(uint32_t bonfireId) {
    LOG_INFO("Bonfire %u rested", bonfireId);

    Network::EventFlagPacket packet{};
    packet.header.magic = 0x44533243;
    packet.header.type  = Network::PacketType::BonfireRest;
    packet.header.size  = sizeof(Network::EventFlagPacket);
    packet.flagId       = bonfireId;
    packet.flagValue    = true;

    Network::PeerManager::GetInstance().BroadcastPacket(&packet.header);
}

void SessionManager::OnFogGateEntered(uint32_t fogGateId) {
    LOG_INFO("Fog gate %u entered", fogGateId);

    Network::EventFlagPacket packet{};
    packet.header.magic = 0x44533243;
    packet.header.type  = Network::PacketType::FogGateTransition;
    packet.header.size  = sizeof(Network::EventFlagPacket);
    packet.flagId       = fogGateId;
    packet.flagValue    = true;

    Network::PeerManager::GetInstance().BroadcastPacket(&packet.header);
}

void SessionManager::PreventDisconnection() {
    m_preventDisconnect = true;
    LOG_INFO("Session disconnection prevention enabled");
}

void SessionManager::AllowDisconnection() {
    m_preventDisconnect = false;
    LOG_INFO("Session disconnection prevention disabled");
}

void SessionManager::TransitionToState(SessionState newState) {
    if (m_state == newState) return;
    LOG_INFO("Session state: %d -> %d", static_cast<int>(m_state), static_cast<int>(newState));
    m_state = newState;
}

void SessionManager::SynchronizePlayers() {
    // Driven by PlayerSync::Update now
}

bool SessionManager::GetLastPassword(std::string& outPassword) {
    return LoadLastPassword(outPassword);
}
