if(NOT DEFINED OUTPUT OR NOT DEFINED SOURCE_ROOT OR NOT DEFINED GENERATED_ROOT)
    message(FATAL_ERROR "Embedded Shader includes require OUTPUT, SOURCE_ROOT, and GENERATED_ROOT")
endif()

set(resources
    "cluster_common.glsl|${GENERATED_ROOT}/cluster_common.glsl"
    "lighting_common.glsl|${GENERATED_ROOT}/lighting_common.glsl"
    "shadow_common.glsl|${GENERATED_ROOT}/shadow_common.glsl"
    "view_pc_prefix.glsl|${GENERATED_ROOT}/view_pc_prefix.glsl"
    "brdf/brdf_common.glsl|${SOURCE_ROOT}/brdf/brdf_common.glsl"
    "brdf/brdf_ggx.glsl|${SOURCE_ROOT}/brdf/brdf_ggx.glsl"
    "brdf/brdf_toon.glsl|${SOURCE_ROOT}/brdf/brdf_toon.glsl"
    "gbuffer_encode.glsl|${SOURCE_ROOT}/gbuffer_encode.glsl"
    "light_types.glsl|${SOURCE_ROOT}/light_types.glsl"
    "lighting_tbn.glsl|${SOURCE_ROOT}/lighting_tbn.glsl"
    "material_types.glsl|${SOURCE_ROOT}/material_types.glsl"
    "shadow_evsm.glsl|${SOURCE_ROOT}/shadow_evsm.glsl"
    "shadow_pcf.glsl|${SOURCE_ROOT}/shadow_pcf.glsl"
    "texture_sampling.glsl|${SOURCE_ROOT}/texture_sampling.glsl"
    "transition_dither.glsl|${SOURCE_ROOT}/transition_dither.glsl"
    "view_common.glsl|${SOURCE_ROOT}/view_common.glsl"
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
