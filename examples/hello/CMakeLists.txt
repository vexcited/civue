cmake_minimum_required(VERSION 3.29)

# c2y and the defer TS take clang 22, newer than the one Xcode ships.
if(APPLE AND NOT CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
  find_program(CMAKE_C_COMPILER clang
    PATHS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin NO_DEFAULT_PATH)
endif()

project(hello VERSION 0.0.0 LANGUAGES C)

#   FetchContent_Declare(civue
#     GIT_REPOSITORY https://code.vexcited.com/ciko/civue.git
#     GIT_TAG main)
include(FetchContent)
if(NOT TARGET civue::civue)
  FetchContent_Declare(civue
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  FetchContent_MakeAvailable(civue)
endif()

add_executable(hello
  src/main.c
  src/look.c
  src/components/add_button.c
  src/components/dark_switch.c
  src/components/volume_slider.c
  src/components/name_field.c)
target_link_libraries(hello PRIVATE civue::civue)
target_compile_options(hello PRIVATE -Wall -Wextra)

civue_icons(hello)
