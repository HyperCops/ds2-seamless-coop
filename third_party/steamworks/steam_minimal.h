#pragma once
// Minimal Steamworks bindings for DS2 Seamless Co-op
// Uses manual vtable dispatch — no C++ vtable layout ambiguity.
//
// Tested against steam_api64.dll shipped with DS2 Scholar of the First Sin
// (DLL timestamp: 2013-02-25 — ISteamNetworking005, ISteamMatchmaking008).
//
// IF SOMETHING CRASHES check the slot numbers below against the actual
// ISteamNetworking / ISteamMatchmaking vtable in the DLL with a debugger.

#include <Windows.h>
#include <cstdint>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Core types
// ─────────────────────────────────────────────────────────────────────────────

typedef uint64_t SteamAPICall_t;
static const SteamAPICall_t k_uAPICallInvalid = 0ULL;

struct CSteamID {
    uint64_t m_steamid;
    CSteamID()                     : m_steamid(0) {}
    explicit CSteamID(uint64_t id) : m_steamid(id) {}
    bool     IsValid()             const { return m_steamid != 0; }
    uint64_t ConvertToUint64()     const { return m_steamid; }
    bool operator==(const CSteamID& o) const { return m_steamid == o.m_steamid; }
    bool operator!=(const CSteamID& o) const { return m_steamid != o.m_steamid; }
};

// ─────────────────────────────────────────────────────────────────────────────
// P2P enums
// ─────────────────────────────────────────────────────────────────────────────

enum EP2PSend {
    k_EP2PSendUnreliable              = 0,
    k_EP2PSendUnreliableNoDelay       = 1,
    k_EP2PSendReliable                = 2,
    k_EP2PSendReliableWithBuffering   = 3,
};

// ─────────────────────────────────────────────────────────────────────────────
// Lobby enums
// ─────────────────────────────────────────────────────────────────────────────

enum ELobbyType {
    k_ELobbyTypePrivate     = 0,
    k_ELobbyTypeFriendsOnly = 1,
    k_ELobbyTypePublic      = 2,
    k_ELobbyTypeInvisible   = 3,
};

enum ELobbyComparison {
    k_ELobbyComparisonEqualToOrLessThan    = -2,
    k_ELobbyComparisonLessThan             = -1,
    k_ELobbyComparisonEqual                =  0,
    k_ELobbyComparisonGreaterThan          =  1,
    k_ELobbyComparisonEqualToOrGreaterThan =  2,
    k_ELobbyComparisonNotEqual             =  3,
};

// ─────────────────────────────────────────────────────────────────────────────
// Async result structs (GetAPICallResult)
// ─────────────────────────────────────────────────────────────────────────────

struct LobbyCreated_t {
    static const int k_iCallback = 513;
    uint32_t m_eResult;           // EResult (1 = k_EResultOK)
    uint64_t m_ulSteamIDLobby;
};

struct LobbyMatchList_t {
    static const int k_iCallback = 510;
    uint32_t m_nLobbiesMatching;
};

struct LobbyEnter_t {
    static const int k_iCallback = 504;
    uint64_t m_ulSteamIDLobby;
    uint32_t m_rgfChatPermissions;
    bool     m_bLocked;
    uint32_t m_EChatRoomEnterResponse;  // 1 = k_EChatRoomEnterResponseSuccess
};

// ─────────────────────────────────────────────────────────────────────────────
// Manual vtable dispatch  (x64: first hidden arg is 'this')
// ─────────────────────────────────────────────────────────────────────────────

template<typename Ret, typename... Args>
static inline Ret SteamVCall(void* iface, int slot, Args... args) {
    using Fn = Ret(*)(void*, Args...);
    auto vtable = *reinterpret_cast<void***>(iface);
    auto fn = reinterpret_cast<Fn>(vtable[slot]);
    return fn(iface, args...);
}

// ─────────────────────────────────────────────────────────────────────────────
// ISteamNetworking005  —  verified vtable slots (2013 SDK)
// ─────────────────────────────────────────────────────────────────────────────

namespace SteamNet {
    // 0: SendP2PPacket
    inline bool SendP2PPacket(void* net, CSteamID remote, const void* data,
                               uint32_t size, EP2PSend sendType, int channel = 0) {
        return SteamVCall<bool>(net, 0, remote, data, size, sendType, channel);
    }
    // 1: IsP2PPacketAvailable
    inline bool IsP2PPacketAvailable(void* net, uint32_t* msgSize, int channel = 0) {
        return SteamVCall<bool>(net, 1, msgSize, channel);
    }
    // 2: ReadP2PPacket
    inline bool ReadP2PPacket(void* net, void* dest, uint32_t destSize,
                               uint32_t* msgSize, CSteamID* remoteID, int channel = 0) {
        return SteamVCall<bool>(net, 2, dest, destSize, msgSize, remoteID, channel);
    }
    // 3: AcceptP2PSessionWithUser
    inline bool AcceptP2PSessionWithUser(void* net, CSteamID remote) {
        return SteamVCall<bool>(net, 3, remote);
    }
    // 4: CloseP2PSessionWithUser
    inline bool CloseP2PSessionWithUser(void* net, CSteamID remote) {
        return SteamVCall<bool>(net, 4, remote);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ISteamMatchmaking008  —  vtable slots (2013 SDK)
//
// Slots 0-3  : Favorites (GetFavoriteGameCount, GetFavoriteGame,
//              AddFavoriteGame, RemoveFavoriteGame) — we skip those.
// Slot  4    : RequestLobbyList() -> SteamAPICall_t
// Slot  5    : AddRequestLobbyListStringFilter(key, value, comparison)
// Slot  6    : AddRequestLobbyListNumericalFilter(key, value, comparison)
// Slot  7    : AddRequestLobbyListNearValueFilter(key, value)
// Slot  8    : AddRequestLobbyListFilterSlotsAvailable(slots)
// Slot  9    : AddRequestLobbyListDistanceFilter(filter)
// Slot 10    : AddRequestLobbyListResultCountFilter(count)
// Slot 11    : GetLobbyByIndex(index) -> CSteamID
//              [008 = index 11;  009 adds CompatibleMembersFilter → shifts to 12]
// Slot 12    : CreateLobby(type, maxMembers) -> SteamAPICall_t
// Slot 13    : JoinLobby(lobbyID) -> SteamAPICall_t
// Slot 14    : LeaveLobby(lobbyID)
// Slot 17    : GetNumLobbyMembers(lobbyID) -> int
// Slot 18    : GetLobbyMemberByIndex(lobbyID, index) -> CSteamID
// Slot 19    : GetLobbyData(lobbyID, key) -> const char*
// Slot 20    : SetLobbyData(lobbyID, key, value) -> bool
// Slot 34    : GetLobbyOwner(lobbyID) -> CSteamID
// ─────────────────────────────────────────────────────────────────────────────

namespace SteamMM {
    inline SteamAPICall_t RequestLobbyList(void* mm) {
        return SteamVCall<SteamAPICall_t>(mm, 4);
    }
    inline void AddRequestLobbyListStringFilter(void* mm, const char* key,
                                                 const char* value, ELobbyComparison cmp) {
        SteamVCall<void>(mm, 5, key, value, cmp);
    }
    inline void AddRequestLobbyListResultCountFilter(void* mm, int count) {
        SteamVCall<void>(mm, 10, count);
    }
    inline CSteamID GetLobbyByIndex(void* mm, int index) {
        return SteamVCall<CSteamID>(mm, 11, index);
    }
    inline SteamAPICall_t CreateLobby(void* mm, ELobbyType type, int maxMembers) {
        return SteamVCall<SteamAPICall_t>(mm, 12, type, maxMembers);
    }
    inline SteamAPICall_t JoinLobby(void* mm, CSteamID lobbyID) {
        return SteamVCall<SteamAPICall_t>(mm, 13, lobbyID);
    }
    inline void LeaveLobby(void* mm, CSteamID lobbyID) {
        SteamVCall<void>(mm, 14, lobbyID);
    }
    inline int GetNumLobbyMembers(void* mm, CSteamID lobbyID) {
        return SteamVCall<int>(mm, 17, lobbyID);
    }
    inline CSteamID GetLobbyMemberByIndex(void* mm, CSteamID lobbyID, int index) {
        return SteamVCall<CSteamID>(mm, 18, lobbyID, index);
    }
    inline const char* GetLobbyData(void* mm, CSteamID lobbyID, const char* key) {
        return SteamVCall<const char*>(mm, 19, lobbyID, key);
    }
    inline bool SetLobbyData(void* mm, CSteamID lobbyID, const char* key, const char* value) {
        return SteamVCall<bool>(mm, 20, lobbyID, key, value);
    }
    inline CSteamID GetLobbyOwner(void* mm, CSteamID lobbyID) {
        return SteamVCall<CSteamID>(mm, 34, lobbyID);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ISteamUser  —  slots 1 (BLoggedOn) + 2 (GetSteamID)
// ─────────────────────────────────────────────────────────────────────────────

namespace SteamUsr {
    inline bool BLoggedOn(void* user) {
        return SteamVCall<bool>(user, 1);
    }
    inline CSteamID GetSteamID(void* user) {
        return SteamVCall<CSteamID>(user, 2);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ISteamUtils  —  slots 11 (IsAPICallCompleted) + 13 (GetAPICallResult)
// ─────────────────────────────────────────────────────────────────────────────

namespace SteamUt {
    inline bool IsAPICallCompleted(void* utils, SteamAPICall_t handle, bool* failed) {
        return SteamVCall<bool>(utils, 11, handle, failed);
    }
    inline bool GetAPICallResult(void* utils, SteamAPICall_t handle,
                                  void* pCallback, int cbSize,
                                  int iCallbackExpected, bool* failed) {
        return SteamVCall<bool>(utils, 13, handle, pCallback, cbSize, iCallbackExpected, failed);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Global interface accessors  (loaded from the already-mapped steam_api64.dll)
// ─────────────────────────────────────────────────────────────────────────────

namespace SteamAPI {
    namespace _detail {
        template<const char* Name>
        inline void* GetIface() {
            static void* iface = nullptr;
            if (!iface) {
                HMODULE h = GetModuleHandleA("steam_api64.dll");
                if (h) {
                    auto fn = reinterpret_cast<void*(*)()>(GetProcAddress(h, Name));
                    if (fn) iface = fn();
                }
            }
            return iface;
        }
    }

    inline void* Networking()   { static void* p = nullptr; if (!p) { auto h = GetModuleHandleA("steam_api64.dll"); if (h) { auto f = (void*(*)())GetProcAddress(h,"SteamNetworking");  if (f) p=f(); } } return p; }
    inline void* Matchmaking()  { static void* p = nullptr; if (!p) { auto h = GetModuleHandleA("steam_api64.dll"); if (h) { auto f = (void*(*)())GetProcAddress(h,"SteamMatchmaking"); if (f) p=f(); } } return p; }
    inline void* User()         { static void* p = nullptr; if (!p) { auto h = GetModuleHandleA("steam_api64.dll"); if (h) { auto f = (void*(*)())GetProcAddress(h,"SteamUser");       if (f) p=f(); } } return p; }
    inline void* Utils()        { static void* p = nullptr; if (!p) { auto h = GetModuleHandleA("steam_api64.dll"); if (h) { auto f = (void*(*)())GetProcAddress(h,"SteamUtils");      if (f) p=f(); } } return p; }

    inline void RunCallbacks() {
        static auto fn = reinterpret_cast<void(*)()>(
            GetProcAddress(GetModuleHandleA("steam_api64.dll"), "SteamAPI_RunCallbacks"));
        if (fn) fn();
    }
    inline CSteamID GetLocalSteamID() {
        void* u = User();
        return u ? SteamUsr::GetSteamID(u) : CSteamID{};
    }
    inline bool IsAvailable() {
        return Networking() && Matchmaking() && User() && Utils();
    }
}
