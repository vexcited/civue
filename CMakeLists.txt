cmake_minimum_required(VERSION 3.29)
project(civue VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ${PROJECT_IS_TOP_LEVEL})
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
endif()

option(CIVUE_BUILD_TESTS "build the civue tests and examples" ${PROJECT_IS_TOP_LEVEL})

# a parent project may already provide cilo, otherwise it is fetched.
include(FetchContent)
if(NOT TARGET cilo::cilo)
  FetchContent_Declare(cilo GIT_REPOSITORY ssh://git@vexcited.com/ciko/cilo.git GIT_TAG main)
  FetchContent_MakeAvailable(cilo)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/civue_shaders.cmake)

set(CIVUE_SOURCES src/base.c src/draw.c src/field.c src/icons.c src/numfield.c src/richedit.c src/richtext.c src/theme.c
  src/ui.c src/widgets.c)
set(CIVUE_DARWIN_SOURCES src/darwin/clipboard.c src/darwin/cursors.c src/darwin/filepanel.c src/darwin/gfx.c
  src/darwin/gpu.c src/darwin/objc.c src/darwin/pasteboard.c src/darwin/spell.c src/darwin/svg.c src/darwin/text.c
  src/darwin/window.c)
set(CIVUE_LINUX_SOURCES src/linux/clipboard.c src/linux/cursors.c src/linux/filepanel.c src/linux/gfx.c
  src/linux/pasteboard.c src/linux/spell.c src/linux/svg.c src/linux/text.c src/linux/vk.c src/linux/window.c)

if(APPLE)
  enable_language(OBJC)
  add_library(civue STATIC ${CIVUE_SOURCES} ${CIVUE_DARWIN_SOURCES})
  target_compile_options(civue PRIVATE -fblocks)
  target_link_libraries(civue PUBLIC "-framework Foundation" "-framework AppKit" "-framework Metal"
    "-framework QuartzCore" "-framework CoreText" "-framework CoreGraphics" "-framework ImageIO"
    "-framework CoreFoundation" "-framework UniformTypeIdentifiers" objc)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(civue STATIC ${CIVUE_SOURCES} ${CIVUE_LINUX_SOURCES})
  target_compile_definitions(civue PRIVATE _GNU_SOURCE)
  find_package(Threads REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(CIVUE_DEPS REQUIRED IMPORTED_TARGET vulkan freetype2 harfbuzz fontconfig fribidi librsvg-2.0 cairo
    gio-2.0 wayland-client wayland-cursor xkbcommon enchant-2)
  target_link_libraries(civue PUBLIC PkgConfig::CIVUE_DEPS Threads::Threads m)
  # the wayland protocols past the core one, their client code generated.
  pkg_get_variable(CIVUE_SCANNER_PATH wayland-scanner wayland_scanner)
  find_program(CIVUE_WAYLAND_SCANNER NAMES wayland-scanner HINTS ${CIVUE_SCANNER_PATH} REQUIRED)
  file(GLOB protocols CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/protocols/*.xml)
  set(gen ${CMAKE_CURRENT_BINARY_DIR}/wayland)
  file(MAKE_DIRECTORY ${gen})
  foreach(xml ${protocols})
    get_filename_component(name ${xml} NAME_WE)
    add_custom_command(OUTPUT ${gen}/${name}-client-protocol.h ${gen}/${name}-protocol.c
      COMMAND ${CIVUE_WAYLAND_SCANNER} client-header ${xml} ${gen}/${name}-client-protocol.h
      COMMAND ${CIVUE_WAYLAND_SCANNER} private-code ${xml} ${gen}/${name}-protocol.c
      DEPENDS ${xml}
      COMMENT "wayland-scanner ${name}")
    target_sources(civue PRIVATE ${gen}/${name}-client-protocol.h ${gen}/${name}-protocol.c)
  endforeach()
  target_include_directories(civue SYSTEM PRIVATE ${gen})
else()
  message(FATAL_ERROR "civue runs on macOS and Linux")
endif()
add_library(civue::civue ALIAS civue)
civue_embed_shaders(civue PREFIX civue SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui.metal
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui.vert ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui.frag)
target_include_directories(civue PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(civue PUBLIC c_std_23)
target_link_libraries(civue PUBLIC cilo::cilo)
target_compile_options(civue PRIVATE -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter
  -Wno-missing-field-initializers -Wno-sign-compare)
set_target_properties(civue PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(CIVUE_BUILD_TESTS)
  enable_testing()
  file(GLOB tests CONFIGURE_DEPENDS tests/*.c)
  foreach(source ${tests})
    get_filename_component(name ${source} NAME_WE)
    add_executable(test_${name} ${source})
    target_link_libraries(test_${name} PRIVATE civue)
    target_compile_options(test_${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND test_${name})
  endforeach()
endif()
