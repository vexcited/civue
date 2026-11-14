cmake_minimum_required(VERSION 3.29)
project(hello VERSION 0.1.0 LANGUAGES C)

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

add_executable(hello src/main.c)
target_link_libraries(hello PRIVATE civue::civue)
target_compile_options(hello PRIVATE -Wall -Wextra)

civue_icons(hello)
