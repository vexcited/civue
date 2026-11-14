cmake_minimum_required(VERSION 3.29)
project(layout VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

#   FetchContent_Declare(civue
#     GIT_REPOSITORY https://code.vexcited.com/ciko/civue.git
#     GIT_TAG main)
include(FetchContent)
if(NOT TARGET civue::civue)
  FetchContent_Declare(civue
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  FetchContent_MakeAvailable(civue)
endif()

add_executable(layout src/main.c)
target_link_libraries(layout PRIVATE civue::civue)
target_compile_options(layout PRIVATE -Wall -Wextra)

civue_icons(layout)
