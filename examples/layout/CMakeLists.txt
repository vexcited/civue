cmake_minimum_required(VERSION 3.29)

# c2y and the defer TS take clang 22, newer than the one Xcode ships.
if(APPLE AND NOT CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
  find_program(CMAKE_C_COMPILER clang
    PATHS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin NO_DEFAULT_PATH)
endif()

project(layout VERSION 0.0.0 LANGUAGES C)

# this checkout when the example is in it, otherwise it is fetched.
include(FetchContent)
get_filename_component(local ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
if(NOT FETCHCONTENT_SOURCE_DIR_CIVUE AND EXISTS ${local}/CMakeLists.txt)
  set(FETCHCONTENT_SOURCE_DIR_CIVUE ${local})
endif()
if(NOT TARGET civue::civue)
  FetchContent_Declare(civue
    GIT_REPOSITORY https://code.vexcited.com/ciko/civue.git
    GIT_TAG main)
  FetchContent_MakeAvailable(civue)
endif()

add_executable(layout
  src/main.c
  src/look.c
  src/scrollbars.c
  src/components/header.c
  src/components/grid_card.c
  src/components/list_card.c)
target_link_libraries(layout PRIVATE civue::civue)
target_compile_options(layout PRIVATE -Wall -Wextra)

civue_icons(layout)
