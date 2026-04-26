// Packet handler - processes incoming P2P packets
//
// Routes packets to the appropriate subsystem (session, sync, etc.)

#include "../../include/network.h"
#include "../../include/session.h"
#include "../../include/sync.h"
#include "../../include/utils.h"
#include "../../include/ui.h"
#include <unordered_map>

using namespace DS2Coop::Network;
using namespace DS2Coop::Utils;

PacketHandler& PacketHandler::GetInstance() {
    static PacketHandler instance;
    return instance;
}

// Externals from player_sync.cpp — flag our own ItemGive calls to avoid
// the ItemGiveHook re-broadcasting items we're granting to ourselves.
extern bool g_ourItemGiveCall;

void PacketHandler::HandlePacket(const PacketHeader* packet, const PeerInfo& sender) {
    if (!packet) return;

    switch (packet->type) {
        case PacketType::Handshake:
            if (packet->size >= sizeof(HandshakePacket))
                HandleHandshake(reinterpret_cast<const HandshakePacket*>(packet), sender);
            break;

        case PacketType::Heartbeat:
            break;

        case PacketType::Disconnect:
            LOG_INFO("Player %s disconnected", sender.playerName.c_str());
            {
                auto& sessionMgr = DS2Coop::Session::SessionManager::GetInstance();
                sessionMgr.RemovePlayer(sender.playerId);
                std::string msg = sender.playerName + " has left the session";
                DS2Coop::UI::Overlay::GetInstance().ShowCenteredNotification(msg, 4.0f, 1);
            }
            break;

        case PacketType::PlayerPosition:
            if (packet->size >= sizeof(PlayerPositionPacket))
                HandlePlayerPosition(reinterpret_cast<const PlayerPositionPacket*>(packet));
            break;

        case PacketType::PlayerState:
            if (packet->size >= sizeof(PlayerStatePacket))
                HandlePlayerState(reinterpret_cast<const PlayerStatePacket*>(packet));
            break;

        case PacketType::PlayerDeath:
            LOG_INFO("Remote player %s died", sender.playerName.c_str());
            {
                auto& sessionMgr = DS2Coop::Session::SessionManager::GetInstance();
                sessionMgr.NotifyPlayerDeath(sender.playerId);
                std::string msg = sender.playerName + " has died";
                DS2Coop::UI::Overlay::GetInstance().ShowCenteredNotification(msg, 4.0f, 2);
            }
            break;

        case PacketType::PlayerRespawn:
            LOG_INFO("Remote player %s respawned", sender.playerName.c_str());
            {
                auto& sessionMgr = DS2Coop::Session::SessionManager::GetInstance();
                sessionMgr.NotifyPlayerRespawn(sender.playerId);
                std::string msg = sender.playerName + " has respawned";
                DS2Coop::UI::Overlay::GetInstance().ShowCenteredNotification(msg, 3.0f, 0);
            }
            break;

        case PacketType::BossDefeated:
            if (packet->size >= sizeof(BossDefeatedPacket))
                HandleBossDefeated(reinterpret_cast<const BossDefeatedPacket*>(packet));
            break;

        case PacketType::EventFlag:
            if (packet->size >= sizeof(EventFlagPacket))
                HandleEventFlag(reinterpret_cast<const EventFlagPacket*>(packet));
            break;

        case PacketType::SoulsGranted:
            if (packet->size >= sizeof(SoulsGrantedPacket)) {
                const auto* sp = reinterpret_cast<const SoulsGrantedPacket*>(packet);
                LOG_INFO("[SOULS] Received %u souls from %s — applying",
                         sp->souls, sender.playerName.c_str());
                DS2Coop::Sync::ProgressSync::ApplySoulsGainToMemory(sp->souls);
            }
            break;

        case PacketType::ItemPickup:
            if (packet->size >= sizeof(ItemPickupPacket)) {
                const auto* ip = reinterpret_cast<const ItemPickupPacket*>(packet);
                LOG_INFO("[ITEM] Received item 0x%08X (cat=%d qty=%d) from %s — giving to local player",
                         ip->itemId, ip->category, ip->quantity, sender.playerName.c_str());
                // Mark as mod call so ItemGiveHook doesn't re-broadcast it
                g_ourItemGiveCall = true;
                DS2Coop::Sync::ProgressSync::ApplyItemPickupToInventory(
                    ip->category, ip->itemId, ip->durability,
                    ip->quantity, ip->upgrade, ip->infusion);
                g_ourItemGiveCall = false;
            }
            break;

        case PacketType::ZoneTransition:
            if (packet->size >= sizeof(ZoneTransitionPacket)) {
                const auto* zp = reinterpret_cast<const ZoneTransitionPacket*>(packet);
                // transitionType 1 = death respawn — don't sync, each player dies independently
                if (zp->transitionType != 1) {
                    const char* bonfireName =
                        DS2Coop::Sync::ProgressSync::GetBonfireName(zp->bonfireId);
                    LOG_INFO("[ZONE] %s warped to '%s' (bonfire %u) — executing local warp",
                             sender.playerName.c_str(), bonfireName, zp->bonfireId);
                    {
                        std::string msg = sender.playerName + " warped to " + bonfireName;
                        DS2Coop::UI::Overlay::GetInstance().ShowCenteredNotification(msg, 4.0f, 3);
                    }
                    DS2Coop::Sync::ProgressSync::ExecuteBonfireWarp(zp->bonfireId);
                } else {
                    LOG_DEBUG("[ZONE] %s respawned (death) — not syncing",
                              sender.playerName.c_str());
                }
            }
            break;

        case PacketType::BonfireRest:
            if (packet->size >= sizeof(EventFlagPacket)) {
                const auto* ep = reinterpret_cast<const EventFlagPacket*>(packet);
                LOG_INFO("[BONFIRE] Remote player lit bonfire %u — applying flag", ep->flagId);
                // Sync the event flag so the bonfire appears lit on next area load
                DS2Coop::Sync::ProgressSync::ApplyEventFlagToMemory(ep->flagId, true);
            } else {
                LOG_INFO("[BONFIRE] Remote player rested at bonfire");
            }
            break;

        case PacketType::FogGateTransition:
            LOG_INFO("[FOG] Remote player entered fog gate");
            break;

        default:
            LOG_DEBUG("Unknown packet type: %u from %s",
                      static_cast<uint8_t>(packet->type), sender.playerName.c_str());
            break;
    }
}

void PacketHandler::HandleHandshake(const HandshakePacket* packet, const PeerInfo& sender) {
    if (!packet) return;

    LOG_INFO("Handshake from %s (ID: %llu)", packet->playerName, packet->playerId);

    // Register this player in the session
    auto& sessionMgr = DS2Coop::Session::SessionManager::GetInstance();
    sessionMgr.AddPlayer(packet->playerId, packet->playerName);

    std::string msg = std::string(packet->playerName) + " has joined the session";
    DS2Coop::UI::Overlay::GetInstance().ShowCenteredNotification(msg, 5.0f, 1);
}

void PacketHandler::RemovePlayer(uint64_t playerId) {
    std::lock_guard<std::mutex> lock(m_seqMutex);
    m_lastPosSequence.erase(playerId);
}

void PacketHandler::HandlePlayerPosition(const PlayerPositionPacket* packet) {
    if (!packet) return;

    // Drop out-of-order packets (handles uint32 wrap-around)
    {
        std::lock_guard<std::mutex> lock(m_seqMutex);
        uint32_t& lastSeq = m_lastPosSequence[packet->playerId];
        int32_t seqDiff = static_cast<int32_t>(packet->header.sequence - lastSeq);
        if (seqDiff < 0 && seqDiff > -1000)
            return; // old packet, discard
        lastSeq = packet->header.sequence;
    }

    auto& sessionMgr = DS2Coop::Session::SessionManager::GetInstance();
    sessionMgr.UpdatePlayerPosition(packet->playerId, packet->x, packet->y, packet->z);

    auto& playerSync = DS2Coop::Sync::PlayerSync::GetInstance();
    playerSync.ApplyRemotePlayerPosition(packet->playerId,
                                         packet->x, packet->y, packet->z,
                                         packet->rotX, packet->rotY, packet->rotZ);
}

void PacketHandler::HandlePlayerState(const PlayerStatePacket* packet) {
    if (!packet) return;

    LOG_DEBUG("Player %llu state: HP %d/%d",
              packet->playerId, packet->health, packet->maxHealth);

    auto& sessionMgr = DS2Coop::Session::SessionManager::GetInstance();
    sessionMgr.UpdatePlayerHealth(packet->playerId, packet->health, packet->maxHealth);

    auto& playerSync = DS2Coop::Sync::PlayerSync::GetInstance();
    playerSync.ApplyRemotePlayerState(packet->playerId,
                                      packet->health, packet->maxHealth,
                                      packet->stamina, packet->maxStamina);
}

void PacketHandler::HandleBossDefeated(const BossDefeatedPacket* packet) {
    if (!packet) return;

    LOG_INFO("[BOSS] Remote peer killed boss (event flag %u) — applying to local memory",
             packet->bossId);

    // broadcastToNetwork=false: we received this from a peer, so we only write
    // the flag to our own game memory.  No re-broadcast to avoid round-trips.
    auto& progressSync = DS2Coop::Sync::ProgressSync::GetInstance();
    progressSync.SyncBossDefeat(packet->bossId, false);
}

void PacketHandler::HandleEventFlag(const EventFlagPacket* packet) {
    if (!packet) return;

    LOG_DEBUG("Event flag %u set to %d", packet->flagId, packet->flagValue);

    auto& progressSync = DS2Coop::Sync::ProgressSync::GetInstance();
    progressSync.SyncEventFlag(packet->flagId, packet->flagValue);
}
