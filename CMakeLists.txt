cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# Header dependency scanning for Makefile generators.
# Do NOT use cl.exe's /showIncludes here: on a non-English Windows the prefix is
# localized and emitted as UTF-8, while CMake writes the NMake makefiles in the
# ANSI code page - the filter never matches, .obj.d files stay empty, and header
# edits silently leave stale objects behind. CMake's own include scanner is
# encoding-independent (lesson inherited from pecia, 2026-10-24).
# ----------------------------------------------------------------------------
if(CMAKE_GENERATOR MATCHES "Makefiles")
    set(CMAKE_DEPENDS_USE_COMPILER FALSE)
endif()

project(AHKDeck VERSION 0.1.0 LANGUAGES C CXX RC)

# Static CRT (/MT): the shipped exe must run with ZERO VC++ redistributable
# dependencies ("单 exe 即装即用"). CMP0091 NEW lets CMAKE_MSVC_RUNTIME_LIBRARY
# control the flag instead of hardcoded /MD; do NOT re-add /MD to
# add_compile_options - the two fight. Third-party libs linked into the exe
# (FLTK at the UI milestone) must be built with the same flag (4_build passes
# -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded to the FLTK configure).
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ----------------------------------------------------------------------------
# Supported target is strictly Windows x64 (64-bit), Win10/11.
# The engine embeds the AutoHotkey_H interpreter (MSVC/Win32 only); there is no
# 32-bit or cross-platform story. Fail the configure loudly instead of shipping
# a silent 32-bit build.
# ----------------------------------------------------------------------------
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "AHKDeck supports only Windows x64 (64-bit). "
        "Configure with a 64-bit toolchain. "
        "Current pointer size: ${CMAKE_SIZEOF_VOID_P} bytes.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ----------------------------------------------------------------------------
# Output policy (project AGENTS.md):
#   * the shipped exe goes to <project>/.build - and only there;
#   * every intermediate (build tree, test binaries) lives in <project>/.temp.
# The CMake build tree itself is .temp/cmake_build (see build/4_build.bat).
# ----------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../.build")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/../.build")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/../.build")
set(AHKDECK_TEMP_BIN "${CMAKE_SOURCE_DIR}/../.temp/bin")

# MSVC: UTF-8 sources (Chinese comments are fine with this flag - without it
# MSVC parses them as GBK and a trailing byte can swallow the next #include),
# strict conformance, conformant preprocessor. CRT staticness is handled by
# CMAKE_MSVC_RUNTIME_LIBRARY above - never re-add /MD or /MT here.
# /W4 is opt-in per target, never global: third-party targets keep their own
# warning level and a global /W4 would print one D9025 per file.
if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:preprocessor)
endif()

# ----------------------------------------------------------------------------
# core - pure logic layer. Must stay free of FLTK, windows.h GUI calls and the
# engine; this is what keeps it unit-testable (see 目录结构说明.md).
# ----------------------------------------------------------------------------
add_library(core STATIC
    core/Version.cpp
    core/TextFile.cpp
    core/Library.cpp
    core/Settings.cpp
    core/Hotkeys.cpp
    core/Json.cpp
    core/AiPolicy.cpp
)
target_include_directories(core PUBLIC "${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_compile_options(core PRIVATE /W4 /WX)
endif()

# ----------------------------------------------------------------------------
# engine - AutoHotkey_H host layer (m010).
# Loads the patched kernel DLL at runtime (NewThread / DeckSetErrorSink), so the
# kernel is NOT linked in - it is the one shipped file next to AHKDeck.exe.
# ----------------------------------------------------------------------------
add_library(engine STATIC
    engine/DeckEngine.cpp
)
target_link_libraries(engine PUBLIC core user32)
if(MSVC)
    target_compile_options(engine PRIVATE /W4 /WX)
endif()

# ----------------------------------------------------------------------------
# agent - the script tools and the MCP endpoint (m063/m064). Depends on core and
# engine only: no FLTK, no HTTP. Everything that drives a tool - the MCP stdio
# server here, and the gate that calls tools without any model - shares this one
# implementation, so the tool list cannot fork into a second truth.
#
# The built-in conversation loop (ai/, m018) is gone: m065 handed the model and
# the retry policy to external agents, which do that better than our 8-step
# loop ever did.
# ----------------------------------------------------------------------------
add_library(agent STATIC
    agent/Toolbox.cpp
    agent/McpServer.cpp
)
target_link_libraries(agent PUBLIC core engine)
if(MSVC)
    target_compile_options(agent PRIVATE /W4 /WX)
endif()

# ----------------------------------------------------------------------------
# ui - FLTK-based interface layer (m014).
# The FLTK static library is built by build/4_build.bat from .3rd/fltk-1.4.5
# with /MT, so it links into the single shipped exe (no DLL next to it beyond
# the kernel). The build tree lives in .temp per AGENTS.md rule 8.
# ----------------------------------------------------------------------------
set(AHKDECK_FLTK_BUILD "${CMAKE_SOURCE_DIR}/../.temp/fltk-build")
if(NOT EXISTS "${AHKDECK_FLTK_BUILD}/FLTKConfig.cmake")
    message(FATAL_ERROR
        "FLTK build tree missing at .temp/fltk-build. "
        "Run build/4_build.bat first - it configures and builds FLTK before AHKDeck.")
endif()
list(APPEND CMAKE_PREFIX_PATH "${AHKDECK_FLTK_BUILD}")
find_package(FLTK 1.4 REQUIRED NO_MODULE)

add_library(deckui STATIC
    ui/Theme.cpp
    ui/NativeHooks.cpp
    ui/DeckUI.cpp
    ui/DeckTitleBar.cpp
    ui/DeckFrame.cpp
    ui/DeckDialog.cpp
    ui/WindowFrame.cpp
    ui/WindowShadow.cpp
    ui/ImeInPlace.cpp
)
# Theme.cpp defines g_popupCheckColor / g_borderColour, the two globals the
# patched FLTK sources read but never define (patches/fltk-1.4.5).
target_include_directories(deckui SYSTEM PUBLIC ${FLTK_INCLUDE_DIRS})
target_link_libraries(deckui PUBLIC engine agent fltk::fltk fltk::images
    comctl32 ole32 shell32 gdi32 dwmapi user32 version imm32)
if(MSVC)
    target_compile_options(deckui PRIVATE /W4 /WX)
endif()

# ----------------------------------------------------------------------------
# Product executable. GUI subsystem: double-clicking AHKDeck.exe must NOT open a
# console window (rules-standing.md 13), so it links with /SUBSYSTEM:WINDOWS.
# The entry stays the CRT's main() flavour (/ENTRY:mainCRTStartup) because every
# code path here is written against main(); the CLI paths re-attach to the parent
# console at runtime instead of getting one from the subsystem.
# ----------------------------------------------------------------------------
add_executable(AHKDeck WIN32
    main.cpp
)
target_link_libraries(AHKDeck PRIVATE core engine deckui)
if(MSVC)
    target_compile_options(AHKDeck PRIVATE /W4 /WX)
    target_link_options(AHKDeck PRIVATE /ENTRY:mainCRTStartup)
endif()

# Windows VERSIONINFO (explorer "file version" fields). The template is
# configured from project(VERSION) so a version bump touches CMakeLists.txt +
# core/Version.h only; the generated .rc is an intermediate under .temp.
configure_file("${CMAKE_SOURCE_DIR}/build/version_info.rc.in"
               "${CMAKE_BINARY_DIR}/version_info.rc" @ONLY)
# The .rc references these two by bare name; copy them next to the generated
# .rc so rc.exe never needs an absolute source path.
configure_file("${CMAKE_SOURCE_DIR}/ui/DeckIcon.h"
               "${CMAKE_BINARY_DIR}/DeckIcon.h" COPYONLY)
configure_file("${CMAKE_SOURCE_DIR}/image/autohotkey.ico"
               "${CMAKE_BINARY_DIR}/autohotkey.ico" COPYONLY)
target_sources(AHKDeck PRIVATE "${CMAKE_BINARY_DIR}/version_info.rc")

# ----------------------------------------------------------------------------
# Tests (ctest gate - build/5_test.bat fails on any failing test).
# Test binaries are intermediates: they go to .temp/bin, never .build.
# ----------------------------------------------------------------------------
enable_testing()

add_executable(test_version
    test/test_version.cpp
)
target_link_libraries(test_version PRIVATE core)
if(MSVC)
    target_compile_options(test_version PRIVATE /W4 /WX)
endif()
set_target_properties(test_version PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${AHKDECK_TEMP_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${AHKDECK_TEMP_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${AHKDECK_TEMP_BIN}")
add_test(NAME version COMMAND test_version)

# m015: the editor's disk codec (UTF-8/BOM/UTF-16LE + line endings). Pure
# logic, so it lives in core and runs without FLTK or the kernel.
set(AHKDECK_TEXTFILE_DIR "${CMAKE_SOURCE_DIR}/../.temp/textfile_gate")
file(MAKE_DIRECTORY "${AHKDECK_TEXTFILE_DIR}")
add_executable(test_textfile
    test/test_textfile.cpp
)
target_link_libraries(test_textfile PRIVATE core)
if(MSVC)
    target_compile_options(test_textfile PRIVATE /W4 /WX)
endif()
set_target_properties(test_textfile PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${AHKDECK_TEMP_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${AHKDECK_TEMP_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${AHKDECK_TEMP_BIN}")
add_test(NAME textfile COMMAND test_textfile)
set_tests_properties(textfile PROPERTIES
    WORKING_DIRECTORY "${AHKDECK_TEXTFILE_DIR}")

# m016: the user's script library (persistence reads/writes/dedup).
set(AHKDECK_LIBRARY_DIR "${CMAKE_SOURCE_DIR}/../.temp/library_gate")
file(MAKE_DIRECTORY "${AHKDECK_LIBRARY_DIR}")
add_executable(test_library
    test/test_library.cpp
)
target_link_libraries(test_library PRIVATE core)
if(MSVC)
    target_compile_options(test_library PRIVATE /W4 /WX)
endif()
set_target_properties(test_library PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${AHKDECK_TEMP_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${AHKDECK_TEMP_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${AHKDECK_TEMP_BIN}")
add_test(NAME library COMMAND test_library)
set_tests_properties(library PROPERTIES
    WORKING_DIRECTORY "${AHKDECK_LIBRARY_DIR}")

# m017: window geometry persistence (hand-editable ui.ini).
set(AHKDECK_SETTINGS_DIR "${CMAKE_SOURCE_DIR}/../.temp/settings_gate")
file(MAKE_DIRECTORY "${AHKDECK_SETTINGS_DIR}")
add_executable(test_settings
    test/test_settings.cpp
)
target_link_libraries(test_settings PRIVATE core)
if(MSVC)
    target_compile_options(test_settings PRIVATE /W4 /WX)
endif()
set_target_properties(test_settings PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${AHKDECK_TEMP_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${AHKDECK_TEMP_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${AHKDECK_TEMP_BIN}")
add_test(NAME settings COMMAND test_settings)
set_tests_properties(settings PROPERTIES
    WORKING_DIRECTORY "${AHKDECK_SETTINGS_DIR}")

# m023: the AHK hotkey grammar -> Win32 modifiers/VK, and the per-script binding
# file. Pure logic: no window, no kernel, so the table itself is the unit under
# test.
set(AHKDECK_HOTKEY_DIR "${CMAKE_SOURCE_DIR}/../.temp/hotkey_gate")
file(MAKE_DIRECTORY "${AHKDECK_HOTKEY_DIR}")
add_executable(test_hotkeys
    test/test_hotkeys.cpp
)
target_link_libraries(test_hotkeys PRIVATE core)
if(MSVC)
    target_compile_options(test_hotkeys PRIVATE /W4 /WX)
endif()
set_target_properties(test_hotkeys PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${AHKDECK_TEMP_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${AHKDECK_TEMP_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${AHKDECK_TEMP_BIN}")
add_test(NAME hotkeys COMMAND test_hotkeys)
set_tests_properties(hotkeys PROPERTIES
    WORKING_DIRECTORY "${AHKDECK_HOTKEY_DIR}")

# m018: JSON + the AI's file boundary.
add_executable(test_json
    test/test_json.cpp
)
target_link_libraries(test_json PRIVATE core)
if(MSVC)
    target_compile_options(test_json PRIVATE /W4 /WX)
endif()
set_target_properties(test_json PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${AHKDECK_TEMP_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${AHKDECK_TEMP_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${AHKDECK_TEMP_BIN}")
add_test(NAME json COMMAND test_json)

set(AHKDECK_POLICY_DIR "${CMAKE_SOURCE_DIR}/../.temp/policy_gate")
file(MAKE_DIRECTORY "${AHKDECK_POLICY_DIR}")
add_executable(test_aipolicy
    test/test_aipolicy.cpp
)
target_link_libraries(test_aipolicy PRIVATE core)
if(MSVC)
    target_compile_options(test_aipolicy PRIVATE /W4 /WX)
endif()
set_target_properties(test_aipolicy PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${AHKDECK_TEMP_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${AHKDECK_TEMP_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${AHKDECK_TEMP_BIN}")
add_test(NAME aipolicy COMMAND test_aipolicy)
set_tests_properties(aipolicy PROPERTIES
    WORKING_DIRECTORY "${AHKDECK_POLICY_DIR}")


# ----------------------------------------------------------------------------
# m011 session gates: several scripts in one host process, and the machine
# readable journal.
#   session  - the test host (the CLI stays single-script on purpose) drives
#              three concurrent sessions plus the routing corner cases;
#   journal  - AHKDeck.exe --journal must yield one parseable JSON object per
#              line, because that stream is what an AI reads.
# Both need the patched kernel, so AHKDECK_KERNEL points at .build, and both run
# with cwd = .temp/session_gate. Heartbeats stay in that cwd; session logs now
# live next to the test exe (m051 portable: .temp/bin/Logs). Both are
# intermediates and must never land in the source tree (AGENTS.md rule 8).
# ----------------------------------------------------------------------------
set(AHKDECK_GATE_DIR "${CMAKE_SOURCE_DIR}/../.temp/session_gate")
file(MAKE_DIRECTORY "${AHKDECK_GATE_DIR}")
set(AHKDECK_KERNEL_DLL "${CMAKE_SOURCE_DIR}/../.build/AutoHotkey64.dll")
set(AHKDECK_PROBE_DIR "${CMAKE_SOURCE_DIR}/test/session_gate/probe")

add_executable(session_gate
    test/session_gate/session_gate.cpp
)
target_link_libraries(session_gate PRIVATE core engine)
if(MSVC)
    target_compile_options(session_gate PRIVATE /W4 /WX)
endif()
set_target_properties(session_gate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${AHKDECK_TEMP_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${AHKDECK_TEMP_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${AHKDECK_TEMP_BIN}")
add_test(NAME session COMMAND session_gate "${AHKDECK_PROBE_DIR}")
set_tests_properties(session PROPERTIES
    WORKING_DIRECTORY "${AHKDECK_GATE_DIR}"
    ENVIRONMENT "AHKDECK_KERNEL=${AHKDECK_KERNEL_DLL}")

add_test(NAME journal COMMAND powershell -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_SOURCE_DIR}/test/session_gate/gate_journal.ps1"
    -Exe "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/AHKDeck.exe"
    -Probe "${AHKDECK_PROBE_DIR}")
set_tests_properties(journal PROPERTIES
    WORKING_DIRECTORY "${AHKDECK_GATE_DIR}"
    ENVIRONMENT "AHKDECK_KERNEL=${AHKDECK_KERNEL_DLL}")

# m013: the shipped exe must not be a console image (rules-standing.md 13).
# The gate self-tests against session_gate.exe, which IS console subsystem, so a
# parser that calls everything "GUI" fails instead of passing vacuously.
add_test(NAME no_console COMMAND powershell -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_SOURCE_DIR}/test/shipping_gate/gate_no_console.ps1"
    -Exe "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/AHKDeck.exe"
    -ConsoleSample "${AHKDECK_TEMP_BIN}/session_gate.exe")

# ----------------------------------------------------------------------------
# m064: the MCP endpoint, in two halves.
#   mcp_selftest - no window, no model. It points the gate's own client at a
#                  deliberately wrong server and demands the cells go RED. A
#                  gate that cannot fail is not a gate, and m056-m060 are the
#                  proof of what happens when we forget that.
#   mcp_stdio    - the real handshake plus the closed loop (write -> run ->
#                  read the kernel's error -> fix -> pass) against the shipped
#                  exe. That exe is the deck window, so this one is labelled
#                  "gui" and 5_test.bat runs it only under AHKDECK_GATE_UI=1.
# ----------------------------------------------------------------------------
set(AHKDECK_MCP_DIR "${CMAKE_SOURCE_DIR}/../.temp/mcp_gate")
file(MAKE_DIRECTORY "${AHKDECK_MCP_DIR}")
find_program(AHKDECK_PYTHON NAMES python python3)
if(NOT AHKDECK_PYTHON)
    message(WARNING "python not found: the mcp gates (m064) are NOT registered")
else()
    add_test(NAME mcp_selftest COMMAND "${AHKDECK_PYTHON}"
        "${CMAKE_SOURCE_DIR}/test/mcp_gate/mcp_client.py" --selftest "${AHKDECK_MCP_DIR}")
    set_tests_properties(mcp_selftest PROPERTIES
        WORKING_DIRECTORY "${AHKDECK_MCP_DIR}"
        TIMEOUT 120)

    add_test(NAME mcp_stdio COMMAND "${AHKDECK_PYTHON}"
        "${CMAKE_SOURCE_DIR}/test/mcp_gate/mcp_client.py"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/AHKDeck.exe" "${AHKDECK_MCP_DIR}")
    set_tests_properties(mcp_stdio PROPERTIES
        WORKING_DIRECTORY "${AHKDECK_MCP_DIR}"
        LABELS "gui"
        TIMEOUT 180)
endif()

# ----------------------------------------------------------------------------
# Shipped scripts distribution (project AGENTS.md rule: scripts are copied to
# .build/Scripts once written). *.ahk under scripts/ rides along with every
# build; other files there are not distributed.
# ----------------------------------------------------------------------------
# Shipped scripts, plus the menu-bridge include they #Include (m023): a managed
# script says "#Include AHKDeckBridge.inc" and expects the file next to it, so the
# include is as much a shipped artifact as the demos.
file(GLOB AHKDECK_SCRIPTS "${CMAKE_SOURCE_DIR}/scripts/*.ahk" "${CMAKE_SOURCE_DIR}/scripts/*.inc")
list(LENGTH AHKDECK_SCRIPTS AHKDECK_SCRIPT_COUNT)
if(AHKDECK_SCRIPT_COUNT GREATER 0)
    # An ALL custom target, not POST_BUILD: editing only scripts/ must still
    # refresh .build/Scripts even when no C++ file changed and the exe target is
    # already up to date (m020 found the stale copy this way).
    add_custom_target(deck_scripts ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Scripts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${AHKDECK_SCRIPTS}
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Scripts"
        COMMENT "Copying scripts/ -> .build/Scripts")
endif()
