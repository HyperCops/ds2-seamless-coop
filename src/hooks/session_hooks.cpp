// Protobuf Interception Hooks - THE CORE OF SEAMLESS CO-OP
//
// This file implements the ds3os approach: instead of finding individual game
// functions (CreateSession, DestroySession, etc.), we hook the protobuf
// serialization layer that ALL network messages pass through.
//
// When the game tries to send a disconnect/leave message after a boss kill
// or player death, we intercept it here and silently drop it. The game
// thinks the message was sent, but the session stays alive.
//
// Sources:
//   - ds3os/Source/Injector/Hooks/DarkSouls2/DS2_LogProtobufsHook.cpp
//   - AOB patterns verified in DS2 SotFS by TLeonardUK

#include "../../include/hooks.h"
#include "../../include/addresses.h"
#include "../../include/session.h"
#include "../../include/network.h"
#include "../../include/utils.h"
#include "../../include/pattern_scanner.h"
#include "../../include/address_resolver.h"
#include "MinHook.h"
#include <typeinfo>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

using namespace DS2Coop::Hooks;
using namespace DS2Coop::Utils;
using namespace DS2Coop::Addresses;

// ============================================================================
// State
// ============================================================================
static std::atomic<bool> g_seamlessActive{false};
static std::atomic<uint32_t> g_blockedCount{0};
static std::atomic<uint32_t> g_totalCount{0};

// Steam ID whitelist — only signs from these players are shown
static std::vector<std::string> g_sessionSteamIds;
static std::mutex g_steamIdMutex;
static std::string g_localSteamId;

// ============================================================================
// Get local Steam ID from steam_api64.dll
// ============================================================================
typedef uint64_t(*SteamID_t)();

static uint64_t SafeCallGetSteamID(void* userIface) {
    typedef uint64_t(__fastcall* Fn)(void*);
    void** vt = *reinterpret_cast<void***>(userIface);
    auto fn = reinterpret_cast<Fn>(vt[2]);
    __try { return fn(userIface); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static std::string GetLocalSteamIdInternal() {
    if (!g_localSteamId.empty()) return g_localSteamId;

    HMODULE hSteam = GetModuleHandleW(L"steam_api64.dll");
    if (!hSteam) return "";

    typedef void*(*GetSteamUser_t)();
    auto SteamUser = reinterpret_cast<GetSteamUser_t>(GetProcAddress(hSteam, "SteamAPI_SteamUser_v023"));
    if (!SteamUser) SteamUser = reinterpret_cast<GetSteamUser_t>(GetProcAddress(hSteam, "SteamAPI_SteamUser_v021"));
    if (!SteamUser) return "";

    void* user = SteamUser();
    if (!user) return "";

    uint64_t steamId = SafeCallGetSteamID(user);
    if (steamId == 0) return "";

    g_localSteamId = std::to_string(steamId);
    LOG_INFO("[STEAM] Local Steam ID: %s", g_localSteamId.c_str());
    return g_localSteamId;
}

// ============================================================================
// Check if raw protobuf bytes contain a steam_id from our session
// Field 5 (player_steam_id) has wire key 0x2A (field 5, type 2 = length-delimited)
// ============================================================================
static bool ContainsSessionSteamId(const uint8_t* data, int size) {
    std::lock_guard<std::mutex> lock(g_steamIdMutex);
    if (g_sessionSteamIds.empty()) return true; // no filter active

    // Scan for wire key 0x2A followed by varint length then steam ID string
    for (int i = 0; i < size - 20; i++) {
        if (data[i] == 0x2A) {
            // Read varint length
            int len = 0;
            int j = i + 1;
            if (j < size) {
                len = data[j] & 0x7F;
                if (data[j] & 0x80 && j + 1 < size) {
                    len |= (data[j + 1] & 0x7F) << 7;
                    j++;
                }
                j++;
            }
            // Steam IDs are 17 digits starting with "7656119"
            if (len >= 17 && len <= 20 && j + len <= size) {
                std::string candidate(reinterpret_cast<const char*>(data + j), len);
                if (candidate.substr(0, 7) == "7656119") {
                    // Found a Steam ID — check if it's in our session
                    for (const auto& sid : g_sessionSteamIds) {
                        if (sid == candidate) return true;
                    }
                    // Steam ID found but not in session — block this sign
                    return false;
                }
            }
        }
    }

    return true; // no steam ID found in message — allow through
}

// Original function pointers
static ProtobufHooks::SerializeFunc g_originalSerialize = nullptr;
static ProtobufHooks::ParseFunc g_originalParse = nullptr;

// ============================================================================
// RTTI Helper - extract class name from MSVC vtable
// This is how ds3os identifies which protobuf message is being serialized.
//
// MSVC x64 RTTI layout:
//   object -> vtable pointer (first 8 bytes)
//   vtable[-1] -> pointer to RTTICompleteObjectLocator
//   COL -> pTypeDescriptor (offset within module)
//   TypeDescriptor -> decorated name string
//
// The ds3os project uses this same technique (GetRttiNameFromObject).
// ============================================================================

// MSVC RTTI structures (simplified)
struct _RTTITypeDescriptor {
    void* pVFTable;
    void* spare;
    char name[1]; // Decorated name, e.g. ".?AVRequestNotifyDisconnectSession@@"
};

struct _RTTICompleteObjectLocator {
    uint32_t signature;       // 1 for x64
    uint32_t offset;
    uint32_t cdOffset;
    int32_t  pTypeDescriptor; // RVA to TypeDescriptor (relative to module base in x64)
    int32_t  pClassHierarchy; // RVA to ClassHierarchyDescriptor
    int32_t  pSelf;           // RVA to this COL (for module base calculation)
};

static const char* GetRttiClassName(void* obj) {
    if (!obj) return "unknown";

    __try {
        // Read vtable pointer
        uintptr_t* vtable = *reinterpret_cast<uintptr_t**>(obj);
        if (!vtable) return "unknown";

        // vtable[-1] points to RTTICompleteObjectLocator
        auto* col = reinterpret_cast<_RTTICompleteObjectLocator*>(vtable[-1]);
        if (!col) return "unknown";

        // For x64 (signature == 1), TypeDescriptor is an RVA from module base.
        // Calculate module base from the COL's pSelf field.
        if (col->signature == 1) {
            uintptr_t colAddr = reinterpret_cast<uintptr_t>(col);
            uintptr_t moduleBase = colAddr - static_cast<uint32_t>(col->pSelf);
            auto* typeDesc = reinterpret_cast<_RTTITypeDescriptor*>(
                moduleBase + static_cast<uint32_t>(col->pTypeDescriptor));
            return typeDesc->name; // ".?AVClassName@@"
        }

        // x86 fallback (signature == 0) - direct pointer
        auto* typeDesc = reinterpret_cast<_RTTITypeDescriptor*>(
            static_cast<uintptr_t>(col->pTypeDescriptor));
        return typeDesc->name;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return "unknown";
    }
}

// ============================================================================
// Check if a message class name corresponds to a disconnect/leave message
// ============================================================================
// Messages to block when SENDING (outgoing — serialize hook)
// Don't block ANY outgoing messages — let them serialize and send normally.
// Blocking outgoing messages corrupts the server's session state.
// Outgoing disconnect blocking is OFF. See crash_history.md for why.
// Instead, we capture the return address when LeaveGuestPlayer is serialized
// to identify the caller function for future NOP patching.
static bool IsOutgoingDisconnect(const char* className) {
    (void)className;
    return false;
}

// Messages to block when RECEIVING (incoming — parse hook)
//
// CRITICAL: These messages are blocked BEFORE calling the original parser
// so the game code never sees them. Returning true (success) from ParseHook
// without parsing is safe — the caller checks the return value and skips
// processing if we've already handled it by returning true pre-emptively.
// Actually we return true to prevent errors but the object stays default-constructed;
// the caller's message dispatch finds nothing to act on.
//
// Boss kill sequence:
//   1. Boss HP → 0
//   2. DS2 local code tries to call FUN_140191bb0 → NOPed by PatchPhantomDismissalLoops
//   3. Server still detects boss kill → pushes NotifyLeaveGuestPlayer
//   4. WITHOUT block: game dismisses phantom on step 3
//   5. WITH block: step 3 message is dropped → phantom stays
//
// Phantom natural death (player HP → 0):
//   This uses NotifyPlayerDeath / NotifyKillPlayer, NOT NotifyLeaveGuestPlayer.
//   So blocking NotifyLeaveGuestPlayer does NOT break phantom death/respawn.
static bool IsIncomingDisconnect(const char* className) {
    if (!className) return false;

    // Server-initiated forced disconnect
    if (strstr(className, "DisconnectSession")) return true;

    // Server kicking us (anti-cheat, admin ban, etc.)
    if (strstr(className, "BanishPlayer")) return true;

    // Boss kill phantom dismissal — server pushes this when boss dies
    // Safe to block: phantom death uses NotifyPlayerDeath, not this message
    if (strstr(className, "NotifyLeaveGuestPlayer")) return true;
    if (strstr(className, "PushLeaveGuestPlayer")) return true;
    if (strstr(className, "LeaveGuestPlayer")) return true;

    // Session leave pushed from server (warp/bonfire disconnects)
    if (strstr(className, "PushLeaveSession")) return true;
    if (strstr(className, "NotifyLeaveSession")) return true;

    // When a phantom uses the Black Separation Crystal, the server sends
    // PushRequestRemoveSign to the host. The host's game processes this as
    // "phantom is gone" and tries to tear down the active session — crash.
    if (strstr(className, "RemoveSign")) return true;
    if (strstr(className, "RejectSign")) return true;

    // Block all invasion-related messages while seamless co-op is active.
    if (strstr(className, "BreakIn")) return true;
    if (strstr(className, "PushBreakIn")) return true;

    return false;
}

// ============================================================================
// Check if a message is a death notification (we log but don't block)
// ============================================================================
static bool IsDeathMessage(const char* className) {
    if (!className) return false;
    if (strstr(className, "NotifyDeath")) return true;
    if (strstr(className, "NotifyKillPlayer")) return true;
    return false;
}

// Forward declarations for helpers defined below ParseHook
static void OnPhantomJoined();
static void OnPhantomLeft();

// ============================================================================
// HOOKED: SerializeWithCachedSizesToArray
//
// Every outgoing protobuf message passes through this function.
// We inspect the message type via RTTI and block disconnect messages.
// ============================================================================
static uint8_t* __fastcall SerializeHook(void* thisPtr, uint8_t* target) {
    g_totalCount++;

    const char* className = GetRttiClassName(thisPtr);

    // If seamless mode is active, block disconnect messages
    if (g_seamlessActive.load()) {
        if (IsOutgoingDisconnect(className)) {
            g_blockedCount++;
            LOG_INFO("[SEAMLESS] BLOCKED outgoing: %s (total: %u)", className, g_blockedCount.load());

            // Let the serialize run so the game's internal state stays consistent,
            // but return the original target pointer so zero bytes are added to
            // the output buffer. The network layer sees an empty message and skips it.
            // Previous approach of memset-zeroing corrupted the protobuf stream.
            g_originalSerialize(thisPtr, target);
            return target;
        }

        if (IsDeathMessage(className)) {
            LOG_INFO("[SEAMLESS] Death notification sent (allowed): %s", className);
            // We allow death messages through - the session should survive deaths
            // because we block the disconnect that would normally follow.
        }
    }

    // Log session, item, and interaction messages for debugging
    if (strstr(className, "Session") || strstr(className, "Sign") ||
        strstr(className, "Guest") || strstr(className, "BreakIn") ||
        strstr(className, "Leave") || strstr(className, "Return") ||
        strstr(className, "Banish") || strstr(className, "Remove") ||
        strstr(className, "Phantom") || strstr(className, "Summon") ||
        strstr(className, "Item") || strstr(className, "Buy") ||
        strstr(className, "Chest") || strstr(className, "Treasure") ||
        strstr(className, "Bonfire") || strstr(className, "Visit") ||
        strstr(className, "Boss") || strstr(className, "FogWall")) {
        LOG_INFO("[PROTOBUF >>] %s (seamless=%s)", className,
                 g_seamlessActive.load() ? "ON" : "OFF");
    }

    // Capture call stack when the game tries to send LeaveGuestPlayer or LeaveSession
    // This tells us which function initiates phantom removal after boss kills.
    if (strstr(className, "LeaveGuestPlayer") || strstr(className, "LeaveSession")) {
        // Walk the return addresses on the stack to find the caller chain
        void* callers[16] = {};
        USHORT frames = CaptureStackBackTrace(0, 16, callers, nullptr);
        uintptr_t exeBase = (uintptr_t)GetModuleHandle(nullptr);
        LOG_INFO("[CALLTRACE] %s initiated from (exe base: 0x%llX):", className, exeBase);
        for (int i = 0; i < frames && i < 16; i++) {
            uintptr_t addr = (uintptr_t)callers[i];
            LOG_INFO("[CALLTRACE]   [%d] 0x%llX (exe+0x%llX)", i, addr, addr - exeBase);
        }
    }

    // Call original for all non-blocked messages
    return g_originalSerialize(thisPtr, target);
}

// Plain C helper — no C++ objects, safe for SEH.
// Reads phantom name from NetSessionManager into buf (UTF-8, null-terminated).
static void TryReadPhantomName(char* buf, int bufLen) {
    uintptr_t nsm = DS2Coop::AddressResolver::GetInstance().GetNetSessionManager();
    if (!nsm) return;
    __try {
        uintptr_t pp = 0;
        if (!DS2Coop::Utils::Memory::Read<uintptr_t>(nsm + 0x20, &pp) || !pp) return;
        wchar_t wname[24] = {};
        for (int i = 0; i < 23; i++) {
            wchar_t ch = 0;
            if (!DS2Coop::Utils::Memory::Read<wchar_t>(pp + 0x234 + i*2, &ch) || ch == 0) break;
            if (ch < 0x20 || ch > 0x9FFF) return;
            wname[i] = ch;
        }
        if (wname[0])
            WideCharToMultiByte(CP_UTF8, 0, wname, -1, buf, bufLen-1, nullptr, nullptr);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static std::atomic<int> g_phantomCounter{0};

static void OnPhantomJoined() {
    // Don't read name from NSM+0x20+0x234 — that's always the LOCAL player's name.
    // Use a numbered placeholder. P2P handshake will update the name if it connects.
    int num = ++g_phantomCounter;
    char nameBuf[32];
    snprintf(nameBuf, sizeof(nameBuf), "Phantom %d", num);

    // Use a deterministic-ish ID that won't collide with the local player ID
    uint64_t phantomId = static_cast<uint64_t>(GetTickCount64()) ^ (0xDE1F0000ULL + num);
    LOG_INFO("[SEAMLESS] Phantom entered world: %s (id=%llu)", nameBuf, phantomId);
    DS2Coop::Session::SessionManager::GetInstance().AddPlayer(phantomId, nameBuf);
}

static void OnPhantomLeft() {
    LOG_INFO("[SEAMLESS] Phantom left world — removing from session");
    auto& sm = DS2Coop::Session::SessionManager::GetInstance();
    auto players = sm.GetPlayers();
    auto* local = sm.GetLocalPlayer();
    uint64_t localId = local ? local->playerId : 0;
    for (const auto& p : players) {
        if (p.playerId != localId) {
            sm.RemovePlayer(p.playerId);
            break;
        }
    }
}

// ============================================================================
// HOOKED: ParseFromArray
//
// Every incoming protobuf message passes through this function.
//
// CRITICAL ORDER: we MUST check for disconnect messages BEFORE calling the
// original parser. If the original parser runs, the game's C++ code processes
// the message immediately (setting state flags, triggering callbacks). By the
// time we return false, the disconnect already happened. Blocking pre-parse
// means the game's message handler never sees the data.
//
// Returning 'true' (without parsing) pretends the message parsed to an empty
// object. The caller's dispatch switch finds no opcode/data to act on.
// ============================================================================
static bool __fastcall ParseHook(void* thisPtr, void* data, int size) {
    // GetRttiClassName reads the vtable pointer set at object construction —
    // this works even before ParseFromArray populates the fields.
    const char* className = GetRttiClassName(thisPtr);

    // ── BLOCK DISCONNECT MESSAGES BEFORE GAME PROCESSES THEM ─────────────────
    if (g_seamlessActive.load() && IsIncomingDisconnect(className)) {
        LOG_INFO("[SEAMLESS] BLOCKED incoming (pre-parse): %s (size=%d)", className, size);
        g_blockedCount++;
        // Return true so caller doesn't treat this as a parse error.
        // The object stays default-constructed — no action triggered.
        return true;
    }

    // Log interesting incoming messages
    if (strstr(className, "Session") || strstr(className, "Guest") ||
        strstr(className, "Sign") || strstr(className, "BreakIn") ||
        strstr(className, "Summon") || strstr(className, "Push") ||
        strstr(className, "Join") || strstr(className, "Leave") ||
        strstr(className, "Phantom") || strstr(className, "Remove")) {
        LOG_INFO("[PROTOBUF <<] %s (size: %d)", className, size);
    }

    // Call original parser — game processes the message
    bool result = g_originalParse(thisPtr, data, size);

    if (result) {
        // Detect phantom entering world via DS2 soapstone summon.
        // Handled in helper functions to avoid C2712 (__try + C++ objects).
        if (strstr(className, "NotifyJoinGuestPlayer"))
            OnPhantomJoined();

        // NotifyLeaveGuestPlayer is now blocked above, so OnPhantomLeft()
        // here would only fire for messages we explicitly allowed through.
        // If we want to track voluntary phantom departure (BSC), we could add
        // a separate handler here. For now, leave detection is handled by
        // the session manager's timeout logic.
    }

    return result;
}

// ============================================================================
// Installation
// ============================================================================
bool ProtobufHooks::InstallHooks() {
    LOG_INFO("==========================================");
    LOG_INFO("Installing Protobuf Interception Hooks...");
    LOG_INFO("==========================================");

    int hooked = 0;

    // Find SerializeWithCachedSizesToArray via AOB scan
    LOG_INFO("Scanning for SerializeWithCachedSizesToArray...");
    uintptr_t serializeAddr = PatternScanner::FindPattern(
        ProtobufPatterns::SERIALIZE_PATTERN,
        ProtobufPatterns::SERIALIZE_MASK,
        nullptr
    );

    if (serializeAddr) {
        LOG_INFO("  Found at: 0x%p", reinterpret_cast<void*>(serializeAddr));
        if (HookManager::GetInstance().InstallHook(
            reinterpret_cast<void*>(serializeAddr),
            reinterpret_cast<void*>(&SerializeHook),
            reinterpret_cast<void**>(&g_originalSerialize)
        )) {
            LOG_INFO("  HOOKED SerializeWithCachedSizesToArray");
            hooked++;
        } else {
            LOG_ERROR("  Failed to hook SerializeWithCachedSizesToArray");
        }
    } else {
        LOG_ERROR("  SerializeWithCachedSizesToArray pattern NOT FOUND");
        LOG_ERROR("  This is the critical hook - seamless co-op cannot work without it.");
        LOG_ERROR("  Your game version may have a different protobuf implementation.");
    }

    // Find ParseFromArray via AOB scan
    LOG_INFO("Scanning for ParseFromArray...");
    uintptr_t parseAddr = PatternScanner::FindPattern(
        ProtobufPatterns::PARSE_PATTERN,
        ProtobufPatterns::PARSE_MASK,
        nullptr
    );

    if (parseAddr) {
        LOG_INFO("  Found at: 0x%p", reinterpret_cast<void*>(parseAddr));
        if (HookManager::GetInstance().InstallHook(
            reinterpret_cast<void*>(parseAddr),
            reinterpret_cast<void*>(&ParseHook),
            reinterpret_cast<void**>(&g_originalParse)
        )) {
            LOG_INFO("  HOOKED ParseFromArray");
            hooked++;
        } else {
            LOG_ERROR("  Failed to hook ParseFromArray");
        }
    } else {
        LOG_WARNING("  ParseFromArray pattern NOT FOUND (non-critical, logging only)");
    }

    LOG_INFO("==========================================");
    LOG_INFO("Protobuf Hooks Result: %d/2 installed", hooked);
    LOG_INFO("==========================================");

    if (hooked >= 1) {
        LOG_INFO("Protobuf interception active.");
        if (hooked == 1) {
            LOG_WARNING("Only serialize hook installed. Incoming message logging unavailable.");
        }
        return true;
    }

    LOG_ERROR("No protobuf hooks installed. Seamless co-op will NOT work.");
    LOG_ERROR("The mod will still load but cannot prevent disconnections.");
    return false;
}

void ProtobufHooks::UninstallHooks() {
    LOG_INFO("Uninstalling protobuf hooks...");
    g_seamlessActive = false;
}

void ProtobufHooks::SetSeamlessActive(bool active) {
    bool prev = g_seamlessActive.exchange(active);
    if (prev != active) {
        LOG_INFO("[SEAMLESS] Disconnect blocking %s", active ? "ENABLED" : "DISABLED");
    }
}

bool ProtobufHooks::IsSeamlessActive() {
    return g_seamlessActive.load();
}

uint32_t ProtobufHooks::GetBlockedMessageCount() {
    return g_blockedCount.load();
}

uint32_t ProtobufHooks::GetTotalMessageCount() {
    return g_totalCount.load();
}

void ProtobufHooks::AddSessionSteamId(const std::string& steamId) {
    std::lock_guard<std::mutex> lock(g_steamIdMutex);
    // Don't add duplicates
    for (const auto& s : g_sessionSteamIds) {
        if (s == steamId) return;
    }
    g_sessionSteamIds.push_back(steamId);
    LOG_INFO("[SEAMLESS] Added session Steam ID: %s (total: %zu)", steamId.c_str(), g_sessionSteamIds.size());
}

void ProtobufHooks::ClearSessionSteamIds() {
    std::lock_guard<std::mutex> lock(g_steamIdMutex);
    g_sessionSteamIds.clear();
    g_phantomCounter.store(0);
    g_blockedCount.store(0);
    LOG_INFO("[SEAMLESS] Cleared session Steam ID whitelist");
}

std::string ProtobufHooks::GetLocalSteamId() {
    return GetLocalSteamIdInternal();
}
