cmake_minimum_required(VERSION 3.20)
project(NeuralGuard LANGUAGES C CXX)

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
  src/core/db.cpp
  src/core/signer.cpp
  src/core/identity.cpp
  src/core/dns.cpp
  src/core/habit.cpp
  src/core/enforcer.cpp
  src/core/prompt.cpp)
target_include_directories(ngcore PUBLIC src)
target_compile_definitions(ngcore PUBLIC ${NG_DEFS})
target_compile_options(ngcore PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4>)
# fwpuclnt = WFP API; uuid = predefined FWPM_LAYER_*/CONDITION_* GUIDs.
target_link_libraries(ngcore PUBLIC sqlite3 bcrypt crypt32 wintrust tdh advapi32 ws2_32 fwpuclnt uuid)

# --- ngmon (Phase 0): WFP net-event printer ---------------------------------
add_executable(ngmon src/ngmon/main.cpp)
target_include_directories(ngmon PRIVATE src)
target_compile_definitions(ngmon PRIVATE ${NG_DEFS})
target_compile_options(ngmon PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4>)
target_link_libraries(ngmon PRIVATE fwpuclnt ws2_32)

# --- ngd (Phase 1): WFP net-event recorder ----------------------------------
add_executable(ngd src/ngd/main.cpp src/ngd/recorder.cpp src/ngd/enforce.cpp)
target_compile_definitions(ngd PRIVATE ${NG_DEFS})
target_compile_options(ngd PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4>)
target_link_libraries(ngd PRIVATE ngcore fwpuclnt ws2_32)

# --- ngctl (Phase 2): firewall control (enforcement + panic) ----------------
add_executable(ngctl src/ngctl/main.cpp)
target_compile_definitions(ngctl PRIVATE ${NG_DEFS})
target_compile_options(ngctl PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4>)
target_link_libraries(ngctl PRIVATE ngcore ws2_32)

# --- ngtray (Phase 2c): system-tray UI (native Win32, runs in user session) --
add_executable(ngtray WIN32 src/ngtray/main.cpp)
target_compile_definitions(ngtray PRIVATE ${NG_DEFS})
target_compile_options(ngtray PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4>)
target_link_libraries(ngtray PRIVATE user32 shell32)
