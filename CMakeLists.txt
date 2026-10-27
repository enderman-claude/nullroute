cmake_minimum_required(VERSION 3.16)
project(nullroute VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---- engine: raylib 5.5, fetched and built with the same toolchain ----
include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
FetchContent_Declare(raylib
  URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
)
FetchContent_MakeAvailable(raylib)

# ---- core: the fake machine, plain C99, no dependencies ----
add_library(nr_core STATIC
  src/core/util.c
  src/core/vfs.c
  src/core/shell.c
  src/core/coreutils.c
  src/core/tools.c
  src/core/world.c
  src/core/game.c
)
target_include_directories(nr_core PUBLIC src/core)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(nr_core PRIVATE -Wall -Wextra -pedantic)
endif()

# ---- game: window, menus and the terminal widget ----
add_executable(nullroute src/gui/main.c src/gui/ui.c src/gui/apps.c)
target_include_directories(nullroute PRIVATE src/gui)
target_link_libraries(nullroute PRIVATE nr_core raylib)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(nullroute PRIVATE -Wall -Wextra)
endif()
add_custom_command(TARGET nullroute POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:nullroute>/assets
)

# ---- tests: the core without any window ----
add_executable(nr_tests tests/test_core.c)
target_link_libraries(nr_tests PRIVATE nr_core)
enable_testing()
add_test(NAME core COMMAND nr_tests ${CMAKE_SOURCE_DIR}/assets/world.txt)

install(TARGETS nullroute RUNTIME DESTINATION .)
install(DIRECTORY assets DESTINATION .)
