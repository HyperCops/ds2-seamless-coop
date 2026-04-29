// In-game ImGui overlay menu for Seamless Co-op
//
// Rendered via the DX11 Present hook in renderer.cpp.
// INSERT toggles the menu open/closed.
//
// States:
//   Main    — status + HOST / JOIN / LEAVE buttons
//   Host    — enter password, see your IP, start hosting
//   Join    — enter IP + password, connect
//
// No popup windows, no alt-tab, no file editing.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>

#include "imgui.h"
#include "../../include/ui.h"
#include "../../include/mod.h"
#include "../../include/session.h"
#include "../../include/network.h"
#include "../../include/hooks.h"
#include "../../include/sync.h"
#include "../../include/utils.h"

#include <sstream>
#include <string>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wininet.lib")

using namespace DS2Coop::Utils;
using namespace DS2Coop::Session;
using namespace DS2Coop::Network;

namespace DS2Coop::UI {

// ============================================================================
// Session code helpers
//
// A session code is: "DS2-" + Base64("IP:PORT:password")
//
// Example: "85.23.114.7:27015:coop" → DS2-ODUuMjMuMTE0Ljc6MjcwMTU6Y29vcA==
//
// Encoding this way means:
//   • A single string the host pastes to their friend (Discord, chat, …)
//   • The join menu auto-detects "DS2-" and decodes it automatically
//   • No IP, no port, no separate password field for the guest
// ============================================================================

static const char kB64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string Base64Encode(const std::string& in) {
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(kB64Chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6)
        out.push_back(kB64Chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4)
        out.push_back('=');
    return out;
}

static std::string Base64Decode(const std::string& in) {
    int T[256];
    memset(T, -1, sizeof(T));
    for (int i = 0; i < 64; i++) T[(unsigned char)kB64Chars[i]] = i;
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// Builds a DS2-<base64> session code from IP, port and password.
static std::string MakeSessionCode(const std::string& ip, uint16_t port, const std::string& password) {
    if (ip.empty() || password.empty()) return "";
    std::string raw = ip + ":" + std::to_string(port) + ":" + password;
    return std::string("DS2-") + Base64Encode(raw);
}

// Returns true + fills ip/port/password if 'code' is a valid DS2- session code.
static bool ParseSessionCode(const std::string& code,
                             std::string& outIP, uint16_t& outPort,
                             std::string& outPassword) {
    if (code.size() < 5 || code.substr(0, 4) != "DS2-") return false;
    std::string raw = Base64Decode(code.substr(4));
    // raw = "IP:PORT:password"
    size_t p1 = raw.find(':');
    if (p1 == std::string::npos) return false;
    size_t p2 = raw.find(':', p1 + 1);
    if (p2 == std::string::npos) return false;

    outIP       = raw.substr(0, p1);
    std::string portStr = raw.substr(p1 + 1, p2 - p1 - 1);
    outPassword = raw.substr(p2 + 1);

    if (outIP.empty() || portStr.empty() || outPassword.empty()) return false;
    try { outPort = static_cast<uint16_t>(std::stoi(portStr)); }
    catch (...) { return false; }
    return true;
}

// ============================================================================
// Helper: copy text to clipboard
// ============================================================================
static void CopyToClipboard(const std::string& text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (hMem) {
        memcpy(GlobalLock(hMem), text.c_str(), text.size() + 1);
        GlobalUnlock(hMem);
        SetClipboardData(CF_TEXT, hMem);
    }
    CloseClipboard();
}

// ============================================================================
// Helper: fetch public IP (async, runs once in background)
// ============================================================================
static std::string g_publicIP;
static std::atomic<bool> g_publicIPFetched{false};
static std::atomic<bool> g_publicIPFetching{false};

static void FetchPublicIPThread() {
    // Winsock is already initialized by PeerManager — don't call WSAStartup/Cleanup
    // here or we risk decrementing the global refcount and killing active sockets.

    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo("api.ipify.org", "80", &hints, &result) != 0) {
        g_publicIPFetched = true;
        return;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(result);
        g_publicIPFetched = true;
        return;
    }

    if (connect(sock, result->ai_addr, (int)result->ai_addrlen) != 0) {
        closesocket(sock);
        freeaddrinfo(result);
        g_publicIPFetched = true;
        return;
    }
    freeaddrinfo(result);

    const char* request = "GET / HTTP/1.1\r\nHost: api.ipify.org\r\nConnection: close\r\n\r\n";
    send(sock, request, (int)strlen(request), 0);

    char buf[512] = {};
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int n = recv(sock, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    closesocket(sock);

    // Parse IP from HTTP response body (last line after \r\n\r\n)
    char* body = strstr(buf, "\r\n\r\n");
    if (body) {
        body += 4;
        // Trim whitespace
        while (*body && (*body == ' ' || *body == '\r' || *body == '\n')) body++;
        char* end = body;
        while (*end && *end != '\r' && *end != '\n' && *end != ' ') end++;
        *end = '\0';
        if (strlen(body) >= 7 && strlen(body) <= 15) { // basic IP length check
            g_publicIP = body;
        }
    }
    g_publicIPFetched = true;
}

static void EnsurePublicIPFetched() {
    bool expected = false;
    if (g_publicIPFetching.compare_exchange_strong(expected, true)) {
        std::thread(FetchPublicIPThread).detach();
    }
}

// ============================================================================
// Helper: get local IP
// ============================================================================
static std::string GetLocalIP() {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) return "unknown";

    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostname, nullptr, &hints, &result) != 0) return "unknown";

    std::string hamachiIP, lanIP, other;
    for (auto* p = result; p; p = p->ai_next) {
        if (p->ai_family != AF_INET) continue;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((sockaddr_in*)p->ai_addr)->sin_addr, ip, sizeof(ip));
        std::string s(ip);
        if (s.substr(0,4) == "127.") continue;
        if (s.substr(0,3) == "25." || s.substr(0,2) == "5.") hamachiIP = s;
        else if (s.substr(0,8) == "192.168." || s.substr(0,3) == "10." || s.substr(0,4) == "172.")
            { if (lanIP.empty()) lanIP = s; }
        else if (other.empty()) other = s;
    }
    freeaddrinfo(result);
    if (!hamachiIP.empty()) return hamachiIP;
    if (!lanIP.empty())     return lanIP;
    if (!other.empty())     return other;
    return "unknown";
}

// ============================================================================
// Overlay singleton
// ============================================================================

// INSERT handler — toggle menu.
// With Steamworks P2P there is no "smart connect" based on server_ip.
// The user chooses Host or Join from the menu.
void Overlay::OnInsertPressed() {
    if (m_visible) {
        m_visible = false;
        return;
    }
    m_visible      = true;
    m_currentState = MenuState::Main;
}

Overlay& Overlay::GetInstance() {
    static Overlay instance;
    return instance;
}

void Overlay::Initialize() {
    if (m_initialized) return;
    // Actual ImGui init happens in renderer.cpp when Present is first called
    m_initialized = true;
    LOG_INFO("Overlay initialized");
}

void Overlay::Shutdown() {
    if (!m_initialized) return;
    m_initialized = false;
    m_notifications.clear();
}

void Overlay::ShowConnectionMenu() {
    m_visible = true;
    m_currentState = MenuState::Main;
}

void Overlay::ShowPlayerList() {
    m_visible = true;
    m_currentState = MenuState::PlayerList;
}

void Overlay::ShowNotification(const std::string& message, float duration) {
    Notification n;
    n.message = message;
    n.timeRemaining = duration;
    n.id = m_nextNotifId++;
    {
        std::lock_guard<std::mutex> lock(m_notifMutex);
        m_notifications.push_back(n);
    }
    LOG_INFO("Notification: %s", message.c_str());
}

// ============================================================================
// Main render — called every frame from HookedPresent
// ============================================================================
void Overlay::Render() {
    // Always render notifications and player HUD (even when menu is closed)
    RenderNotifications();
    RenderPlayerHUD();

    if (!m_visible) return;

    // Center the window on screen
    ImGuiIO& io = ImGui::GetIO();
    float cx = io.DisplaySize.x * 0.5f;
    float cy = io.DisplaySize.y * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(cx, cy), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Always); // auto height

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoScrollbar;

    switch (m_currentState) {
        case MenuState::Main:       RenderMainMenu();  break;
        case MenuState::Host:       RenderHostMenu();  break;
        case MenuState::Join:       RenderJoinMenu();  break;
        case MenuState::PlayerList: RenderPlayerList(); break;
        default: break;
    }
}

// ============================================================================
// Main menu
// ============================================================================
void Overlay::RenderMainMenu() {
    auto& sessionMgr = SessionManager::GetInstance();
    bool inSession = sessionMgr.IsActive();

    const char* title = inSession ? "SEAMLESS CO-OP  [ACTIVE]###main" : "SEAMLESS CO-OP###main";
    if (!ImGui::Begin(title, &m_visible, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar)) {
        ImGui::End();
        return;
    }

    if (inSession) {
        auto players = sessionMgr.GetPlayers();

        ImGui::TextDisabled("Session Active");
        ImGui::Separator();

        uint64_t localId = PeerManager::GetInstance().GetLocalPlayerId();

        for (const auto& p : players) {
            bool isLocal = (p.playerId == 0 || p.playerId == localId);

            // Name + (you) tag
            if (isLocal)
                ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "%s  (you)", p.playerName.c_str());
            else if (!p.isAlive && p.maxHealth > 0)
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s  [dead]", p.playerName.c_str());
            else
                ImGui::Text("%s", p.playerName.c_str());

            // HP bar (only if we have valid data)
            if (p.maxHealth > 0) {
                float frac = (float)p.health / (float)p.maxHealth;
                if (frac < 0.0f) frac = 0.0f;
                if (frac > 1.0f) frac = 1.0f;
                ImVec4 barColor = frac > 0.5f ? ImVec4(0.2f, 0.7f, 0.2f, 1.0f)
                                : frac > 0.25f ? ImVec4(0.8f, 0.6f, 0.1f, 1.0f)
                                :               ImVec4(0.8f, 0.1f, 0.1f, 1.0f);
                char hpLabel[32];
                snprintf(hpLabel, sizeof(hpLabel), "%d / %d", p.health, p.maxHealth);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
                ImGui::ProgressBar(frac, ImVec2(-1, 6), "");
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();
        }

        ImGui::Separator();
        ImGui::Text("%zu player%s", players.size(), players.size() == 1 ? "" : "s");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Share session code (visible only if we are the host and have codes)
        if (!m_activePublicCode.empty() || !m_activeLANCode.empty()) {
            ImGui::TextDisabled("Share session code:");
            ImGui::Separator();

            if (!m_activePublicCode.empty()) {
                ImGui::TextDisabled("  Internet:");
                ImGui::SameLine();
                if (ImGui::SmallButton("Copy##scodepub")) {
                    CopyToClipboard(m_activePublicCode);
                    ShowNotification("Internet session code copied!", 2.5f);
                }
            }
            if (!m_activeLANCode.empty()) {
                ImGui::TextDisabled("  LAN/VPN: ");
                ImGui::SameLine();
                if (ImGui::SmallButton("Copy##scodelan")) {
                    CopyToClipboard(m_activeLANCode);
                    ShowNotification("LAN session code copied!", 2.5f);
                }
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }

        // Seamless tools
        ImGui::TextDisabled("Tools");
        ImGui::Separator();

        if (ImGui::Button("Grant Soapstones", ImVec2(-1, 0))) {
            if (DS2Coop::Sync::PlayerSync::GetInstance().GrantSoapstones()) {
                ShowNotification("Soapstones added to inventory!", 4.0f);
            } else {
                ShowNotification("Could not grant items. Try again in-game.", 4.0f);
            }
        }

        ImGui::Spacing();

        // Emergency teleport — only useful for guests (grayed out if we are host)
        bool isHost = DS2Coop::Network::PeerManager::GetInstance().IsHost();
        if (isHost) {
            ImGui::BeginDisabled(true);
            ImGui::Button("⚡  TP sur l'hôte  (vous êtes l'hôte)", ImVec2(-1, 0));
            ImGui::EndDisabled();
        } else {
            // Show cooldown in button label
            static DWORD s_lastTpMs = 0;
            constexpr DWORD TP_CD = 10000;
            DWORD elapsed = GetTickCount() - s_lastTpMs;
            bool onCooldown = (s_lastTpMs > 0 && elapsed < TP_CD);

            char tpLabel[64];
            if (onCooldown) {
                DWORD secs = (TP_CD - elapsed) / 1000 + 1;
                snprintf(tpLabel, sizeof(tpLabel), "⚡  TP sur l'hôte  (%us)", secs);
            } else {
                snprintf(tpLabel, sizeof(tpLabel), "⚡  TP sur l'hôte");
            }

            ImGui::BeginDisabled(onCooldown);
            if (ImGui::Button(tpLabel, ImVec2(-1, 0))) {
                if (DS2Coop::Sync::PlayerSync::GetInstance().TeleportToHost()) {
                    s_lastTpMs = GetTickCount();
                } else {
                    // Notification already shown by TeleportToHost()
                }
            }
            ImGui::EndDisabled();

            if (!onCooldown) {
                ImGui::TextDisabled("  Utiliser si bloqué ou déconnecté de l'hôte.");
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Leave Session", ImVec2(-1, 0))) {
            sessionMgr.LeaveSession();
            DS2Coop::Hooks::ProtobufHooks::SetSeamlessActive(false);
            // Clear cached session codes so they don't appear next time
            m_activePublicCode.clear();
            m_activeLANCode.clear();
            m_codeInput[0] = '\0';
            m_visible = false;
            ShowNotification("Left session.", 3.0f);
        }

    } else {
        // Not in session — show Host / Join buttons
        ImGui::TextDisabled("Seamless Co-op — hors session");
        ImGui::Spacing();

        auto state = sessionMgr.GetState();
        if (state == DS2Coop::Session::SessionState::Connecting) {
            ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.2f, 1.0f), "Connexion Steam en cours...");
            ImGui::Spacing();
        } else if (state == DS2Coop::Session::SessionState::Error) {
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "Erreur de connexion — verifiez les logs.");
            ImGui::Spacing();
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Non connecte");
            ImGui::Spacing();
        }

        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Heberger une session", ImVec2(-1, 0))) {
            m_currentState = MenuState::Host;
        }
        ImGui::Spacing();
        if (ImGui::Button("Rejoindre une session", ImVec2(-1, 0))) {
            m_currentState = MenuState::Join;
        }

        // Rejoin last session shortcut
        std::string lastPw;
        if (SessionManager::GetLastPassword(lastPw)) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextDisabled("Derniere session : mot de passe \"%s\"", lastPw.c_str());
            ImGui::Spacing();
            if (ImGui::Button("Retenter la connexion", ImVec2(-1, 0))) {
                if (sessionMgr.JoinSession(lastPw)) {
                    ShowNotification("Recherche du lobby en cours...", 5.0f);
                } else {
                    ShowNotification("Echec. Steam disponible ?", 4.0f);
                }
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("INSERT pour fermer");

    ImGui::End();
}

// ============================================================================
// Host menu — Steam lobby (no IP, no session codes)
// ============================================================================
void Overlay::RenderHostMenu() {
    if (!ImGui::Begin("HEBERGER###main", &m_visible,
                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Cree un lobby Steam prive.");
    ImGui::TextDisabled("Donnez le mot de passe a vos amis pour qu'ils puissent rejoindre.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Password field ──────────────────────────────────────────────────────
    ImGui::TextDisabled("Mot de passe de session :");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##password", m_inputPassword, sizeof(m_inputPassword));
    ImGui::Spacing();

    bool hasPassword = strlen(m_inputPassword) > 0;
    if (hasPassword) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.9f, 0.4f, 1.0f));
        ImGui::Text("  Partagez ce mot de passe avec vos amis.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
    } else {
        ImGui::TextDisabled("  Entrez un mot de passe pour commencer.");
        ImGui::Spacing();
    }

    ImGui::Separator();
    ImGui::Spacing();

    // ── Start / Back ───────────────────────────────────────────────────────
    ImGui::BeginDisabled(!hasPassword);
    if (ImGui::Button("Demarrer l'hebergement", ImVec2(-1, 0))) {
        auto& sessionMgr = SessionManager::GetInstance();
        if (sessionMgr.CreateSession(m_inputPassword)) {
            ShowNotification("Creation du lobby Steam...", 4.0f);
            m_currentState = MenuState::Main;
        } else {
            ShowNotification("Echec de creation de session. Verifiez les logs.", 4.0f);
        }
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (ImGui::Button("Retour", ImVec2(-1, 0))) {
        m_currentState = MenuState::Main;
    }

    ImGui::End();
}

// ============================================================================
// Join menu — Steam lobby discovery by password
// ============================================================================
void Overlay::RenderJoinMenu() {
    if (!ImGui::Begin("REJOINDRE###main", &m_visible,
                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Rejoindre la session Steam d'un ami.");
    ImGui::TextDisabled("Entrez le meme mot de passe que l'hote.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Password field ──────────────────────────────────────────────────────
    ImGui::TextDisabled("Mot de passe de session :");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##pw", m_inputPassword, sizeof(m_inputPassword));
    ImGui::Spacing();

    bool hasPassword = strlen(m_inputPassword) > 0;

    if (!hasPassword) {
        ImGui::TextDisabled("Entrez le mot de passe communique par l'hote.");
        ImGui::Spacing();
    }

    ImGui::Separator();
    ImGui::Spacing();

    // ── Join button ─────────────────────────────────────────────────────────
    ImGui::BeginDisabled(!hasPassword);
    if (ImGui::Button("Rejoindre la session", ImVec2(-1, 0))) {
        auto& sessionMgr = SessionManager::GetInstance();
        if (sessionMgr.JoinSession(m_inputPassword)) {
            ShowNotification("Recherche du lobby Steam...", 5.0f);
            m_currentState = MenuState::Main;
        } else {
            ShowNotification("Impossible de demarrer la recherche. Steam disponible ?", 4.0f);
        }
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (ImGui::Button("Retour", ImVec2(-1, 0))) {
        m_currentState = MenuState::Main;
    }

    ImGui::End();
}

// ============================================================================
// Player list (unused state, integrated into Main now)
// ============================================================================
void Overlay::RenderPlayerList() {
    m_currentState = MenuState::Main;
}

// ============================================================================
// Player HUD — always-on party panel (Yui-style)
//
// Shown in the top-right corner whenever a co-op session is active,
// regardless of whether the INSERT menu is open.
//
// Each entry shows:
//   [██████████] Character Name  ← HP bar + name
//   or
//   [░░░░░░░░░░] Character Name  [MORT]  ← greyed out when dead
//
// The local player is highlighted in green, remote players in white.
// ============================================================================
void Overlay::RenderPlayerHUD() {
    auto& sessionMgr = SessionManager::GetInstance();
    if (!sessionMgr.IsActive()) return;

    auto players = sessionMgr.GetPlayers();
    if (players.empty()) return;

    ImGuiIO& io = ImGui::GetIO();

    // Anchor: top-right, small margin
    const float panelW = 220.0f;
    const float marginX = 12.0f, marginY = 12.0f;
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x - panelW - marginX, marginY),
        ImGuiCond_Always, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(panelW, 0), ImGuiCond_Always); // auto height
    ImGui::SetNextWindowBgAlpha(0.55f);

    ImGuiWindowFlags hudFlags =
        ImGuiWindowFlags_NoDecoration   |
        ImGuiWindowFlags_NoInputs        |
        ImGuiWindowFlags_NoNav           |
        ImGuiWindowFlags_NoMove          |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoFocusOnAppearing    |
        ImGuiWindowFlags_AlwaysAutoResize;

    if (!ImGui::Begin("##playerhud", nullptr, hudFlags)) {
        ImGui::End();
        return;
    }

    uint64_t localId = PeerManager::GetInstance().GetLocalPlayerId();

    for (const auto& p : players) {
        bool isLocal = (p.playerId == 0 || p.playerId == localId);
        bool isDead  = (!p.isAlive && p.maxHealth > 0);

        // ── HP bar ──────────────────────────────────────────────────────────
        float hpFrac = 1.0f;
        if (p.maxHealth > 0) {
            hpFrac = (float)p.health / (float)p.maxHealth;
            if (hpFrac < 0.0f) hpFrac = 0.0f;
            if (hpFrac > 1.0f) hpFrac = 1.0f;
        }

        ImVec4 barColor;
        if (isDead) {
            barColor = ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
        } else if (hpFrac > 0.6f) {
            barColor = ImVec4(0.25f, 0.75f, 0.35f, 1.0f);  // green
        } else if (hpFrac > 0.3f) {
            barColor = ImVec4(0.85f, 0.65f, 0.10f, 1.0f);  // orange
        } else {
            barColor = ImVec4(0.85f, 0.20f, 0.15f, 1.0f);  // red
        }

        // Bar width = 60% of panel, label takes the rest
        const float barWidth = panelW * 0.55f - 8.0f;

        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.12f, 0.12f, 0.85f));
        ImGui::ProgressBar(isDead ? 0.0f : hpFrac, ImVec2(barWidth, 8.0f), "");
        ImGui::PopStyleColor(2);

        ImGui::SameLine(0, 6);

        // ── Name + ping ─────────────────────────────────────────────────────
        const std::string& name = p.playerName.empty() ? "(unnamed)" : p.playerName;

        if (isDead) {
            ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 1.0f), "%s", name.c_str());
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.55f, 0.30f, 0.30f, 1.0f), "[dead]");
        } else if (isLocal) {
            ImGui::TextColored(ImVec4(0.50f, 1.0f, 0.55f, 1.0f), "%s", name.c_str());
        } else {
            ImGui::TextUnformatted(name.c_str());

            // Ping indicator — only for remote players with a known ping
            if (p.ping_ms > 0) {
                ImGui::SameLine();
                ImVec4 pingColor;
                if      (p.ping_ms <  80) pingColor = {0.35f, 0.85f, 0.35f, 0.85f}; // green  < 80ms
                else if (p.ping_ms < 150) pingColor = {0.90f, 0.75f, 0.20f, 0.85f}; // yellow < 150ms
                else                      pingColor = {0.90f, 0.25f, 0.20f, 0.85f}; // red    >= 150ms
                char pingBuf[16];
                snprintf(pingBuf, sizeof(pingBuf), "%ums", p.ping_ms);
                ImGui::TextColored(pingColor, "%s", pingBuf);
            }
        }
    }

    ImGui::End();
}

// ============================================================================
// ShowCenteredNotification — DS2-style centered message
// ============================================================================
void Overlay::ShowCenteredNotification(const std::string& message, float duration, int type) {
    std::lock_guard<std::mutex> lock(m_notifMutex);
    Notification n;
    n.message       = message;
    n.timeRemaining = duration;
    n.id            = m_nextNotifId++;
    n.centered      = true;
    n.type          = type;
    m_centeredNotifs.push_back(n);
}

// ============================================================================
// Notifications — rendered as a small stack in the bottom-left
// Shown even when the menu is closed
// ============================================================================
void Overlay::RenderNotifications() {
    // Snapshot under lock so the update thread can push notifications safely
    std::vector<Notification> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_notifMutex);
        if (m_notifications.empty()) return;
        snapshot = m_notifications;
    }

    float dt = ImGui::GetIO().DeltaTime;

    ImGuiWindowFlags notifFlags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_AlwaysAutoResize;

    ImGuiIO& io = ImGui::GetIO();
    float y = io.DisplaySize.y - 20.0f;

    for (auto it = snapshot.end(); it != snapshot.begin(); ) {
        --it;
        if (it->timeRemaining <= 0.0f) continue;

        float alpha = it->timeRemaining < 1.0f ? it->timeRemaining : 1.0f;
        ImGui::SetNextWindowBgAlpha(alpha * 0.78f);
        ImGui::SetNextWindowPos(ImVec2(20.0f, y), ImGuiCond_Always, ImVec2(0.0f, 1.0f));

        char id[32];
        snprintf(id, sizeof(id), "##notif%u", it->id);
        ImGui::Begin(id, nullptr, notifFlags);
        ImGui::TextUnformatted(it->message.c_str());
        // Use actual rendered window height for gap so notifications don't overlap
        float winH = ImGui::GetWindowHeight();
        ImGui::End();

        y -= winH + 4.0f;
    }

    // Tick timers and remove expired entries under lock
    {
        std::lock_guard<std::mutex> lock(m_notifMutex);
        for (auto& n : m_notifications) n.timeRemaining -= dt;
        m_notifications.erase(
            std::remove_if(m_notifications.begin(), m_notifications.end(),
                [](const Notification& n) { return n.timeRemaining <= 0.0f; }),
            m_notifications.end()
        );
    }

    // ── Centered DS2-style notifications ─────────────────────────────────────
    std::vector<Notification> centeredSnap;
    {
        std::lock_guard<std::mutex> lock(m_notifMutex);
        if (m_centeredNotifs.empty()) goto tick_centered;
        centeredSnap = m_centeredNotifs;
    }
    {
        // Colors per type: 0=white, 1=amber(join), 2=red(death), 3=cyan(zone)
        static const ImVec4 kColors[] = {
            {1.0f,  1.0f,  1.0f,  1.0f},   // 0 white
            {0.98f, 0.78f, 0.25f, 1.0f},   // 1 amber  — join/leave
            {0.85f, 0.22f, 0.18f, 1.0f},   // 2 red    — death
            {0.40f, 0.88f, 0.95f, 1.0f},   // 3 cyan   — zone
        };

        float screenW = io.DisplaySize.x;
        float screenH = io.DisplaySize.y;
        float baseY   = screenH * 0.72f; // ~lower third, like DS2 covenant messages

        ImGuiWindowFlags cf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
                            | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove
                            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize
                            | ImGuiWindowFlags_NoBringToFrontOnFocus;

        int idx = 0;
        for (auto& n : centeredSnap) {
            if (n.timeRemaining <= 0.0f) { idx++; continue; }
            float alpha  = (n.timeRemaining < 1.0f) ? n.timeRemaining : 1.0f;
            ImVec4 col   = kColors[n.type < 4 ? n.type : 0];
            col.w        = alpha;

            ImGui::SetNextWindowBgAlpha(alpha * 0.72f);
            // Position: centered horizontally, stacked vertically
            ImGui::SetNextWindowPos(
                ImVec2(screenW * 0.5f, baseY - idx * 36.0f),
                ImGuiCond_Always, ImVec2(0.5f, 1.0f));

            char wid[32]; snprintf(wid, sizeof(wid), "##cnotif%u", n.id);
            ImGui::Begin(wid, nullptr, cf);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(n.message.c_str());
            ImGui::PopStyleColor();
            ImGui::End();
            idx++;
        }
    }

tick_centered:
    {
        std::lock_guard<std::mutex> lock(m_notifMutex);
        for (auto& n : m_centeredNotifs) n.timeRemaining -= dt;
        m_centeredNotifs.erase(
            std::remove_if(m_centeredNotifs.begin(), m_centeredNotifs.end(),
                [](const Notification& n) { return n.timeRemaining <= 0.0f; }),
            m_centeredNotifs.end()
        );
    }
}

// HandleInput is now done inside renderer.cpp's HookedPresent / HookedWndProc
void Overlay::HandleInput() {}

} // namespace DS2Coop::UI
