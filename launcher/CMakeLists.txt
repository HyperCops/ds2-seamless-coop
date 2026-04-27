# ─── GUI Installer ────────────────────────────────────────────────────────────
# Graphical installer / mod manager — no command line required.
# Detects DS2 SOTFS path, installs/removes dinput8.dll, launches the game.
add_executable(ds2_installer WIN32
    installer_gui.cpp
)

set_target_properties(ds2_installer PROPERTIES
    OUTPUT_NAME "DS2SeamlessCoopInstaller"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_compile_options(ds2_installer PRIVATE
    $<$<CONFIG:Release>:/MT>
    $<$<CONFIG:Debug>:/MTd>
    /utf-8          # source + execution charset = UTF-8 (fixes garbled French/symbols)
)

target_link_libraries(ds2_installer PRIVATE
    user32
    gdi32
    shell32
    shlwapi
    advapi32
    ole32
    comctl32
)

# No post-build copy needed — CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE
# already points to repo/dist/ so the exe lands there directly.

