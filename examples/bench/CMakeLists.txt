cmake_minimum_required(VERSION 3.29)

# c2y and the defer TS take clang 22, newer than the one Xcode ships.
if(APPLE AND NOT CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
  find_program(CMAKE_C_COMPILER clang
    PATHS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin NO_DEFAULT_PATH)
  set(CMAKE_OBJC_COMPILER ${CMAKE_C_COMPILER} CACHE FILEPATH "objc compiler")
endif()

project(bench VERSION 0.0.0 LANGUAGES C)

# civue's own build provides it, otherwise it comes from this checkout.
# an app outside of this repository fetches it instead.
#
#   FetchContent_Declare(civue
#     GIT_REPOSITORY https://code.vexcited.com/ciko/civue.git
#     GIT_TAG main)
include(FetchContent)
if(NOT TARGET civue::civue)
  FetchContent_Declare(civue
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  FetchContent_MakeAvailable(civue)
endif()

add_executable(bench
  src/main.c
  src/recorder.c
  src/words.c
  src/scenes/counter.c
  src/scenes/list.c
  src/scenes/grid.c
  src/scenes/document.c)

# the other frameworks' apps have a menu bar, so this one does too.
if(APPLE)
  enable_language(OBJC)
  target_sources(bench PRIVATE src/menu.m)
  target_compile_options(bench PRIVATE
    $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
endif()

target_link_libraries(bench PRIVATE civue::civue)
target_compile_options(bench PRIVATE -Wall -Wextra)

civue_icons(bench)
