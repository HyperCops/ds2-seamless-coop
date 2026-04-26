// DS2 Seamless Co-op — Graphical Installer / Manager
//
// A single-window Win32 application that:
//   1. Auto-detects the DS2 SOTFS Steam installation path
//   2. Shows the current mod status (installed / not installed)
//   3. Installs the mod:  copies dinput8.dll + ds2_seamless_coop.ini template
//                         backs up any pre-existing dinput8.dll
//   4. Removes  the mod:  deletes the mod files, restores backup if present
//   5. Launches the game via Steam (steam://run/335300)
//
// Distribution layout (all in the same folder as this exe):
//   DS2SeamlessCoopInstaller.exe
//   dinput8.dll                   ← the mod
//   ds2_seamless_coop.ini         ← default config (optional, generated if missing)
//
// No command line, no admin rights needed (user's Steam folder is writable).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <ShlObj.h>      // SHBrowseForFolder, IFileDialog
#include <Shlwapi.h>     // PathFileExists
#include <shellapi.h>    // ShellExecute
#include <commdlg.h>
#include <string>
#include <filesystem>
#include <fstream>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")

namespace fs = std::filesystem;

// ─── Control IDs ─────────────────────────────────────────────────────────────
#define IDC_LABEL_PATH   101
#define IDC_EDIT_PATH    102
#define IDC_BTN_BROWSE   103
#define IDC_LABEL_STATUS 104
#define IDC_STATUS_VAL   105
#define IDC_BTN_INSTALL  106
#define IDC_BTN_REMOVE   107
#define IDC_BTN_LAUNCH   108
#define IDC_LABEL_INFO   109

// ─── Constants ────────────────────────────────────────────────────────────────
// Marker file we write in the game folder to track our installation
static const wchar_t* kMarker     = L"ds2sc_installed";
// The DLL the game auto-loads (dinput8 proxy)
static const wchar_t* kModDLL     = L"dinput8.dll";
// Backup name for any vanilla dinput8.dll that was already there
static const wchar_t* kBackupDLL  = L"dinput8.dll.vanilla_backup";
// Steam App ID for DS2 SOTFS
static const wchar_t* kSteamAppID = L"335300";

// ─── Global handles ───────────────────────────────────────────────────────────
static HWND g_hwnd        = nullptr;
static HWND g_hPath       = nullptr;   // Edit: game path
static HWND g_hStatusVal  = nullptr;   // Static: status text
static HWND g_hInstall    = nullptr;   // Button: Install
static HWND g_hRemove     = nullptr;   // Button: Remove
static HWND g_hLaunch     = nullptr;   // Button: Launch
static HFONT g_hFontUI    = nullptr;   // Segoe UI 9pt
static HFONT g_hFontBold  = nullptr;   // Segoe UI 9pt Bold

// Status colours (drawn in WM_CTLCOLORSTATIC)
static COLORREF g_colorOk      = RGB(30,  160, 80);
static COLORREF g_colorWarn    = RGB(200, 100, 20);
static COLORREF g_colorError   = RGB(190, 40,  40);
static COLORREF g_colorCurrent = RGB(100, 100, 100);

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    auto pos = s.rfind(L'\\');
    return (pos != std::wstring::npos) ? s.substr(0, pos) : s;
}

// Read the Steam install path from the registry (tries both 32/64-bit hives)
static std::wstring GetSteamPath() {
    const wchar_t* keys[] = {
        L"SOFTWARE\\WOW6432Node\\Valve\\Steam",
        L"SOFTWARE\\Valve\\Steam"
    };
    for (auto* key : keys) {
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
            wchar_t path[MAX_PATH] = {};
            DWORD sz = sizeof(path);
            if (RegQueryValueExW(hk, L"InstallPath", nullptr, nullptr,
                                 (LPBYTE)path, &sz) == ERROR_SUCCESS) {
                RegCloseKey(hk);
                return path;
            }
            RegCloseKey(hk);
        }
    }
    return L"";
}

// Try to locate the DS2 SOTFS Game\ folder automatically
static std::wstring DetectGamePath() {
    std::wstring steam = GetSteamPath();
    if (steam.empty()) return L"";

    // Standard library path
    std::wstring candidate = steam +
        L"\\steamapps\\common\\Dark Souls II Scholar of the First Sin\\Game";
    if (fs::exists(candidate + L"\\DarkSoulsII.exe")) return candidate;

    // Also check libraryfolders.vdf for additional library paths
    std::wstring vdf = steam + L"\\steamapps\\libraryfolders.vdf";
    std::ifstream f(vdf);
    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            // Lines like:  "path"   "D:\\SteamLibrary"
            if (line.find("\"path\"") != std::string::npos) {
                auto q1 = line.rfind('"', line.size() - 1);
                if (q1 != std::string::npos) {
                    auto q0 = line.rfind('"', q1 - 1);
                    if (q0 != std::string::npos && q1 > q0 + 1) {
                        std::string raw = line.substr(q0 + 1, q1 - q0 - 1);
                        // Unescape backslashes
                        std::string path;
                        for (size_t i = 0; i < raw.size(); i++) {
                            if (raw[i] == '\\' && i + 1 < raw.size() && raw[i+1] == '\\')
                                { path += '\\'; i++; }
                            else path += raw[i];
                        }
                        // Convert to wstring
                        std::wstring wpath(path.begin(), path.end());
                        std::wstring alt = wpath +
                            L"\\steamapps\\common\\Dark Souls II Scholar of the First Sin\\Game";
                        if (fs::exists(alt + L"\\DarkSoulsII.exe")) return alt;
                    }
                }
            }
        }
    }
    return L"";
}

static bool IsValidGamePath(const std::wstring& p) {
    return !p.empty() && fs::exists(p + L"\\DarkSoulsII.exe");
}

static bool IsModInstalled(const std::wstring& gamePath) {
    return IsValidGamePath(gamePath) &&
           fs::exists(gamePath + L"\\" + kMarker);
}

// Write a minimal default config file if none exists yet
static void WriteDefaultConfig(const std::wstring& gamePath) {
    std::wstring cfgPath = gamePath + L"\\ds2_seamless_coop.ini";
    if (fs::exists(cfgPath)) return;
    std::ofstream f(cfgPath);
    if (!f.is_open()) return;
    f << "# DS2 Seamless Co-op configuration\n"
      << "# Edit these values as needed.\n\n"
      << "enabled=true\n"
      << "debug_logging=false\n"
      << "max_players=2\n"
      << "port=27015\n\n"
      << "# Sync settings\n"
      << "allow_invasions=false\n"
      << "sync_bonfires=true\n"
      << "sync_items=true\n"
      << "sync_enemies=false\n\n"
      << "# Custom server (optional)\n"
      << "use_custom_server=false\n"
      << "server_ip=\n"
      << "server_port=50031\n";
}

// Install: copy dinput8.dll (and config) to game folder, write marker
static bool InstallMod(const std::wstring& gamePath, std::wstring& errorOut) {
    if (!IsValidGamePath(gamePath)) {
        errorOut = L"Chemin invalide (DarkSoulsII.exe introuvable).";
        return false;
    }

    std::wstring srcDLL = GetExeDir() + L"\\" + kModDLL;
    if (!fs::exists(srcDLL)) {
        errorOut = L"dinput8.dll introuvable à côté de l'installeur.\n"
                   L"Assurez-vous que le fichier est bien dans le même dossier.";
        return false;
    }

    // Back up any existing dinput8.dll (vanilla anti-cheat stub, etc.)
    std::wstring dstDLL = gamePath + L"\\" + kModDLL;
    if (fs::exists(dstDLL) && !fs::exists(gamePath + L"\\" + kBackupDLL)) {
        std::error_code ec;
        fs::rename(dstDLL, gamePath + L"\\" + kBackupDLL, ec);
        if (ec) {
            errorOut = L"Impossible de sauvegarder l'ancien dinput8.dll :\n" +
                       std::wstring(ec.message().begin(), ec.message().end());
            return false;
        }
    }

    // Copy our mod DLL
    std::error_code ec;
    fs::copy_file(srcDLL, dstDLL, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        errorOut = L"Erreur lors de la copie du mod :\n" +
                   std::wstring(ec.message().begin(), ec.message().end());
        return false;
    }

    // Write default config if missing
    WriteDefaultConfig(gamePath);

    // Write marker
    std::ofstream mark(gamePath + L"\\" + kMarker);
    mark << "DS2 Seamless Co-op installed\n";

    return true;
}

// Remove: delete mod DLL + marker, restore backup if present
static bool RemoveMod(const std::wstring& gamePath, std::wstring& errorOut) {
    std::error_code ec;

    // Delete mod DLL
    std::wstring dstDLL = gamePath + L"\\" + kModDLL;
    if (fs::exists(dstDLL)) {
        fs::remove(dstDLL, ec);
        if (ec) {
            errorOut = L"Impossible de supprimer dinput8.dll :\n" +
                       std::wstring(ec.message().begin(), ec.message().end());
            return false;
        }
    }

    // Restore vanilla backup if present
    std::wstring backup = gamePath + L"\\" + kBackupDLL;
    if (fs::exists(backup)) {
        fs::rename(backup, dstDLL, ec);
        if (ec) {
            // Non-fatal — just warn
            errorOut = L"Avertissement : impossible de restaurer la sauvegarde vanilla.\n"
                       L"Le fichier " + std::wstring(kBackupDLL) + L" est toujours présent.";
        }
    }

    // Delete marker
    std::wstring marker = gamePath + L"\\" + kMarker;
    if (fs::exists(marker)) fs::remove(marker, ec);

    return true;
}

// ─── UI update ────────────────────────────────────────────────────────────────

static void RefreshUI() {
    wchar_t pathBuf[MAX_PATH] = {};
    GetWindowTextW(g_hPath, pathBuf, MAX_PATH);
    std::wstring gamePath(pathBuf);

    bool valid     = IsValidGamePath(gamePath);
    bool installed = IsModInstalled(gamePath);

    if (!valid) {
        SetWindowTextW(g_hStatusVal,
            L"⚠  Chemin invalide — DarkSoulsII.exe introuvable");
        g_colorCurrent = g_colorError;
        EnableWindow(g_hInstall, FALSE);
        EnableWindow(g_hRemove,  FALSE);
        EnableWindow(g_hLaunch,  FALSE);
    } else if (installed) {
        SetWindowTextW(g_hStatusVal, L"✓  Mod installé et actif");
        g_colorCurrent = g_colorOk;
        EnableWindow(g_hInstall, FALSE);
        EnableWindow(g_hRemove,  TRUE);
        EnableWindow(g_hLaunch,  TRUE);
    } else {
        SetWindowTextW(g_hStatusVal, L"○  Mod non installé (jeu en mode vanilla)");
        g_colorCurrent = g_colorWarn;
        EnableWindow(g_hInstall, TRUE);
        EnableWindow(g_hRemove,  FALSE);
        EnableWindow(g_hLaunch,  TRUE);
    }

    InvalidateRect(g_hStatusVal, nullptr, TRUE);
}

// ─── Folder picker (IFileDialog — Vista+) ────────────────────────────────────

static std::wstring BrowseForFolder(HWND parent, const wchar_t* title) {
    IFileDialog* pfd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))))
        return L"";

    DWORD opts = 0;
    pfd->GetOptions(&opts);
    pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    pfd->SetTitle(title);

    if (FAILED(pfd->Show(parent))) { pfd->Release(); return L""; }

    IShellItem* psi = nullptr;
    if (FAILED(pfd->GetResult(&psi))) { pfd->Release(); return L""; }

    wchar_t* name = nullptr;
    psi->GetDisplayName(SIGDN_FILESYSPATH, &name);
    std::wstring result = name ? name : L"";
    CoTaskMemFree(name);
    psi->Release();
    pfd->Release();
    return result;
}

// ─── Window procedure ────────────────────────────────────────────────────────

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE: {
        // Create Segoe UI fonts
        g_hFontUI = CreateFontW(
            -MulDiv(9, GetDeviceCaps(GetDC(hwnd), LOGPIXELSY), 72),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_hFontBold = CreateFontW(
            -MulDiv(9, GetDeviceCaps(GetDC(hwnd), LOGPIXELSY), 72),
            0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

        auto MakeFont = [](HWND h) {
            SendMessageW(h, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            return h;
        };
        auto MakeBold = [](HWND h) {
            SendMessageW(h, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
            return h;
        };

        // Title
        MakeBold(CreateWindowExW(0, L"STATIC",
            L"DS2 Seamless Co-op — Installeur",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            16, 14, 420, 22, hwnd, nullptr, nullptr, nullptr));

        // Separator line
        CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            16, 42, 420, 2, hwnd, nullptr, nullptr, nullptr);

        // Path label
        MakeFont(CreateWindowExW(0, L"STATIC",
            L"Chemin vers DS2 Scholar of the First Sin \\Game :",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            16, 56, 420, 16, hwnd, (HMENU)IDC_LABEL_PATH, nullptr, nullptr));

        // Path edit
        g_hPath = MakeFont(CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            16, 76, 360, 22, hwnd, (HMENU)IDC_EDIT_PATH, nullptr, nullptr));

        // Browse button
        MakeFont(CreateWindowExW(0, L"BUTTON", L"...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            382, 76, 54, 22, hwnd, (HMENU)IDC_BTN_BROWSE, nullptr, nullptr));

        // Separator
        CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            16, 112, 420, 2, hwnd, nullptr, nullptr, nullptr);

        // Status label
        MakeFont(CreateWindowExW(0, L"STATIC", L"Statut :",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            16, 124, 80, 16, hwnd, (HMENU)IDC_LABEL_STATUS, nullptr, nullptr));

        // Status value
        g_hStatusVal = MakeBold(CreateWindowExW(0, L"STATIC",
            L"Chargement...",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            16, 144, 420, 18, hwnd, (HMENU)IDC_STATUS_VAL, nullptr, nullptr));

        // Separator
        CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            16, 174, 420, 2, hwnd, nullptr, nullptr, nullptr);

        // Install button (green-ish)
        g_hInstall = MakeFont(CreateWindowExW(0, L"BUTTON",
            L"✦  Installer le mod",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            16, 184, 200, 30, hwnd, (HMENU)IDC_BTN_INSTALL, nullptr, nullptr));

        // Remove button
        g_hRemove = MakeFont(CreateWindowExW(0, L"BUTTON",
            L"✦  Retirer le mod (vanilla)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            224, 184, 212, 30, hwnd, (HMENU)IDC_BTN_REMOVE, nullptr, nullptr));

        // Launch button
        g_hLaunch = MakeFont(CreateWindowExW(0, L"BUTTON",
            L"▶  Lancer DS2 via Steam",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            16, 222, 420, 30, hwnd, (HMENU)IDC_BTN_LAUNCH, nullptr, nullptr));

        // Info line
        MakeFont(CreateWindowExW(0, L"STATIC",
            L"Le jeu se charge normalement — appuyez sur INSERT en jeu pour ouvrir le menu co-op.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            16, 262, 420, 32, hwnd, (HMENU)IDC_LABEL_INFO, nullptr, nullptr));

        // Auto-detect path on startup
        CoInitialize(nullptr);
        std::wstring detected = DetectGamePath();
        if (!detected.empty()) {
            SetWindowTextW(g_hPath, detected.c_str());
        } else {
            SetWindowTextW(g_hPath, L"(chemin non détecté — parcourir manuellement)");
        }
        RefreshUI();
        return 0;
    }

    // Colour the status label according to state
    case WM_CTLCOLORSTATIC: {
        HWND ctrl = (HWND)lParam;
        if (ctrl == g_hStatusVal) {
            HDC hdc = (HDC)wParam;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, g_colorCurrent);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_COMMAND: {
        WORD id = LOWORD(wParam);

        if (id == IDC_BTN_BROWSE) {
            std::wstring folder = BrowseForFolder(hwnd,
                L"Sélectionnez le dossier Game\\  de DS2 SOTFS");
            if (!folder.empty()) {
                SetWindowTextW(g_hPath, folder.c_str());
                RefreshUI();
            }
        }

        else if (id == IDC_EDIT_PATH && HIWORD(wParam) == EN_CHANGE) {
            RefreshUI();
        }

        else if (id == IDC_BTN_INSTALL) {
            wchar_t pathBuf[MAX_PATH] = {};
            GetWindowTextW(g_hPath, pathBuf, MAX_PATH);
            std::wstring err;
            if (InstallMod(pathBuf, err)) {
                MessageBoxW(hwnd,
                    L"Mod installé avec succès !\n\n"
                    L"Lancez DS2 normalement via Steam,\n"
                    L"puis appuyez sur INSERT pour ouvrir le menu co-op.",
                    L"Installation réussie", MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxW(hwnd, err.c_str(), L"Erreur d'installation", MB_OK | MB_ICONERROR);
            }
            RefreshUI();
        }

        else if (id == IDC_BTN_REMOVE) {
            int confirm = MessageBoxW(hwnd,
                L"Retirer le mod Seamless Co-op ?\n\n"
                L"Le jeu reviendra en mode vanilla.\n"
                L"Vos sauvegardes ne seront pas affectées.",
                L"Confirmer la désinstallation",
                MB_YESNO | MB_ICONQUESTION);
            if (confirm == IDYES) {
                wchar_t pathBuf[MAX_PATH] = {};
                GetWindowTextW(g_hPath, pathBuf, MAX_PATH);
                std::wstring err;
                if (RemoveMod(pathBuf, err)) {
                    MessageBoxW(hwnd,
                        L"Mod retiré. Le jeu est de nouveau en mode vanilla.",
                        L"Désinstallation réussie", MB_OK | MB_ICONINFORMATION);
                } else {
                    // err may be a warning, not a hard failure
                    MessageBoxW(hwnd, err.c_str(), L"Attention", MB_OK | MB_ICONWARNING);
                }
                RefreshUI();
            }
        }

        else if (id == IDC_BTN_LAUNCH) {
            // Launch via Steam protocol — works whether mod is installed or not
            ShellExecuteW(hwnd, L"open",
                (std::wstring(L"steam://run/") + kSteamAppID).c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }

    case WM_DESTROY:
        if (g_hFontUI)   DeleteObject(g_hFontUI);
        if (g_hFontBold) DeleteObject(g_hFontBold);
        CoUninitialize();
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ─── Entry point ──────────────────────────────────────────────────────────────

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DS2CoopInstaller";
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // Fixed-size window (no resize, no maximize — clean installer look)
    const int W = 460, H = 310;
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);

    g_hwnd = CreateWindowExW(
        0,
        L"DS2CoopInstaller",
        L"DS2 Seamless Co-op — Installeur",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sx - W) / 2, (sy - H) / 2,
        W, H,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
