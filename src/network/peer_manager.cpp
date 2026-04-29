// Peer-to-peer networking — Steamworks P2P transport
//
// Replaces the old UDP/ds3os layer.
// Transport: ISteamNetworking::SendP2PPacket / ReadP2PPacket
// Discovery:  Steam lobbies (managed by SessionManager)
//
// OUR MOD HOOKS (protobuf, memory patches, ItemGive, SetEventFlag) are
// completely independent of this transport layer — they operate on DS2's own
// networking code which runs in parallel.  This layer only carries our custom
// sync packets (position, HP, boss kills, etc.) between mod instances.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "../../include/network.h"
#include "../../include/session.h"
#include "../../include/sync.h"
#include "../../include/hooks.h"
#include "../../include/ui.h"
#include "../../include/utils.h"
#include "../../include/crypto.h"
#include "../../../third_party/steamworks/steam_minimal.h"

#include <chrono>
#include <algorithm>
#include <vector>

using namespace DS2Coop::Network;
using namespace DS2Coop::Utils;

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

constexpr uint32_t PACKET_MAGIC           = 0x44533243; // 'DS2C'
constexpr uint32_t PACKET_MAGIC_ENCRYPTED = 0x44533245; // 'DS2E'
constexpr int      P2P_CHANNEL            = 7;          // our channel (DS2 uses 0-2)
constexpr uint64_t HEARTBEAT_INTERVAL_MS  = 5000;
constexpr uint64_t TIMEOUT_DURATION_MS    = 90000;      // Steam relay can add latency

static uint64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static uint64_t NowSystemMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────

PeerManager& PeerManager::GetInstance() {
    static PeerManager instance;
    return instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// Initialize — just verify Steam is available
// ─────────────────────────────────────────────────────────────────────────────

bool PeerManager::Initialize() {
    if (m_initialized) return true;

    if (!SteamAPI::IsAvailable()) {
        LOG_ERROR("[P2P] Steam interfaces unavailable — is Steam running?");
        return false;
    }

    CSteamID localID = SteamAPI::GetLocalSteamID();
    if (!localID.IsValid()) {
        LOG_ERROR("[P2P] Could not retrieve local Steam ID");
        return false;
    }

    m_localPlayerId = localID.ConvertToUint64();
    m_initialized   = true;
    LOG_INFO("[P2P] Steamworks P2P ready  SteamID=%llu", m_localPlayerId);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Shutdown
// ─────────────────────────────────────────────────────────────────────────────

void PeerManager::Shutdown() {
    if (!m_initialized) return;
    LeaveSession();
    m_initialized = false;
    LOG_INFO("[P2P] Shut down");
}

// ─────────────────────────────────────────────────────────────────────────────
// CreateSession  (host side — called after lobby is ready)
// ─────────────────────────────────────────────────────────────────────────────

bool PeerManager::CreateSession(const std::string& password) {
    if (!m_initialized) return false;

    m_isHost          = true;
    m_connected       = true;
    m_sessionPassword = password;

    if (!DS2Coop::Crypto::DeriveKey(password))
        LOG_WARNING("[P2P] Key derivation failed — packets will be unencrypted");

    LOG_INFO("[P2P] Session created (host), waiting for peers...");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// JoinSession  (joiner side — called once lobby join succeeded)
// ─────────────────────────────────────────────────────────────────────────────

bool PeerManager::JoinSession(uint64_t hostSteamId, const std::string& password) {
    if (!m_initialized || hostSteamId == 0) return false;

    LOG_INFO("[P2P] Joining session, host SteamID=%llu", hostSteamId);

    m_sessionPassword = password;

    if (!DS2Coop::Crypto::DeriveKey(password))
        LOG_WARNING("[P2P] Key derivation failed — packets will be unencrypted");

    // Build handshake packet
    HandshakePacket hs{};
    hs.header.magic     = PACKET_MAGIC;
    hs.header.type      = PacketType::Handshake;
    hs.header.size      = sizeof(HandshakePacket);
    hs.header.sequence  = 0;
    hs.header.timestamp = NowMs();
    hs.version          = 1;
    hs.playerId         = m_localPlayerId;
    std::string charName = DS2Coop::Sync::PlayerSync::GetInstance().GetLocalCharacterName();
    strncpy_s(hs.playerName, charName.empty() ? "Player" : charName.c_str(), sizeof(hs.playerName));
    strncpy_s(hs.password,   password.c_str(), sizeof(hs.password));

    // Register the host as a pending peer
    {
        std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
        PeerInfo host{};
        host.playerId      = hostSteamId;
        host.steamId       = hostSteamId;
        host.playerName    = "Host";
        host.lastHeartbeat = NowMs();
        host.connected     = true;
        m_peers.push_back(host);
    }

    // Send handshake via Steam P2P
    void* net = SteamAPI::Networking();
    if (!net) { LOG_ERROR("[P2P] Steam networking unavailable"); return false; }

    CSteamID hostID(hostSteamId);
    bool sent = SteamNet::SendP2PPacket(net, hostID,
                                        &hs, sizeof(hs),
                                        k_EP2PSendReliable, P2P_CHANNEL);
    if (!sent) {
        LOG_ERROR("[P2P] Failed to send handshake to host %llu", hostSteamId);
        return false;
    }

    m_isHost                  = false;
    m_connected               = true;
    m_handshakeConfirmed      = false;
    m_connectingTimestampMs   = NowMs();

    LOG_INFO("[P2P] Handshake sent to host %llu", hostSteamId);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// LeaveSession
// ─────────────────────────────────────────────────────────────────────────────

void PeerManager::LeaveSession() {
    if (!m_connected) return;

    // Send disconnect to all peers
    PacketHeader bye{};
    bye.magic     = PACKET_MAGIC;
    bye.type      = PacketType::Disconnect;
    bye.size      = sizeof(PacketHeader);
    bye.timestamp = NowMs();
    BroadcastPacket(&bye);

    // Close Steam P2P sessions
    void* net = SteamAPI::Networking();
    if (net) {
        std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
        for (const auto& p : m_peers)
            SteamNet::CloseP2PSessionWithUser(net, CSteamID(p.steamId));
    }

    {
        std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
        m_peers.clear();
    }
    m_connected       = false;
    m_isHost          = false;
    m_currentLobbyId  = 0;
    m_sessionPassword.clear();
    DS2Coop::Crypto::ClearKey();
    LOG_INFO("[P2P] Session left");
}

// ─────────────────────────────────────────────────────────────────────────────
// Update  (called at ~20 Hz from the session update thread)
// ─────────────────────────────────────────────────────────────────────────────

void PeerManager::Update() {
    if (!m_initialized || !m_connected) return;

    // Accept incoming P2P from all lobby members (host)
    if (m_isHost && m_currentLobbyId != 0)
        AcceptLobbyMembers();

    // Process incoming Steam P2P packets
    HandleIncomingPackets();

    {
        std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
        SendHeartbeats();
        CheckTimeouts();
    }

    // Client handshake timeout (15 s)
    if (!m_isHost && !m_handshakeConfirmed && m_connectingTimestampMs > 0) {
        if (NowMs() - m_connectingTimestampMs > 15000) {
            LOG_WARNING("[P2P] Handshake timeout — host did not respond");
            DS2Coop::UI::Overlay::GetInstance().ShowNotification(
                "Connexion echouee — l'hote n'a pas repondu.", 6.0f);
            LeaveSession();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// AcceptLobbyMembers  (host: proactively accept P2P from everyone in the lobby)
// ─────────────────────────────────────────────────────────────────────────────

void PeerManager::AcceptLobbyMembers() {
    void* mm  = SteamAPI::Matchmaking();
    void* net = SteamAPI::Networking();
    if (!mm || !net) return;

    CSteamID lobbyId(m_currentLobbyId);
    CSteamID localId(m_localPlayerId);
    int count = SteamMM::GetNumLobbyMembers(mm, lobbyId);
    for (int i = 0; i < count; i++) {
        CSteamID memberId = SteamMM::GetLobbyMemberByIndex(mm, lobbyId, i);
        if (memberId.IsValid() && memberId != localId)
            SteamNet::AcceptP2PSessionWithUser(net, memberId);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Encryption helpers (unchanged from UDP version)
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<uint8_t> BuildEncryptedBuffer(const PacketHeader* packet) {
    const size_t hdrSize     = sizeof(PacketHeader);
    const size_t totalSize   = packet->size;
    const size_t payloadSize = (totalSize > hdrSize) ? (totalSize - hdrSize) : 0;

    std::vector<uint8_t> buf(hdrSize + DS2Coop::Crypto::GCM_TAG_BYTES + payloadSize);
    memcpy(buf.data(), packet, hdrSize);
    if (payloadSize > 0)
        memcpy(buf.data() + hdrSize + DS2Coop::Crypto::GCM_TAG_BYTES,
               reinterpret_cast<const uint8_t*>(packet) + hdrSize, payloadSize);

    reinterpret_cast<PacketHeader*>(buf.data())->magic = PACKET_MAGIC_ENCRYPTED;
    uint8_t  tag[DS2Coop::Crypto::GCM_TAG_BYTES] = {};
    uint8_t* payload = buf.data() + hdrSize + DS2Coop::Crypto::GCM_TAG_BYTES;

    if (payloadSize > 0)
        DS2Coop::Crypto::Encrypt(buf.data(), hdrSize, payload, payloadSize, packet->sequence, tag);

    memcpy(buf.data() + hdrSize, tag, DS2Coop::Crypto::GCM_TAG_BYTES);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
// SendPacket / BroadcastPacket
// ─────────────────────────────────────────────────────────────────────────────

bool PeerManager::SendPacket(const PacketHeader* packet, uint64_t targetPlayerId) {
    if (!m_initialized || !m_connected || !packet) return false;

    void* net = SteamAPI::Networking();
    if (!net) return false;

    std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
    std::vector<uint8_t> encBuf;
    if (DS2Coop::Crypto::IsKeyReady()) encBuf = BuildEncryptedBuffer(packet);

    for (const auto& peer : m_peers) {
        if (!peer.connected) continue;
        if (targetPlayerId != 0 && peer.playerId != targetPlayerId) continue;

        const void* data   = encBuf.empty() ? (const void*)packet : (const void*)encBuf.data();
        uint32_t    dataLen = encBuf.empty() ? packet->size : (uint32_t)encBuf.size();

        SteamNet::SendP2PPacket(net, CSteamID(peer.steamId), data, dataLen,
                                k_EP2PSendReliable, P2P_CHANNEL);
        if (targetPlayerId != 0) return true;
    }
    return true;
}

void PeerManager::BroadcastPacket(const PacketHeader* packet) {
    if (!m_initialized || !m_connected || !packet) return;

    void* net = SteamAPI::Networking();
    if (!net) return;

    std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
    std::vector<uint8_t> encBuf;
    if (DS2Coop::Crypto::IsKeyReady()) encBuf = BuildEncryptedBuffer(packet);

    for (const auto& peer : m_peers) {
        if (!peer.connected) continue;
        const void* data    = encBuf.empty() ? (const void*)packet : (const void*)encBuf.data();
        uint32_t    dataLen = encBuf.empty() ? packet->size : (uint32_t)encBuf.size();
        SteamNet::SendP2PPacket(net, CSteamID(peer.steamId), data, dataLen,
                                k_EP2PSendReliable, P2P_CHANNEL);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// HandleIncomingPackets
// ─────────────────────────────────────────────────────────────────────────────

void PeerManager::HandleIncomingPackets() {
    void* net = SteamAPI::Networking();
    if (!net) return;

    struct ReceivedPacket { char data[8192]; uint32_t size; uint64_t senderSteamId; };
    std::vector<ReceivedPacket> packets;

    // Drain all pending P2P packets
    uint32_t msgSize = 0;
    while (SteamNet::IsP2PPacketAvailable(net, &msgSize, P2P_CHANNEL) && msgSize > 0) {
        if (msgSize > 8192) { // discard oversized
            ReceivedPacket tmp{}; CSteamID sender;
            SteamNet::ReadP2PPacket(net, tmp.data, 8192, &msgSize, &sender, P2P_CHANNEL);
            continue;
        }
        ReceivedPacket pkt{};
        CSteamID sender;
        if (!SteamNet::ReadP2PPacket(net, pkt.data, sizeof(pkt.data), &pkt.size, &sender, P2P_CHANNEL))
            break;
        pkt.senderSteamId = sender.ConvertToUint64();

        if (pkt.size < sizeof(PacketHeader)) continue;
        PacketHeader* hdr = reinterpret_cast<PacketHeader*>(pkt.data);

        if (hdr->magic == PACKET_MAGIC_ENCRYPTED) {
            const size_t hdrSize  = sizeof(PacketHeader);
            const size_t minSize  = hdrSize + DS2Coop::Crypto::GCM_TAG_BYTES;
            if (pkt.size < minSize) continue;

            uint8_t* raw       = reinterpret_cast<uint8_t*>(pkt.data);
            const uint8_t* tag = raw + hdrSize;
            uint8_t* payload   = raw + hdrSize + DS2Coop::Crypto::GCM_TAG_BYTES;
            size_t payloadSize = pkt.size - minSize;

            if (!DS2Coop::Crypto::Decrypt(raw, hdrSize, payload, payloadSize, hdr->sequence, tag)) {
                LOG_WARNING("[P2P] Dropped packet with invalid GCM tag from %llu", pkt.senderSteamId);
                continue;
            }
            hdr->magic = PACKET_MAGIC;
            memmove(raw + hdrSize, payload, payloadSize);
            pkt.size = (uint32_t)(hdrSize + payloadSize);
            packets.push_back(pkt);
        } else if (hdr->magic == PACKET_MAGIC) {
            packets.push_back(pkt);
        }
    }

    // Process packets
    for (auto& pkt : packets) {
        PacketHeader* header = reinterpret_cast<PacketHeader*>(pkt.data);

        if (header->type == PacketType::Handshake && pkt.size >= sizeof(HandshakePacket)) {
            HandleHandshakePacket(reinterpret_cast<HandshakePacket*>(pkt.data), pkt.senderSteamId);
            continue;
        }

        if (header->type == PacketType::Disconnect && !m_isHost) {
            LOG_WARNING("[P2P] Disconnect from host — wrong password?");
            DS2Coop::UI::Overlay::GetInstance().ShowNotification(
                "Rejete par l'hote. Mauvais mot de passe ?", 5.0f);
            LeaveSession();
            return;
        }

        // Update peer heartbeat + ping
        PeerInfo senderInfo{};
        {
            std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
            for (auto& peer : m_peers) {
                if (peer.steamId == pkt.senderSteamId) {
                    peer.lastHeartbeat = NowMs();
                    if (header->type == PacketType::Heartbeat && header->timestamp > 0) {
                        uint64_t now  = NowSystemMs();
                        uint64_t diff = (now >= header->timestamp) ? (now - header->timestamp) : 0;
                        if (diff < 5000) {
                            peer.ping_ms = static_cast<uint32_t>(diff);
                            DS2Coop::Session::SessionManager::GetInstance()
                                .UpdatePlayerPing(peer.playerId, peer.ping_ms);
                        }
                    }
                    senderInfo = peer;
                    break;
                }
            }
        }
        senderInfo.steamId       = pkt.senderSteamId;
        senderInfo.playerId      = pkt.senderSteamId;
        senderInfo.lastHeartbeat = NowMs();
        senderInfo.connected     = true;

        PacketHandler::GetInstance().HandlePacket(header, senderInfo);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// HandleHandshakePacket
// ─────────────────────────────────────────────────────────────────────────────

void PeerManager::HandleHandshakePacket(const HandshakePacket* hs, uint64_t senderSteamId) {
    LOG_INFO("[P2P] Handshake from %s (SteamID=%llu)", hs->playerName, senderSteamId);

    if (hs->version != 1) {
        LOG_ERROR("[P2P] Version mismatch: expected 1, got %u", hs->version);
        return;
    }

    if (m_isHost) {
        // Validate password
        if (m_sessionPassword != std::string(hs->password)) {
            LOG_WARNING("[P2P] Peer rejected: wrong password from %llu", senderSteamId);
            PacketHeader reject{};
            reject.magic = PACKET_MAGIC; reject.type = PacketType::Disconnect;
            reject.size  = sizeof(PacketHeader); reject.timestamp = NowMs();
            void* net = SteamAPI::Networking();
            if (net) SteamNet::SendP2PPacket(net, CSteamID(senderSteamId),
                                              &reject, sizeof(reject),
                                              k_EP2PSendReliable, P2P_CHANNEL);
            return;
        }

        // Accept peer
        bool newPeer = false;
        {
            std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
            bool known = false;
            for (auto& p : m_peers) {
                if (p.steamId == senderSteamId) { p.lastHeartbeat = NowMs(); known = true; break; }
            }
            if (!known) {
                PeerInfo pi{};
                pi.playerId      = senderSteamId;
                pi.steamId       = senderSteamId;
                pi.playerName    = hs->playerName;
                pi.lastHeartbeat = NowMs();
                pi.connected     = true;
                m_peers.push_back(pi);
                newPeer = true;
            }
        }

        if (newPeer) {
            LOG_INFO("[P2P] Peer accepted: %s (%llu)", hs->playerName, senderSteamId);
            DS2Coop::Hooks::ProtobufHooks::SetSeamlessActive(true);
            DS2Coop::Sync::ProgressSync::GetInstance().DumpStateToNewPeer(senderSteamId);
        }

        // Send response
        HandshakePacket resp{};
        resp.header.magic = PACKET_MAGIC; resp.header.type = PacketType::Handshake;
        resp.header.size  = sizeof(HandshakePacket); resp.header.sequence = 1;
        resp.header.timestamp = NowMs(); resp.version = 1;
        resp.playerId = m_localPlayerId;
        std::string hostName = DS2Coop::Sync::PlayerSync::GetInstance().GetLocalCharacterName();
        strncpy_s(resp.playerName, hostName.empty() ? "Host" : hostName.c_str(), sizeof(resp.playerName));
        resp.password[0] = '\0';

        void* net = SteamAPI::Networking();
        if (net) SteamNet::SendP2PPacket(net, CSteamID(senderSteamId),
                                          &resp, sizeof(resp),
                                          k_EP2PSendReliable, P2P_CHANNEL);

    } else {
        // Joiner: this is the host's response
        LOG_INFO("[P2P] Host responded: %s (%llu)", hs->playerName, senderSteamId);
        {
            std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
            for (auto& p : m_peers) {
                if (p.steamId == senderSteamId) {
                    p.playerName = hs->playerName; p.lastHeartbeat = NowMs(); break;
                }
            }
        }
        m_handshakeConfirmed = true;
        DS2Coop::Hooks::ProtobufHooks::SetSeamlessActive(true);
        LOG_INFO("[P2P] Handshake confirmed — seamless mode ON");
        DS2Coop::UI::Overlay::GetInstance().ShowNotification(
            "Connecte ! Co-op seamless active.", 5.0f);
    }

    // Forward to packet handler for SessionManager
    PeerInfo senderInfo{};
    senderInfo.playerId      = senderSteamId;
    senderInfo.steamId       = senderSteamId;
    senderInfo.playerName    = hs->playerName;
    senderInfo.lastHeartbeat = NowMs();
    senderInfo.connected     = true;
    PacketHandler::GetInstance().HandlePacket(&hs->header, senderInfo);
}

// ─────────────────────────────────────────────────────────────────────────────
// SendHeartbeats / CheckTimeouts
// ─────────────────────────────────────────────────────────────────────────────

void PeerManager::SendHeartbeats() {
    uint64_t now = NowMs();
    if (now - m_lastHeartbeatMs < HEARTBEAT_INTERVAL_MS) return;

    PacketHeader hb{};
    hb.magic = PACKET_MAGIC; hb.type = PacketType::Heartbeat;
    hb.size  = sizeof(PacketHeader); hb.sequence = 0;
    hb.timestamp = NowSystemMs();
    BroadcastPacket(&hb);
    m_lastHeartbeatMs = now;
}

void PeerManager::CheckTimeouts() {
    if (DS2Coop::Hooks::ProtobufHooks::IsSeamlessActive()) return;

    uint64_t now = NowMs();
    auto it = m_peers.begin();
    while (it != m_peers.end()) {
        if (it->connected && (now - it->lastHeartbeat > TIMEOUT_DURATION_MS)) {
            LOG_WARNING("[P2P] Peer %s (%llu) timed out", it->playerName.c_str(), it->steamId);
            uint64_t id = it->playerId;
            it = m_peers.erase(it);
            DS2Coop::Session::SessionManager::GetInstance().RemovePlayer(id);
        } else {
            ++it;
        }
    }
}
