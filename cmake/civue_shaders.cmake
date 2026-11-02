# civue_embed_shaders(<target> PREFIX <name> [INCLUDE <dir>...] SOURCES <file>...) compiles shaders for the gpu
# of the platform and embeds them in <target>. on macos each .metal file becomes <name>_<file>_metallib, a
# metallib, on linux each .vert, .frag or .comp file becomes <name>_<file>_<ext>, spir-v for vulkan 1.2, with a
# _len next to it. the other platform's files are skipped.
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
      add_custom_command(OUTPUT ${lib}
        COMMAND xcrun -sdk macosx metal -std=metal3.1 -mmacosx-version-min=${min} -ffast-math -fpreserve-invariance
          ${includes} -c ${shader} -o ${air}
        COMMAND xcrun -sdk macosx metallib ${air} -o ${lib}
        DEPENDS ${shader} ${depends}
        COMMENT "metal ${name}")
      list(APPEND outputs ${lib})
      string(APPEND source "const unsigned char ${arg_PREFIX}_${name}_metallib[] = {\n#embed \"${name}.metallib\"\n};\n"
        "const size_t ${arg_PREFIX}_${name}_metallib_len = sizeof ${arg_PREFIX}_${name}_metallib;\n")
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
      add_custom_command(OUTPUT ${spv}
        COMMAND ${CIVUE_GLSLC} --target-env=vulkan1.2 -O ${includes} ${shader} -o ${spv}
        DEPENDS ${shader} ${depends}
        COMMENT "glslc ${file}")
      list(APPEND outputs ${spv})
      string(APPEND source "alignas(4) const unsigned char ${arg_PREFIX}_${name}[] = {\n#embed \"${name}.spv\"\n};\n"
        "const size_t ${arg_PREFIX}_${name}_len = sizeof ${arg_PREFIX}_${name};\n")
    endforeach()
  endif()
  set(embedded ${gen}/embedded.c)
  file(GENERATE OUTPUT ${embedded} CONTENT "${source}")
  set_source_files_properties(${embedded} PROPERTIES GENERATED TRUE OBJECT_DEPENDS "${outputs}"
    COMPILE_OPTIONS "--embed-dir=${gen}")
  target_sources(${target} PRIVATE ${embedded})
endfunction()
