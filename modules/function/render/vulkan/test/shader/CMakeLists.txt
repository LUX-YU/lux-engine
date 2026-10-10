find_package(Python3 REQUIRED COMPONENTS Interpreter)

foreach(fixture IN ITEMS F3Tonemap F3Blur F3Composite F3Storage F3SharedA F3SharedB F3Raster)
    add_executable(render_vulkan_schema_${fixture} "${CMAKE_CURRENT_SOURCE_DIR}/../../../graph/test/authoring/Fixture.cpp")

    target_link_libraries(
        render_vulkan_schema_${fixture}
        PRIVATE render_graph toolchain_shader_emitter toolchain_shader_reflection
    )

    target_include_directories(
        render_vulkan_schema_${fixture} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core/type_info/include"
    )

    target_compile_definitions(
        render_vulkan_schema_${fixture} PRIVATE
            "LUX_FIXTURE_HEADER=\"${fixture}.pass.hpp\"" "LUX_FIXTURE_TYPE=${fixture}"
    )

    render_pass_schema(render_vulkan_schema_${fixture} "${CMAKE_CURRENT_SOURCE_DIR}/${fixture}.hpp")

    if(fixture STREQUAL F3Storage)
        set(stages comp)
    else()
        set(stages vert frag)
    endif()

    foreach(stage IN LISTS stages)
        if(stage STREQUAL vert AND NOT fixture MATCHES "^F3(Shared|Raster)")
            set(source "${CMAKE_CURRENT_SOURCE_DIR}/Fullscreen.vert.lglsl")
        else()
            set(source "${CMAKE_CURRENT_SOURCE_DIR}/${fixture}.${stage}.lglsl")
        endif()

        set(output "${CMAKE_CURRENT_BINARY_DIR}/${fixture}.${stage}")
        add_custom_command(
            OUTPUT "${output}.glsl" "${output}.spv"
            COMMAND render_vulkan_schema_${fixture} "${source}" "${output}.glsl"
            COMMAND "${Vulkan_GLSLC_EXECUTABLE}" "-fshader-stage=${stage}" --target-env=vulkan1.3
                "${output}.glsl" -o "${output}.spv"
            COMMAND "${LUX_SPIRV_VAL}" --target-env vulkan1.3 "${output}.spv"
            COMMAND render_vulkan_schema_${fixture} "${output}.spv" validate
            DEPENDS render_vulkan_schema_${fixture} "${source}"
            VERBATIM
        )

        add_custom_target(render_vulkan_spv_${fixture}_${stage} ALL DEPENDS "${output}.spv")
    endforeach()
endforeach()

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/F3Composite.additive.spv"
    COMMAND "${Vulkan_GLSLC_EXECUTABLE}" -fshader-stage=frag --target-env=vulkan1.3 -DF3_ADDITIVE=1
        "${CMAKE_CURRENT_BINARY_DIR}/F3Composite.frag.glsl" -o "${CMAKE_CURRENT_BINARY_DIR}/F3Composite.additive.spv"
    COMMAND "${LUX_SPIRV_VAL}" --target-env vulkan1.3 "${CMAKE_CURRENT_BINARY_DIR}/F3Composite.additive.spv"
    COMMAND render_vulkan_schema_F3Composite "${CMAKE_CURRENT_BINARY_DIR}/F3Composite.additive.spv" validate
    DEPENDS render_vulkan_spv_F3Composite_frag "${CMAKE_CURRENT_BINARY_DIR}/F3Composite.frag.glsl"
    VERBATIM
)

add_custom_target(render_vulkan_spv_additive ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/F3Composite.additive.spv")
