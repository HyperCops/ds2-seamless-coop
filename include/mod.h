#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <string>

namespace DS2Coop {

// Version information
constexpr const char* MOD_VERSION = "1.0.0";
constexpr const char* MOD_NAME = "Dark Souls 2 Seamless Co-op";

// Game version support
enum class GameVersion {
    Unknown,
    SteamLatest,      // Latest Steam version
    CalibrationVer112 // Calibration 1.12
};

// Configuration
struct ModConfig {
    bool enabled = true;
    bool debug_logging = false;
    uint16_t max_players = 6;
    bool allow_invasions = false;
    bool sync_bonfires = true;
    bool sync_items = false;
    bool sync_enemies = false;
    // Session password — same value on both sides triggers automatic connection.
    // Set in ds2_seamless_coop.ini, then press INSERT in-game.
    std::string session_password = "";
};

// Main mod class
class SeamlessCoopMod {
public:
    static SeamlessCoopMod& GetInstance();
    
    bool Initialize();
    void Shutdown();
    
    bool IsInitialized() const { return m_initialized; }
    GameVersion GetGameVersion() const { return m_gameVersion; }
    const ModConfig& GetConfig() const { return m_config; }
    
    void LoadConfig();
    void SaveConfig();

private:
    SeamlessCoopMod() = default;
    ~SeamlessCoopMod() = default;
    SeamlessCoopMod(const SeamlessCoopMod&) = delete;
    SeamlessCoopMod& operator=(const SeamlessCoopMod&) = delete;
    
    bool DetectGameVersion();
    bool InstallHooks();
    void UninstallHooks();
    
    bool m_initialized = false;
    std::atomic<bool> m_running{false}; // contrôle la boucle du thread, séparé de m_initialized
    GameVersion m_gameVersion = GameVersion::Unknown;
    ModConfig m_config;
    HANDLE m_updateThread = nullptr;
};

} // namespace DS2Coop

