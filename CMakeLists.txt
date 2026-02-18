cmake_minimum_required(VERSION 3.16)
project(gamebird_shell LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GBSHELL_BUILD_SERVICES "Build gblibd and gblaunch" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

find_library(SQLITE3_LIBRARY NAMES sqlite3)
if(NOT SQLITE3_LIBRARY)
  find_file(SQLITE3_LIBRARY NAMES libsqlite3.so.0
    PATHS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu /lib/arm-linux-gnueabihf /usr/lib/arm-linux-gnueabihf
  )
endif()
if(NOT SQLITE3_LIBRARY)
  message(FATAL_ERROR "SQLite runtime library not found (expected libsqlite3)")
endif()

set(GBSHELL_DB_SOURCES
  src/db/db.cpp
  src/db/queries.cpp
)

add_executable(gbshell
  src/main.cpp
  src/core/time.cpp
  src/core/logging.cpp
  src/core/settings.cpp
  src/ui/ui_state.cpp
  src/ui/screens/home.cpp
  src/ui/screens/systems.cpp
  src/ui/screens/game_list.cpp
  src/ui/screens/details.cpp
  src/ui/screens/settings.cpp
  src/ui/screens/tools.cpp
  src/ui/widgets/list.cpp
  src/ui/widgets/grid.cpp
  src/ui/widgets/text.cpp
  src/ui/widgets/image.cpp
  src/render/surface_240.cpp
  src/render/glyph_cache.cpp
  src/render/image_cache.cpp
  src/render/theme.cpp
  src/platform/platform_sdl.cpp
  src/platform/platform_fbdev.cpp
  src/platform/input_sdl.cpp
  src/platform/input_evdev.cpp
  src/platform/audio_sdl.cpp
  src/platform/proc.cpp
  ${GBSHELL_DB_SOURCES}
  src/scrape/jobs.cpp
  src/scrape/providers/provider_none.cpp
  src/scrape/providers/provider_screenscraper.cpp
  src/scrape/providers/provider_local_dat.cpp
)

target_include_directories(gbshell PRIVATE src)
target_link_libraries(gbshell PRIVATE PkgConfig::SDL2 ${SQLITE3_LIBRARY})
target_compile_options(gbshell PRIVATE -Wall -Wextra -Wpedantic)

if(GBSHELL_BUILD_SERVICES)
  add_executable(gblibd
    services/gblibd/main.cpp
    src/core/time.cpp
    src/core/logging.cpp
    ${GBSHELL_DB_SOURCES}
    src/scrape/jobs.cpp
    src/scrape/providers/provider_none.cpp
    src/scrape/providers/provider_local_dat.cpp
  )
  target_include_directories(gblibd PRIVATE src)
  target_link_libraries(gblibd PRIVATE ${SQLITE3_LIBRARY})

  add_executable(gblaunch
    services/gblaunch/main.cpp
    src/core/logging.cpp
    src/core/time.cpp
    src/platform/proc.cpp
    src/db/db.cpp
  )
  target_include_directories(gblaunch PRIVATE src)
  target_link_libraries(gblaunch PRIVATE ${SQLITE3_LIBRARY})

  target_compile_options(gblibd PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(gblaunch PRIVATE -Wall -Wextra -Wpedantic)
endif()
