add_library(ds2_seamless_coop SHARED
    core/dllmain.cpp
    core/mod.cpp
    hooks/session_hooks.cpp
    hooks/network_hooks.cpp
    hooks/game_state_hooks.cpp
    network/peer_manager.cpp
    network/packet_handler.cpp
    network/crypto.cpp
    session/session_manager.cpp
    sync/progress_sync.cpp
    sync/player_sync.cpp
    utils/logger.cpp
    utils/memory.cpp
    utils/pattern_scanner.cpp
    utils/address_resolver.cpp
    ui/overlay.cpp
    ui/renderer.cpp
    ui/title_notifier.cpp
)

target_include_directories(ds2_seamless_coop PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${THIRD_PARTY_DIR}/minhook/include
    ${THIRD_PARTY_DIR}/imgui
    ${THIRD_PARTY_DIR}/imgui/backends
)

target_link_libraries(ds2_seamless_coop PRIVATE
    minhook
    imgui
    ws2_32
    d3d11
    dxgi
)

# Set output name and compiler flags
set_target_properties(ds2_seamless_coop PROPERTIES
    OUTPUT_NAME "dinput8"   # DS2 auto-loads dinput8.dll at startup — no launcher needed
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# Static link the C++ runtime to avoid dependency issues
target_compile_options(ds2_seamless_coop PRIVATE
    $<$<CONFIG:Release>:/MT>
    $<$<CONFIG:Debug>:/MTd>
)
# No POST_BUILD copy needed — OUTPUT_NAME "dinput8" + CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE
# already puts dinput8.dll directly in repo/dist/

