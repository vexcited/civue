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

set(CIVUE_SOURCES src/base.c src/draw.c)
set(CIVUE_DARWIN_SOURCES src/darwin/gfx.c src/darwin/gpu.c src/darwin/objc.c)
set(CIVUE_LINUX_SOURCES src/linux/gfx.c src/linux/vk.c)

if(APPLE)
  enable_language(OBJC)
  add_library(civue STATIC ${CIVUE_SOURCES} ${CIVUE_DARWIN_SOURCES})
  target_compile_options(civue PRIVATE -fblocks)
  target_link_libraries(civue PUBLIC "-framework Foundation" "-framework AppKit" "-framework Metal"
    "-framework QuartzCore" "-framework CoreText" "-framework CoreGraphics" "-framework ImageIO"
    "-framework CoreFoundation" objc)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(civue STATIC ${CIVUE_SOURCES} ${CIVUE_LINUX_SOURCES})
  target_compile_definitions(civue PRIVATE _GNU_SOURCE)
  find_package(Threads REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(CIVUE_DEPS REQUIRED IMPORTED_TARGET vulkan)
  target_link_libraries(civue PUBLIC PkgConfig::CIVUE_DEPS Threads::Threads m)
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
