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

    LOG_INFO("Creating new session (Steam lobby)...");

    TransitionToState(SessionState::Connecting);

    m_isHost         = true;
    m_sessionPassword = password;

    // PeerManager: set up host flag + crypto key immediately so we can receive
    // incoming P2P connections while the lobby creation is in flight.
    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.CreateSession(password);
    m_localPlayerId = peerMgr.GetLocalPlayerId();

    // Initialize sync systems so we can read the character name
    Sync::PlayerSync::GetInstance().Initialize();
    Sync::ProgressSync::GetInstance().Initialize();

    // Add local player entry
    SessionPlayer localPlayer{};
    localPlayer.playerId  = m_localPlayerId;
    localPlayer.isAlive   = true;
    localPlayer.isReady   = true;
    localPlayer.soulLevel = 0;
    localPlayer.health = localPlayer.maxHealth = 0;
    localPlayer.x = localPlayer.y = localPlayer.z = 0.0f;

    std::string charName = Sync::PlayerSync::GetInstance().GetLocalCharacterName();
    localPlayer.playerName = charName.empty() ? "Host" : charName;

    {
        std::lock_guard<std::mutex> lock(m_playersMutex);
        m_players.push_back(localPlayer);
    }

    // Set up sign filtering
    Hooks::ProtobufHooks::ClearSessionSteamIds();
    std::string localSteam = Hooks::ProtobufHooks::GetLocalSteamId();
    if (!localSteam.empty())
        Hooks::ProtobufHooks::AddSessionSteamId(localSteam);

    Hooks::GameState::ClearBroadcastFlagCache();

    // Kick off async Steam lobby creation
    void* mm = SteamAPI::Matchmaking();
    if (!mm) {
        LOG_ERROR("SteamMatchmaking unavailable — cannot create lobby");
        TransitionToState(SessionState::Error);
        return false;
    }

    const auto& cfg = DS2Coop::SeamlessCoopMod::GetInstance().GetConfig();
    m_createLobbyCall = SteamMM::CreateLobby(mm, k_ELobbyTypePrivate,
                                              static_cast<int>(cfg.max_players));
    LOG_INFO("Steam CreateLobby dispatched (async)... password='%s'", password.c_str());
    return true;
}

bool SessionManager::JoinSession(const std::string& password) {
    if (!m_initialized) return false;
    if (m_state == SessionState::Connecting || m_state == SessionState::Connected ||
        m_state == SessionState::InGame) {
        LOG_WARNING("JoinSession called while already in state %d", (int)m_state);
        return false;
    }

    LOG_INFO("Searching for session with password '%s'...", password.c_str());

    TransitionToState(SessionState::Connecting);

    m_isHost          = false;
    m_sessionPassword = password;
    m_pendingPassword = password;

    // Initialize sync systems BEFORE joining P2P so the handshake has the real name
    Sync::PlayerSync::GetInstance().Initialize();
    Sync::ProgressSync::GetInstance().Initialize();

    auto& peerMgr = Network::PeerManager::GetInstance();
    m_localPlayerId = peerMgr.GetLocalPlayerId();

    // Add local player entry
    SessionPlayer localPlayer{};
    localPlayer.playerId  = m_localPlayerId;
    localPlayer.isAlive   = true;
    localPlayer.isReady   = true;
    localPlayer.soulLevel = 0;
    localPlayer.health = localPlayer.maxHealth = 0;
    localPlayer.x = localPlayer.y = localPlayer.z = 0.0f;

    std::string charName = Sync::PlayerSync::GetInstance().GetLocalCharacterName();
    localPlayer.playerName = charName.empty() ? "Player" : charName;

    {
        std::lock_guard<std::mutex> lock(m_playersMutex);
        m_players.push_back(localPlayer);
    }

    // Set up sign filtering
    Hooks::ProtobufHooks::ClearSessionSteamIds();
    std::string localSteam = Hooks::ProtobufHooks::GetLocalSteamId();
    if (!localSteam.empty())
        Hooks::ProtobufHooks::AddSessionSteamId(localSteam);

    Hooks::GameState::ClearBroadcastFlagCache();

    // Kick off async lobby list search filtered by our password
    void* mm = SteamAPI::Matchmaking();
    if (!mm) {
        LOG_ERROR("SteamMatchmaking unavailable — cannot search for lobby");
        TransitionToState(SessionState::Error);
        return false;
    }

    SteamMM::AddRequestLobbyListStringFilter(mm, "ds2coop_pw", password.c_str(),
                                             k_ELobbyComparisonEqual);
    SteamMM::AddRequestLobbyListResultCountFilter(mm, 1);
    m_requestLobbyCall = SteamMM::RequestLobbyList(mm);

    SaveLastPassword(password);

    LOG_INFO("Steam RequestLobbyList dispatched (async)...");
    return true;
}

void SessionManager::LeaveSession() {
    if (m_state == SessionState::Disconnected) return;

    LOG_INFO("Leaving session...");

    // Cancel pending async calls
    m_createLobbyCall  = 0;
    m_requestLobbyCall = 0;
    m_joinLobbyCall    = 0;

    // Leave Steam lobby
    if (m_currentLobbyId != 0) {
        void* mm = SteamAPI::Matchmaking();
        if (mm)
            SteamMM::LeaveLobby(mm, CSteamID(m_currentLobbyId));
        m_currentLobbyId = 0;
    }

    // Effacer la sauvegarde de dernière session à la déconnexion volontaire
    ClearLastPassword();

    Hooks::GameState::ClearBroadcastFlagCache();

    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.LeaveSession();

    {
        std::lock_guard<std::mutex> lock(m_playersMutex);
        m_players.clear();
    }

    TransitionToState(SessionState::Disconnected);

    LOG_INFO("Left session");
}

void SessionManager::Update(float deltaTime) {
    // Always pump Steam's callback queue
    SteamAPI::RunCallbacks();

    // Poll async Steam lobby operations (even in Connecting state)
    if (m_state != SessionState::Disconnected) {
        void* utils = SteamAPI::Utils();
        void* mm    = SteamAPI::Matchmaking();

        if (utils && mm) {
            bool failed = false;

            // ── Create lobby result (host) ─────────────────────────────────
            if (m_createLobbyCall != 0) {
                if (SteamUt::IsAPICallCompleted(utils, m_createLobbyCall, &failed)) {
                    LobbyCreated_t result{};
                    SteamUt::GetAPICallResult(utils, m_createLobbyCall,
                        &result, sizeof(result), LobbyCreated_t::k_iCallback, &failed);
                    m_createLobbyCall = 0;

                    if (!failed && result.m_eResult == 1 /* k_EResultOK */) {
                        m_currentLobbyId = result.m_ulSteamIDLobby;

                        // Tag lobby with password so joiners can find it
                        SteamMM::SetLobbyData(mm, CSteamID(m_currentLobbyId),
                                              "ds2coop_pw", m_sessionPassword.c_str());

                        Network::PeerManager::GetInstance().SetCurrentLobby(m_currentLobbyId);

                        TransitionToState(SessionState::Connected);
                        LOG_INFO("Steam lobby created: %llu (password='%s')",
                                 m_currentLobbyId, m_sessionPassword.c_str());
                        UI::Overlay::GetInstance().ShowNotification(
                            "Session creee ! Partagez le mot de passe.", 6.0f);
                    } else {
                        LOG_ERROR("Steam CreateLobby failed (eResult=%u, apiCallFailed=%d)",
                                  result.m_eResult, (int)failed);
                        TransitionToState(SessionState::Error);
                        UI::Overlay::GetInstance().ShowNotification(
                            "Echec creation du lobby Steam. Verifiez Steam.", 5.0f);
                    }
                }
            }

            // ── RequestLobbyList result (joiner) ──────────────────────────
            if (m_requestLobbyCall != 0) {
                if (SteamUt::IsAPICallCompleted(utils, m_requestLobbyCall, &failed)) {
                    LobbyMatchList_t result{};
                    SteamUt::GetAPICallResult(utils, m_requestLobbyCall,
                        &result, sizeof(result), LobbyMatchList_t::k_iCallback, &failed);
                    m_requestLobbyCall = 0;

                    if (!failed && result.m_nLobbiesMatching > 0) {
                        CSteamID lobbyId = SteamMM::GetLobbyByIndex(mm, 0);
                        m_joinLobbyCall  = SteamMM::JoinLobby(mm, lobbyId);
                        LOG_INFO("Found %u lobby/lobbies with matching password — joining...",
                                 result.m_nLobbiesMatching);
                    } else {
                        LOG_WARNING("No lobby found for password '%s' (count=%u, failed=%d)",
                                    m_sessionPassword.c_str(),
                                    result.m_nLobbiesMatching, (int)failed);
                        TransitionToState(SessionState::Error);
                        UI::Overlay::GetInstance().ShowNotification(
                            "Aucun lobby trouve — l'hote est-il en ligne ?", 5.0f);
                    }
                }
            }

            // ── JoinLobby result (joiner) ─────────────────────────────────
            if (m_joinLobbyCall != 0) {
                if (SteamUt::IsAPICallCompleted(utils, m_joinLobbyCall, &failed)) {
                    LobbyEnter_t result{};
                    SteamUt::GetAPICallResult(utils, m_joinLobbyCall,
                        &result, sizeof(result), LobbyEnter_t::k_iCallback, &failed);
                    m_joinLobbyCall = 0;

                    if (!failed && result.m_EChatRoomEnterResponse == 1 /* success */) {
                        m_currentLobbyId = result.m_ulSteamIDLobby;

                        CSteamID hostId = SteamMM::GetLobbyOwner(
                            mm, CSteamID(m_currentLobbyId));

                        auto& peerMgr = Network::PeerManager::GetInstance();
                        peerMgr.SetCurrentLobby(m_currentLobbyId);
                        peerMgr.JoinSession(hostId.ConvertToUint64(), m_sessionPassword);

                        TransitionToState(SessionState::Connected);
                        LOG_INFO("Joined lobby %llu, host SteamID=%llu",
                                 m_currentLobbyId, hostId.ConvertToUint64());
                        UI::Overlay::GetInstance().ShowNotification(
                            "Lobby rejoint ! En attente de l'hote...", 5.0f);
                    } else {
                        LOG_ERROR("JoinLobby failed (response=%u, failed=%d)",
                                  result.m_EChatRoomEnterResponse, (int)failed);
                        TransitionToState(SessionState::Error);
                        UI::Overlay::GetInstance().ShowNotification(
                            "Impossible de rejoindre le lobby Steam.", 5.0f);
                    }
                }
            }
        }
    }

    if (!IsActive()) return;

    // Update networking (receive packets, send heartbeats)
    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.Update();

    // Update player sync (reads game memory, broadcasts position/state)
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
