// Hook system for DS2 Seamless Co-op
//
// Strategy: Hook at the protobuf serialization layer (verified AOB from ds3os)
// rather than guessing internal game function addresses.
//
// The game sends ALL network messages through protobuf serialize/parse.
// By hooking those two functions, we can intercept and block disconnect
// messages (opcodes 0x03F9, 0x03EB, 0x03E9) to keep sessions alive.

#pragma once
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <cstdint>
#include <atomic>

namespace DS2Coop::Hooks {

// ============================================================================
// Hook Manager (MinHook wrapper)
// ============================================================================
class HookManager {
public:
    static HookManager& GetInstance();

    bool Initialize();
    void Shutdown();

    bool InstallHook(void* targetFunc, void* detourFunc, void** originalFunc);
    bool RemoveHook(void* targetFunc);
    bool EnableHooks();
    bool DisableHooks();

private:
    HookManager() = default;
    ~HookManager() = default;
    HookManager(const HookManager&) = delete;
    HookManager& operator=(const HookManager&) = delete;

    bool m_initialized = false;
};

// ============================================================================
// Protobuf Interception Hooks (core of the seamless co-op mechanism)
//
// ds3os discovered that DS2 routes all network messages through protobuf
// serialize/parse functions with known AOB patterns. By hooking these:
//
// 1. We intercept outgoing SerializeWithCachedSizesToArray calls
// 2. We use RTTI to get the protobuf class name (message type)
// 3. If it's a disconnect/leave message, we block it
// 4. The game thinks it sent the disconnect, but nothing went out
// 5. Session stays alive through boss kills, deaths, area transitions
// ============================================================================
namespace ProtobufHooks {
    bool InstallHooks();
    void UninstallHooks();

    // Function signatures from ds3os reverse engineering
    using SerializeFunc = uint8_t*(__fastcall*)(void* thisPtr, uint8_t* target);
    using ParseFunc = bool(__fastcall*)(void* thisPtr, void* data, int size);

    // Control whether disconnect messages are blocked
    void SetSeamlessActive(bool active);
    bool IsSeamlessActive();

    // Stats for debugging
    uint32_t GetBlockedMessageCount();
    uint32_t GetTotalMessageCount();

    // Sign filtering — only show summon signs from session members
    void AddSessionSteamId(const std::string& steamId);
    void ClearSessionSteamIds();
    std::string GetLocalSteamId();
}

// ============================================================================
// Winsock Hooks (connection monitoring + server redirect)
// Hooks Winsock connect() to detect and redirect game server connections.
// ============================================================================
namespace WinsockHooks {
    bool InstallHooks();
    void UninstallHooks();

    // Server redirect configuration
    void SetServerRedirect(const std::string& ip, uint16_t port);
    bool IsRedirectActive();
}

// ============================================================================
// Server Redirect (hostname + RSA key patching in game memory)
// Patches the FromSoft server hostname and RSA public key so the game
// connects to our custom ds3os server instead.
// ============================================================================
namespace ServerRedirect {
    bool PatchHostname(const std::string& newHostname);
    bool PatchRSAKey(const std::string& newPublicKey);
    bool Install(const std::string& serverIp, const std::string& publicKeyPath);
}

// ============================================================================
// Game State Hooks (secondary - for detecting events locally)
// These are optional and use pattern scanning to find game functions.
// If they fail, the mod still works through protobuf interception alone.
// ============================================================================
namespace GameState {
    bool InstallHooks();
    void UninstallHooks();

    // Reset the per-session broadcast-deduplication set.
    // Call at session create and session leave so boss flags are re-broadcast
    // to new peers who joined later.
    void ClearBroadcastFlagCache();

    using PlayerDeathFunc   = void(__fastcall*)(void* playerPtr);
    using BossDefeatedFunc  = void(__fastcall*)(void* bossPtr);
    // SetEventFlag: void __fastcall SetEventFlag(void* flagMan, uint32_t flagId, bool value)
    using SetEventFlagFunc  = void(__fastcall*)(void* flagMan, uint32_t flagId, bool value);
    // ItemGive: void __fastcall ItemGive(void* bag, void* items, int count, int mode)
    using ItemGiveFunc      = void(__fastcall*)(void* bag, void* items, int count, int mode);
}

} // namespace DS2Coop::Hooks
