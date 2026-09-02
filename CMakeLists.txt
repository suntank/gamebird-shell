cmake_minimum_required(VERSION 3.16)
project(gamebird_shell LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GBSHELL_BUILD_SERVICES "Build gblibd and gblaunch" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
find_package(PNG REQUIRED)

find_library(SQLITE3_LIBRARY NAMES sqlite3)
if(NOT SQLITE3_LIBRARY)
  find_file(SQLITE3_LIBRARY NAMES libsqlite3.so.0
    PATHS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu /lib/arm-linux-gnueabihf /usr/lib/arm-linux-gnueabihf
  )
endif()

if(BUILD_TESTING)
  add_executable(launch_config_test
    tests/launch_config_test.cpp
    src/core/launch_config.cpp
    src/core/logging.cpp
    src/core/time.cpp
    src/db/db.cpp
  )
  target_include_directories(launch_config_test PRIVATE src)
  target_link_libraries(launch_config_test PRIVATE ${SQLITE3_LIBRARY})
  target_compile_options(launch_config_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME launch_config_test COMMAND launch_config_test)

  add_executable(retroarch_input_test
    tests/retroarch_input_test.cpp
    src/core/retroarch_input.cpp
  )
  target_include_directories(retroarch_input_test PRIVATE src)
  target_compile_options(retroarch_input_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME retroarch_input_test COMMAND retroarch_input_test)

  add_executable(library_scan_test
    tests/library_scan_test.cpp
    src/core/logging.cpp
    src/core/time.cpp
    src/db/db.cpp
    src/db/queries.cpp
  )
  target_include_directories(library_scan_test PRIVATE src)
  target_link_libraries(library_scan_test PRIVATE ${SQLITE3_LIBRARY})
  target_compile_options(library_scan_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME library_scan_test COMMAND library_scan_test)

  add_executable(library_views_test
    tests/library_views_test.cpp
    src/core/logging.cpp
    src/core/time.cpp
    src/db/db.cpp
  )
  target_include_directories(library_views_test PRIVATE src)
  target_link_libraries(library_views_test PRIVATE ${SQLITE3_LIBRARY})
  target_compile_options(library_views_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME library_views_test COMMAND library_views_test)

  add_executable(image_cache_test
    tests/image_cache_test.cpp
    src/render/image_cache.cpp
    src/render/surface_240.cpp
  )
  target_include_directories(image_cache_test PRIVATE src)
  target_link_libraries(image_cache_test PRIVATE PNG::PNG)
  target_compile_options(image_cache_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME image_cache_test COMMAND image_cache_test)

  add_executable(artwork_index_test
    tests/artwork_index_test.cpp
    src/core/logging.cpp
    src/core/time.cpp
    src/db/db.cpp
    src/db/queries.cpp
    src/scrape/jobs.cpp
    src/scrape/providers/provider_none.cpp
    src/scrape/providers/provider_local_dat.cpp
  )
  target_include_directories(artwork_index_test PRIVATE src)
  target_link_libraries(artwork_index_test PRIVATE ${SQLITE3_LIBRARY})
  target_compile_options(artwork_index_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME artwork_index_test COMMAND artwork_index_test)

  # Device-only integration probe. It is intentionally not registered with
  # CTest because creating a temporary uinput device requires root on Linux.
  add_executable(evdev_hotplug_integration
    tests/evdev_hotplug_integration.cpp
    src/platform/input_evdev.cpp
    src/core/logging.cpp
    src/core/time.cpp
  )
  target_include_directories(evdev_hotplug_integration PRIVATE src)
  target_compile_options(evdev_hotplug_integration PRIVATE -Wall -Wextra -Wpedantic)
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
  src/core/launch_config.cpp
  src/core/retroarch_input.cpp
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
target_link_libraries(gbshell PRIVATE PkgConfig::SDL2 ${SQLITE3_LIBRARY} PNG::PNG)
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
    src/core/launch_config.cpp
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
