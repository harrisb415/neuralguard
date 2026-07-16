cmake_minimum_required(VERSION 3.20)
project(NeuralGuard VERSION 1.5.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "NeuralGuard targets Windows only.")
endif()

if(MSVC)
  add_compile_options(/permissive- /EHsc)
endif()

# Common compile definitions for our own targets.
set(NG_DEFS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# --- Vendored SQLite (amalgamation) -----------------------------------------
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
target_compile_definitions(sqlite3 PUBLIC SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_ENABLE_MATH_FUNCTIONS)  # power()/exp() for habit decay in SQL
if(MSVC)
  target_compile_options(sqlite3 PRIVATE /w)  # third-party: silence warnings
endif()

# --- ngcore: reusable core library (db, identity, signer, util) -------------
# Linked by ngd today; by ngctl / the service / ngtray's backend later.
add_library(ngcore STATIC
  src/core/cmd.cpp
  src/core/db.cpp
  src/core/signer.cpp
  src/core/identity.cpp
  src/core/dns.cpp
  src/core/habit.cpp
  src/core/enforcer.cpp
  src/core/flowstats.cpp
  src/core/scorer.cpp
  src/core/prompt.cpp
  src/core/updater.cpp)
target_include_directories(ngcore PUBLIC src)
# ONNX Runtime headers (C++ API) for the anomaly scorer. Headers only - the DLL
# is loaded dynamically at runtime, so nothing links its import lib and the
# engine has no static dependency on the ML runtime.
target_include_directories(ngcore PRIVATE third_party/onnxruntime/include)
target_compile_definitions(ngcore PUBLIC ${NG_DEFS})
target_compile_options(ngcore PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4>)
# fwpuclnt = WFP API; uuid = predefined FWPM_LAYER_*/CONDITION_* GUIDs;
# iphlpapi = GetTcpTable2 + TCP ESTATS (flow bytes); psapi = PID -> image path.
# winhttp = the in-app updater's HTTPS manifest/installer fetch (src/core/updater.cpp).
target_link_libraries(ngcore PUBLIC sqlite3 bcrypt crypt32 wintrust tdh advapi32 ws2_32 fwpuclnt uuid iphlpapi psapi winhttp)

# --- ngmon (Phase 0): WFP net-event printer ---------------------------------
add_executable(ngmon src/ngmon/main.cpp)
target_include_directories(ngmon PRIVATE src)
target_compile_definitions(ngmon PRIVATE ${NG_DEFS})
target_compile_options(ngmon PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4>)
target_link_libraries(ngmon PRIVATE fwpuclnt ws2_32)

# --- ngd (Phase 1): WFP net-event recorder ----------------------------------
add_executable(ngd src/ngd/main.cpp src/ngd/recorder.cpp src/ngd/enforce.cpp src/ngd/service.cpp)
target_compile_definitions(ngd PRIVATE ${NG_DEFS})
target_compile_options(ngd PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4>)
target_link_libraries(ngd PRIVATE ngcore fwpuclnt ws2_32)
# ngd loads onnxruntime.dll dynamically at runtime for Phase-4b shadow scoring.
# Stage it next to the exe so it's found; it's optional - ngd runs fine without.
add_custom_command(TARGET ngd POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/lib/onnxruntime.dll"
    "$<TARGET_FILE_DIR:ngd>/onnxruntime.dll")

# --- ngctl (Phase 2): firewall control (enforcement + panic) ----------------
add_executable(ngctl src/ngctl/main.cpp)
target_compile_definitions(ngctl PRIVATE ${NG_DEFS})
target_compile_options(ngctl PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4>)
target_link_libraries(ngctl PRIVATE ngcore ws2_32)

# --- ngtray: RETIRED ---------------------------------------------------------
# The tray was a separate process only because a LocalSystem service can't show UI
# (session-0 isolation) - but that never meant the FRONTEND had to be two programs.
# Two of them meant two mode pollers, two panic paths that drifted apart, and a
# tray whose "Status" could only shell out to a cmd.exe window, having no UI of its
# own to render into. The WinUI dashboard now owns the icon, the menu, and the
# prompt pipe (gui/NeuralGuard/Tray.cpp), and talks to the service over the command
# pipe instead of spawning a rival engine. src/ngtray/ is kept in the tree for
# history but is no longer built or shipped.
