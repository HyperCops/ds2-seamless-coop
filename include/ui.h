#pragma once

#include <Windows.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>

namespace DS2Coop::UI {

// Title bar notifier — updates window title to show mod is active
class TitleScreenNotifier {
public:
    static TitleScreenNotifier& GetInstance();
    void Start();
    void Stop();

private:
    TitleScreenNotifier() = default;
    ~TitleScreenNotifier();
    HWND FindGameWindow();
    void UpdateThread();
    bool m_running = false;
    std::thread m_thread;
};

// ImGui overlay — in-game menu for hosting/joining sessions
class Overlay {
public:
    static Overlay& GetInstance();

    void Initialize();
    void Shutdown();
    void Render();
    void HandleInput();

    void ShowConnectionMenu();
    void ShowPlayerList();
    // Standard notification (bottom-left, white text)
    void ShowNotification(const std::string& message, float duration = 3.0f);

    // DS2-style centered notification (à la covenant message)
    // type: 0=info (white), 1=join (amber/gold), 2=death (red), 3=zone (cyan)
    void ShowCenteredNotification(const std::string& message, float duration = 4.0f, int type = 0);

    bool IsVisible() const { return m_visible; }
    void SetVisible(bool visible) { m_visible = visible; }
    void Toggle() { m_visible = !m_visible; }

    // INSERT handler — toggle menu open/closed.
    void OnInsertPressed();

private:
    Overlay() = default;
    ~Overlay() = default;
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    void RenderMainMenu();
    void RenderHostMenu();
    void RenderJoinMenu();
    void RenderPlayerList();
    void RenderNotifications();
    void RenderPlayerHUD();     // Always-on 2D party HUD (names + HP bars, Yui-style)

    bool m_visible = false;
    bool m_initialized = false;

    enum class MenuState { Main, Host, Join, PlayerList };
    MenuState m_currentState = MenuState::Main;

    char m_inputIP[128]       = {0};
    char m_inputPassword[128] = {0};
    char m_codeInput[256]     = {0};   // Join-by-code field

    // Session codes generated when hosting starts (DS2-<base64> strings)
    std::string m_activePublicCode;
    std::string m_activeLANCode;

    struct Notification {
        std::string message;
        float timeRemaining;
        uint32_t id   = 0;
        bool centered = false; // true = DS2-style centered message
        int  type     = 0;     // 0=white 1=amber(join) 2=red(death) 3=cyan(zone)
    };
    std::vector<Notification> m_notifications;
    std::vector<Notification> m_centeredNotifs; // separate list for centered messages
    uint32_t m_nextNotifId = 1;
    mutable std::mutex m_notifMutex; // protects m_notifications from concurrent access
};

// DX11 Present hook — installs the ImGui renderer
class OverlayRenderer {
public:
    static OverlayRenderer& GetInstance();

    bool Initialize();
    void Shutdown();

private:
    OverlayRenderer() = default;
    ~OverlayRenderer() = default;
    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    bool m_initialized = false;
};

} // namespace DS2Coop::UI
