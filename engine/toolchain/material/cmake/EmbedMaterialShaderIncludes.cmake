if(NOT DEFINED OUTPUT OR NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "Embedded Shader includes require OUTPUT and SOURCE_ROOT")
endif()

set(resources
    "gbuffer_encode.glsl|${SOURCE_ROOT}/gbuffer_encode.glsl"
    "material_types.glsl|${SOURCE_ROOT}/material_types.glsl"
    "texture_sampling.glsl|${SOURCE_ROOT}/texture_sampling.glsl"
)

set(data_definitions "")
set(entries "")
set(resource_index 0)
foreach(resource IN LISTS resources)
    string(REPLACE "|" ";" fields "${resource}")
    list(GET fields 0 name)
    list(GET fields 1 path)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Embedded Shader include is missing: ${path}")
    endif()

    file(READ "${path}" content_hex HEX)
    string(LENGTH "${content_hex}" hex_length)
    math(EXPR byte_count "${hex_length} / 2")
    set(data_name "kShaderIncludeData${resource_index}")
    string(APPEND data_definitions "    inline constexpr unsigned char ${data_name}[] = {\n        ")
    set(offset 0)
    set(column 0)
    while(offset LESS hex_length)
        string(SUBSTRING "${content_hex}" ${offset} 2 byte)
        string(APPEND data_definitions "0x${byte}, ")
        math(EXPR offset "${offset} + 2")
        math(EXPR column "${column} + 1")
        if(column EQUAL 16 AND offset LESS hex_length)
            string(APPEND data_definitions "\n        ")
            set(column 0)
        endif()
    endwhile()
    string(APPEND data_definitions "\n    };\n\n")
    string(APPEND entries
        "        EmbeddedShaderInclude{\"${name}\", ${data_name}, ${byte_count}U},\n"
    )
    math(EXPR resource_index "${resource_index} + 1")
endforeach()

list(LENGTH resources resource_count)
set(generated "#pragma once\n\n")
string(APPEND generated "#include <array>\n#include <cstddef>\n#include <string_view>\n\n")
string(APPEND generated "namespace lux::material::compiler::generated\n{\n")
string(APPEND generated "    struct EmbeddedShaderInclude final\n    {\n")
string(APPEND generated
    "        std::string_view name;\n        const unsigned char* data;\n        std::size_t size;\n    };\n\n"
)
string(APPEND generated "${data_definitions}")
string(APPEND generated
    "    inline constexpr std::array<EmbeddedShaderInclude, ${resource_count}> kMaterialShaderIncludes{\n"
)
string(APPEND generated "${entries}    };\n} // namespace lux::material::compiler::generated\n")

get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${OUTPUT}.tmp" "${generated}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OUTPUT}.tmp" "${OUTPUT}"
    COMMAND_ERROR_IS_FATAL ANY
)
file(REMOVE "${OUTPUT}.tmp")
