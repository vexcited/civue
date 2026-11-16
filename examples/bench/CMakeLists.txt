cmake_minimum_required(VERSION 3.29)
project(bench VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

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
