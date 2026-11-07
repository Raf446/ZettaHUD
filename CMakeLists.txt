cmake_minimum_required(VERSION 3.20)

project(ZettaHUD
    VERSION 3.3.0
    DESCRIPTION "Ultra-lightweight zero-injection hardware telemetry and dynamic viewport HUD for Windows"
    LANGUAGES CXX RC
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT WIN32)
    message(FATAL_ERROR "ZettaHUD requires a 64-bit Windows target platform.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ZettaHUD exclusively targets x64 architectures.")
endif()

add_compile_definitions(
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

if(MSVC)
    add_compile_options(/W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wno-missing-field-initializers)
endif()

# Phase 1 Target: Window Tracker & Shared Memory IPC Test Harness
set(PHASE1_SOURCES
    src/daemon/main_phase1_test.cpp
)
if(MSVC)
    list(APPEND PHASE1_SOURCES resources/app.manifest)
endif()

add_executable(zetta_phase1_test ${PHASE1_SOURCES})
target_include_directories(zetta_phase1_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(zetta_phase1_test PRIVATE
    user32
    psapi
    advapi32
    dwmapi
    shcore
)

# Phase 2 Target: Window Tracker + Core Hardware Telemetry Collectors
set(PHASE2_SOURCES
    src/daemon/main_phase2_test.cpp
)
if(MSVC)
    list(APPEND PHASE2_SOURCES resources/app.manifest)
endif()

add_executable(zetta_phase2_test ${PHASE2_SOURCES})
target_include_directories(zetta_phase2_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(zetta_phase2_test PRIVATE
    user32
    psapi
    advapi32
    dwmapi
    shcore
    dxgi
    pdh
    iphlpapi
    powrprof
    setupapi
    ole32
    oleaut32
    wbemuuid
    uuid
)

# Phase 3 Target: Passive ETW / DWM Frame & Present Analyzer + Hardware Telemetry Daemon
set(PHASE3_SOURCES
    src/daemon/main_phase3_test.cpp
)
if(MSVC)
    list(APPEND PHASE3_SOURCES resources/app.manifest)
endif()

add_executable(zetta_phase3_test ${PHASE3_SOURCES})
target_include_directories(zetta_phase3_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(zetta_phase3_test PRIVATE
    user32
    psapi
    advapi32
    dwmapi
    shcore
    dxgi
    pdh
    iphlpapi
    powrprof
    setupapi
    ole32
    oleaut32
    wbemuuid
    uuid
    tdh
)

# Phase 4 Target: Direct2D Hardware-Accelerated Transparent Click-Through Overlay
set(PHASE4_SOURCES
    src/overlay/main_phase4_overlay.cpp
)
if(WIN32)
    list(APPEND PHASE4_SOURCES resources/zetta.rc)
endif()
if(MSVC)
    list(APPEND PHASE4_SOURCES resources/app.manifest)
endif()

add_executable(zetta_hud_live ${PHASE4_SOURCES})
target_include_directories(zetta_hud_live PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(zetta_hud_live PRIVATE
    user32
    gdi32
    psapi
    advapi32
    dwmapi
    shcore
    dxgi
    d2d1
    dwrite
    pdh
    iphlpapi
    powrprof
    setupapi
    ole32
    oleaut32
    wbemuuid
    uuid
    tdh
)

# Phase 5 Target: ZettaHUD Studio v3.3 Loopback IPC, System Tray & Hot-Reload Bridge
set(PHASE5_SOURCES
    src/studio/main_phase5_studio.cpp
)
if(WIN32)
    list(APPEND PHASE5_SOURCES resources/zetta.rc)
endif()
if(MSVC)
    list(APPEND PHASE5_SOURCES resources/app.manifest)
endif()

add_executable(zetta_studio ${PHASE5_SOURCES})
target_include_directories(zetta_studio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(zetta_studio PRIVATE
    ws2_32
    shell32
    user32
    advapi32
)

add_custom_command(TARGET zetta_hud_live POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:zetta_hud_live>/presets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/presets/default_layout.json"
        "$<TARGET_FILE_DIR:zetta_hud_live>/presets/default_layout.json"
    COMMENT "Copying default layout preset to output directory"
)
