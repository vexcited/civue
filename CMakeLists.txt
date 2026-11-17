cmake_minimum_required(VERSION 3.29)

if(CMAKE_SCRIPT_MODE_FILE)
  include(${CIVUE_ICONS_CONFIG})

  # writes a file only when it changes, so nothing rebuilds for nothing.
  function(civue__write path content)
    if(EXISTS ${path})
      file(READ ${path} was)
      if(was STREQUAL content)
        return()
      endif()
    endif()
    file(WRITE ${path} "${content}")
  endfunction()

  set(space "[ \t\r\n]*")
  set(word "[A-Za-z0-9_]+")
  set(names ${extra})
  foreach(source ${sources})
    file(READ ${source} code)
    # literals and comments are read whole, whichever starts first: a comment
    # goes and a literal is emptied, so that neither hides an icon nor names
    # one.
    set(text "\"([^\"\\\n]|\\\\.)*\"")
    set(char "'([^'\\\n]|\\\\.)*'")
    set(comment "/\\*([^*]|\\*+[^*/])*\\*+/|//[^\n]*")
    set(rest "${code}")
    set(code "")
    while(rest MATCHES "${text}|${char}|${comment}")
      set(found "${CMAKE_MATCH_0}")
      string(FIND "${rest}" "${found}" at)
      string(LENGTH "${found}" length)
      string(SUBSTRING "${rest}" 0 ${at} before)
      math(EXPR after "${at} + ${length}")
      string(SUBSTRING "${rest}" ${after} -1 rest)
      string(APPEND code "${before}")
      if(found MATCHES "^/")
        string(APPEND code " ")
      else()
        string(APPEND code "\"\"")
      endif()
    endwhile()
    string(APPEND code "${rest}")
    string(REGEX MATCHALL
      "(^|[^A-Za-z0-9_])(icon|CIVUE_ICON)${space}\\(${space}${word}${space},${space}${word}"
      uses "${code}")
    foreach(use ${uses})
      string(REGEX REPLACE ".*\\(${space}(${word})${space},${space}(${word})$"
        "\\1:\\2" name "${use}")
      string(REPLACE "_" "-" name "${name}")
      list(APPEND names ${name})
    endforeach()
  endforeach()
  list(REMOVE_DUPLICATES names)
  list(SORT names)

  set(defines "")
  set(arrays "")
  set(entries "")
  set(count 0)
  foreach(name ${names})
    string(REPLACE ":" ";" parts "${name}")
    list(GET parts 0 set)
    list(GET parts 1 icon)

    set(dir "")
    foreach(pair ${collections})
      string(FIND "${pair}" "=" eq)
      string(SUBSTRING "${pair}" 0 ${eq} owner)
      if(owner STREQUAL set)
        math(EXPR from "${eq} + 1")
        string(SUBSTRING "${pair}" ${from} -1 dir)
      endif()
    endforeach()

    if(dir)
      if(NOT EXISTS ${dir}/${icon}.svg)
        message(FATAL_ERROR "civue: no ${icon}.svg in ${dir} for ${name}")
      endif()
      file(READ ${dir}/${icon}.svg svg)
    else()
      set(cached ${cache}/${set}/${icon}.svg)
      if(NOT EXISTS ${cached})
        # one build at a time fetches into a set, another may have fetched
        # this icon meanwhile.
        file(MAKE_DIRECTORY ${cache}/${set})
        file(LOCK ${cache}/${set} DIRECTORY)
        if(NOT EXISTS ${cached})
          file(DOWNLOAD ${api}/${set}/${icon}.svg ${cached}.part
            STATUS status TIMEOUT 30 TLS_VERIFY ON)
          list(GET status 0 code)
          list(GET status 1 why)
          if(NOT code EQUAL 0)
            file(REMOVE ${cached}.part)
            if(code EQUAL 22)
              message(FATAL_ERROR "civue: iconify has no icon ${name}")
            endif()
            message(FATAL_ERROR "civue: fetching ${name} failed: ${why}")
          endif()
          file(RENAME ${cached}.part ${cached})
        endif()
        file(LOCK ${cache}/${set} DIRECTORY RELEASE)
      endif()
      file(READ ${cached} svg)
    endif()

    # the svg alone, without the size iconify gives it in ems: it scales to
    # what it is drawn in.
    string(FIND "${svg}" "<svg" start)
    if(start LESS 0)
      message(FATAL_ERROR "civue: ${name} is no svg")
    endif()
    string(SUBSTRING "${svg}" ${start} -1 svg)
    string(FIND "${svg}" ">" close)
    string(SUBSTRING "${svg}" 0 ${close} root)
    string(SUBSTRING "${svg}" ${close} -1 rest)
    string(REGEX REPLACE "[ \t\r\n](width|height)=\"[^\"]*\"" "" root "${root}")
    set(svg "${root}${rest}")

    # an icon painted in currentColor alone is tinted, any other paint keeps
    # its own colors.
    set(color false)
    string(REGEX MATCHALL
      "(fill|stroke|stop-color)[ \t]*[=:][ \t]*[\"']?[^\"';>]*"
      paints "${svg}")
    foreach(paint ${paints})
      string(REGEX REPLACE "^[a-z-]+[ \t]*[=:][ \t]*[\"']?" "" value
        "${paint}")
      string(STRIP "${value}" value)
      string(TOLOWER "${value}" value)
      if(NOT value MATCHES "^(currentcolor|none|transparent|inherit)?$")
        set(color true)
      endif()
    endforeach()

    civue__write(${out}/svg/${set}/${icon}.svg "${svg}")
    string(REPLACE "-" "_" cset "${set}")
    string(REPLACE "-" "_" cicon "${icon}")
    string(APPEND defines
      "#define CIVUE_ICON_${cset}__${cicon} ((civue_icon)${count})\n")
    string(APPEND arrays "static const char icon_${count}[] = {\n"
      "#embed \"svg/${set}/${icon}.svg\"\n};\n")
    string(APPEND entries "  {icon_${count}, sizeof icon_${count}, ${color}},\n")
    math(EXPR count "${count} + 1")
  endforeach()

  string(CONCAT header "#ifndef CIVUE_ICONS_GENERATED_H\n"
    "#define CIVUE_ICONS_GENERATED_H\n\n${defines}\n#endif\n")
  set(source "#include <civue/icons.h>\n\ntypedef int civue__no_icons;\n")
  if(count)
    string(CONCAT source "#include <civue/icons.h>\n\n${arrays}\n"
      "static const civue_icon_svg icons[] = {\n${entries}};\n\n"
      "[[gnu::constructor]] static void preset_icons(void) {\n"
      "  civue_icons_preset(icons, ${count});\n}\n")
  endif()
  civue__write(${out}/civue_icons.h "${header}")
  civue__write(${out}/civue_icons.c "${source}")
  return()
endif()
project(civue VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ${PROJECT_IS_TOP_LEVEL})

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CONFIGURATION_TYPES
  AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
endif()

option(CIVUE_BUILD_EXAMPLES "build the civue examples"
  ${PROJECT_IS_TOP_LEVEL})

# a parent project may already provide cilo, otherwise it is fetched.
include(FetchContent)
if(NOT TARGET cilo::cilo)
  FetchContent_Declare(cilo
    GIT_REPOSITORY https://code.vexcited.com/ciko/cilo.git
    GIT_TAG main)
  FetchContent_MakeAvailable(cilo)
endif()

# civue_embed_shaders(<target> PREFIX <name> [INCLUDE <dir>...]
#   SOURCES <file>...) compiles shaders for the gpu of the platform and
# embeds them in <target>. on macos each .metal file becomes
# <name>_<file>_metallib, on linux each .vert, .frag or .comp file becomes
# <name>_<file>_<ext>, spir-v for vulkan 1.2, with a _len next to it. the
# other platform's files are skipped.
function(civue_embed_shaders target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "PREFIX" "INCLUDE;SOURCES")
  if(NOT arg_PREFIX)
    message(FATAL_ERROR "civue_embed_shaders: PREFIX is required")
  endif()

  set(gen ${CMAKE_CURRENT_BINARY_DIR}/${arg_PREFIX}_shaders)
  file(MAKE_DIRECTORY ${gen})

  set(includes)
  set(depends)
  foreach(dir ${arg_INCLUDE})
    list(APPEND includes -I ${dir})
    file(GLOB headers CONFIGURE_DEPENDS ${dir}/*.h ${dir}/*.glsl)
    list(APPEND depends ${headers})
  endforeach()

  set(outputs)
  set(source "#include <stddef.h>\n")

  if(APPLE)
    set(min ${CMAKE_OSX_DEPLOYMENT_TARGET})
    if(NOT min)
      set(min 14.0)
    endif()

    foreach(shader ${arg_SOURCES})
      if(NOT shader MATCHES "\\.metal$")
        continue()
      endif()

      get_filename_component(name ${shader} NAME_WE)
      set(air ${gen}/${name}.air)
      set(lib ${gen}/${name}.metallib)
      set(var ${arg_PREFIX}_${name}_metallib)

      add_custom_command(OUTPUT ${lib}
        COMMAND xcrun -sdk macosx metal -std=metal3.1
          -mmacosx-version-min=${min} -ffast-math -fpreserve-invariance
          ${includes} -c ${shader} -o ${air}
        COMMAND xcrun -sdk macosx metallib ${air} -o ${lib}
        DEPENDS ${shader} ${depends}
        COMMENT "metal ${name}")

      list(APPEND outputs ${lib})
      string(APPEND source
        "const unsigned char ${var}[] = {\n"
        "#embed \"${name}.metallib\"\n};\n"
        "const size_t ${var}_len = sizeof ${var};\n")
    endforeach()
  else()
    find_program(CIVUE_GLSLC glslc REQUIRED)

    foreach(shader ${arg_SOURCES})
      if(NOT shader MATCHES "\\.(vert|frag|comp)$")
        continue()
      endif()

      get_filename_component(file ${shader} NAME)
      string(REPLACE "." "_" name ${file})
      set(spv ${gen}/${name}.spv)
      set(var ${arg_PREFIX}_${name})

      add_custom_command(OUTPUT ${spv}
        COMMAND ${CIVUE_GLSLC} --target-env=vulkan1.2 -O ${includes}
          ${shader} -o ${spv}
        DEPENDS ${shader} ${depends}
        COMMENT "glslc ${file}")

      list(APPEND outputs ${spv})
      string(APPEND source
        "alignas(4) const unsigned char ${var}[] = {\n"
        "#embed \"${name}.spv\"\n};\n"
        "const size_t ${var}_len = sizeof ${var};\n")
    endforeach()
  endif()

  set(embedded ${gen}/embedded.c)
  file(GENERATE OUTPUT ${embedded} CONTENT "${source}")
  set_source_files_properties(${embedded} PROPERTIES
    GENERATED TRUE
    OBJECT_DEPENDS "${outputs}"
    COMPILE_OPTIONS "--embed-dir=${gen}")
  target_sources(${target} PRIVATE ${embedded})
endfunction()

# civue_icons(<target> [ICONS <set:name>...] [COLLECTIONS <set>=<dir>...]
#   [CACHE <dir>] [API <url>]) gives <target> the iconify icons its sources
# name with icon(set, name) or CIVUE_ICON(set, name), and the ones in ICONS,
# like unplugin-icons does. each is fetched once from iconify's api into
# CACHE, ~/.cache/civue/icons unless given, or read as <dir>/<name>.svg for
# a set of COLLECTIONS, then embedded in <target> as it builds. call it once
# the target has its sources.
function(civue_icons target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "CACHE;API" "ICONS;COLLECTIONS")
  set(gen ${CMAKE_CURRENT_BINARY_DIR}/${target}_icons)

  if(NOT arg_CACHE AND DEFINED ENV{XDG_CACHE_HOME})
    set(arg_CACHE $ENV{XDG_CACHE_HOME}/civue/icons)
  elseif(NOT arg_CACHE AND DEFINED ENV{HOME})
    set(arg_CACHE $ENV{HOME}/.cache/civue/icons)
  elseif(NOT arg_CACHE)
    set(arg_CACHE ${CMAKE_BINARY_DIR}/civue-icons)
  endif()
  cmake_path(ABSOLUTE_PATH arg_CACHE)
  if(NOT arg_API)
    set(arg_API https://api.iconify.design)
  endif()

  get_target_property(sources ${target} SOURCES)
  get_target_property(base ${target} SOURCE_DIR)
  set(files)
  foreach(source ${sources})
    if(source MATCHES "\\$<")
      continue()
    endif()
    cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY ${base}
      OUTPUT_VARIABLE path)
    if(EXISTS ${path})
      list(APPEND files ${path})
    endif()
  endforeach()

  set(collections)
  set(svgs)
  foreach(pair ${arg_COLLECTIONS})
    string(REGEX REPLACE "=.*" "" set "${pair}")
    string(REGEX REPLACE "^[^=]*=" "" dir "${pair}")
    cmake_path(ABSOLUTE_PATH dir)
    list(APPEND collections "${set}=${dir}")
    file(GLOB found CONFIGURE_DEPENDS ${dir}/*.svg)
    list(APPEND svgs ${found})
  endforeach()

  file(CONFIGURE OUTPUT ${gen}/config.cmake @ONLY CONTENT
    "set(sources [==[${files}]==])\nset(extra [==[${arg_ICONS}]==])\n\
set(collections [==[${collections}]==])\nset(cache [==[${arg_CACHE}]==])\n\
set(api [==[${arg_API}]==])\nset(out [==[${gen}]==])\n")

  add_custom_command(
    OUTPUT ${gen}/civue_icons.stamp
    BYPRODUCTS ${gen}/civue_icons.h ${gen}/civue_icons.c
    COMMAND ${CMAKE_COMMAND} -DCIVUE_ICONS_CONFIG=${gen}/config.cmake
      -P ${CMAKE_CURRENT_FUNCTION_LIST_FILE}
    COMMAND ${CMAKE_COMMAND} -E touch ${gen}/civue_icons.stamp
    DEPENDS ${files} ${svgs} ${gen}/config.cmake
      ${CMAKE_CURRENT_FUNCTION_LIST_FILE}
    COMMENT "civue: the icons of ${target}"
    VERBATIM)
  target_sources(${target} PRIVATE ${gen}/civue_icons.c
    ${gen}/civue_icons.stamp)
  target_include_directories(${target} PRIVATE ${gen})
endfunction()

set(CIVUE_SOURCES
  src/base.c src/draw.c src/field.c src/icons.c src/images.c src/layout.c
  src/numfield.c src/raster.c
  src/richedit.c src/richtext.c src/ui.c src/widgets.c)

set(CIVUE_DARWIN_SOURCES
  src/darwin/clipboard.m src/darwin/cursors.m src/darwin/filepanel.m
  src/darwin/gfx.m src/darwin/gpu.m src/darwin/image.m
  src/darwin/pasteboard.m
  src/darwin/spell.m src/darwin/svg.m src/darwin/text.c
  src/darwin/webview.m src/darwin/window.m)

set(CIVUE_LINUX_SOURCES
  src/linux/clipboard.c src/linux/cursors.c src/linux/filepanel.c
  src/linux/gfx.c src/linux/image.c src/linux/pasteboard.c src/linux/spell.c
  src/linux/svg.c src/linux/text.c src/linux/vk.c src/linux/webview.c
  src/linux/window.c)

if(APPLE)
  enable_language(OBJC)
  set(CMAKE_OBJC_STANDARD 23)
  set(CMAKE_OBJC_STANDARD_REQUIRED ON)
  set(CMAKE_OBJC_EXTENSIONS ON)

  add_library(civue STATIC ${CIVUE_SOURCES} ${CIVUE_DARWIN_SOURCES})
  target_compile_options(civue PRIVATE
    $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
  target_link_libraries(civue PUBLIC
    "-framework Foundation" "-framework AppKit" "-framework Metal"
    "-framework QuartzCore" "-framework CoreText" "-framework IOSurface"
    "-framework CoreGraphics" "-framework ImageIO"
    "-framework CoreFoundation" "-framework UniformTypeIdentifiers"
    "-framework WebKit" objc)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(civue STATIC ${CIVUE_SOURCES} ${CIVUE_LINUX_SOURCES})
  target_compile_definitions(civue PRIVATE _GNU_SOURCE)

  find_package(Threads REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(CIVUE_DEPS REQUIRED IMPORTED_TARGET
    vulkan freetype2 harfbuzz fontconfig fribidi librsvg-2.0 gdk-pixbuf-2.0 cairo
    gio-2.0 wayland-client wayland-cursor xkbcommon enchant-2)
  target_link_libraries(civue PUBLIC
    PkgConfig::CIVUE_DEPS Threads::Threads m)

  # the wayland protocols past the core one, their client code generated.
  pkg_get_variable(CIVUE_SCANNER_PATH wayland-scanner wayland_scanner)
  find_program(CIVUE_WAYLAND_SCANNER NAMES wayland-scanner
    HINTS ${CIVUE_SCANNER_PATH} REQUIRED)
  file(GLOB protocols CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/*.xml)

  set(gen ${CMAKE_CURRENT_BINARY_DIR}/wayland)
  file(MAKE_DIRECTORY ${gen})

  foreach(xml ${protocols})
    get_filename_component(name ${xml} NAME_WE)
    set(header ${gen}/${name}-client-protocol.h)
    set(code ${gen}/${name}-protocol.c)

    add_custom_command(OUTPUT ${header} ${code}
      COMMAND ${CIVUE_WAYLAND_SCANNER} client-header ${xml} ${header}
      COMMAND ${CIVUE_WAYLAND_SCANNER} private-code ${xml} ${code}
      DEPENDS ${xml}
      COMMENT "wayland-scanner ${name}")

    target_sources(civue PRIVATE ${header} ${code})
  endforeach()

  target_include_directories(civue SYSTEM PRIVATE ${gen})
else()
  message(FATAL_ERROR "civue runs on macOS and Linux")
endif()

add_library(civue::civue ALIAS civue)

civue_embed_shaders(civue PREFIX civue SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui.metal
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui.vert
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui.frag)

target_include_directories(civue PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(civue PUBLIC c_std_23)
target_link_libraries(civue PUBLIC cilo::cilo)
target_compile_options(civue PRIVATE
  -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter
  -Wno-missing-field-initializers -Wno-sign-compare)
set_target_properties(civue PROPERTIES POSITION_INDEPENDENT_CODE ON)

# each example is a project of its own, built here against this civue.
if(CIVUE_BUILD_EXAMPLES)
  file(GLOB examples LIST_DIRECTORIES true CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/*)
  foreach(example ${examples})
    if(EXISTS ${example}/CMakeLists.txt)
      add_subdirectory(${example})
    endif()
  endforeach()
endif()
