find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Performance qualification compares finalized links. Incremental-link thunks
# and padding depend on linker history even when all measured objects match.
if(MSVC)
    target_link_options(render_vulkan_shader_benchmark PRIVATE /INCREMENTAL:NO)
endif()

set(f4c_test "${CMAKE_CURRENT_LIST_DIR}/../../modules/function/render/vulkan/test")
set(f4c_shader "${CMAKE_BINARY_DIR}/render_vulkan/test/shader")

add_executable(render_vulkan_schema_F4CWrite "${f4c_test}/../../graph/test/authoring/Fixture.cpp")

target_link_libraries(render_vulkan_schema_F4CWrite PRIVATE
    render_graph toolchain_shader_emitter toolchain_shader_reflection
)

target_include_directories(render_vulkan_schema_F4CWrite PRIVATE
    "${f4c_test}/shader" "${f4c_test}/../../../../core/type_info/include"
)

target_compile_definitions(render_vulkan_schema_F4CWrite PRIVATE
    "LUX_FIXTURE_HEADER=\"F4CWrite.pass.hpp\"" "LUX_FIXTURE_TYPE=F4CWrite"
)

render_pass_schema(render_vulkan_schema_F4CWrite "${f4c_test}/shader/F4CWrite.hpp")

add_custom_command(
    OUTPUT "${f4c_shader}/F4CWrite.comp.glsl" "${f4c_shader}/F4CWrite.comp.spv"
    COMMAND render_vulkan_schema_F4CWrite "${f4c_test}/shader/F4CWrite.comp.lglsl" "${f4c_shader}/F4CWrite.comp.glsl"
    COMMAND "${Vulkan_GLSLC_EXECUTABLE}" -fshader-stage=comp --target-env=vulkan1.3
        "${f4c_shader}/F4CWrite.comp.glsl" -o "${f4c_shader}/F4CWrite.comp.spv"
    COMMAND "${LUX_SPIRV_VAL}" --target-env vulkan1.3 "${f4c_shader}/F4CWrite.comp.spv"
    COMMAND render_vulkan_schema_F4CWrite "${f4c_shader}/F4CWrite.comp.spv" validate
    DEPENDS render_vulkan_schema_F4CWrite "${f4c_test}/shader/F4CWrite.comp.lglsl"
    VERBATIM
)

add_custom_target(render_vulkan_spv_F4CWrite ALL DEPENDS "${f4c_shader}/F4CWrite.comp.spv")

add_executable(render_vulkan_graph_closeout "${f4c_test}/NativeGraphCloseout.cpp" "${f4c_test}/NativeGraphSupport.cpp")

target_link_libraries(render_vulkan_graph_closeout PRIVATE render_vulkan_shader_compiler)

target_include_directories(render_vulkan_graph_closeout PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated" "${f4c_shader}/generated" "${f4c_test}/shader"
    "${f4c_test}/../../../../core/type_info/include"
)

add_dependencies(render_vulkan_graph_closeout render_vulkan_spv_F4CWrite
    render_vulkan_schema_F4Readback render_vulkan_schema_F4CopyBuffer render_vulkan_schema_F4CopyUpload
    render_vulkan_schema_F4HostRead render_vulkan_spv_F3Tonemap_frag render_vulkan_spv_F3Tonemap_vert
)

foreach(mode IN ITEMS C1 C1-multi C1-partial C2 C3)
    add_test(NAME render.vulkan.closeout_${mode} COMMAND render_vulkan_graph_closeout "${mode}" "${f4c_shader}")
    set_tests_properties(render.vulkan.closeout_${mode} PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
endforeach()

foreach(fixture IN ITEMS F4CVertex F4CIndexed F4CIndirect F4CIndexedIndirect F4CArguments)
    add_executable(render_vulkan_schema_${fixture} "${f4c_test}/../../graph/test/authoring/Fixture.cpp")

    target_link_libraries(render_vulkan_schema_${fixture} PRIVATE
        render_graph toolchain_shader_emitter toolchain_shader_reflection
    )

    target_include_directories(render_vulkan_schema_${fixture} PRIVATE
        "${f4c_test}/shader" "${f4c_test}/../../../../core/type_info/include"
    )

    target_compile_definitions(render_vulkan_schema_${fixture} PRIVATE
        "LUX_FIXTURE_HEADER=\"${fixture}.pass.hpp\"" "LUX_FIXTURE_TYPE=${fixture}"
    )

    render_pass_schema(render_vulkan_schema_${fixture} "${f4c_test}/shader/${fixture}.hpp")

    if(fixture STREQUAL "F4CArguments")
        set(stages comp)
    else()
        set(stages vert frag)
    endif()

    foreach(stage IN LISTS stages)
        add_custom_command(
            OUTPUT "${f4c_shader}/${fixture}.${stage}.glsl" "${f4c_shader}/${fixture}.${stage}.spv"
            COMMAND render_vulkan_schema_${fixture}
                "${f4c_test}/shader/${fixture}.${stage}.lglsl" "${f4c_shader}/${fixture}.${stage}.glsl"
            COMMAND "${Vulkan_GLSLC_EXECUTABLE}" -fshader-stage=${stage} --target-env=vulkan1.3
                "${f4c_shader}/${fixture}.${stage}.glsl" -o "${f4c_shader}/${fixture}.${stage}.spv"
            COMMAND "${LUX_SPIRV_VAL}" --target-env vulkan1.3 "${f4c_shader}/${fixture}.${stage}.spv"
            COMMAND render_vulkan_schema_${fixture} "${f4c_shader}/${fixture}.${stage}.spv" validate
            DEPENDS render_vulkan_schema_${fixture} "${f4c_test}/shader/${fixture}.${stage}.lglsl"
            VERBATIM
        )

        add_custom_target(render_vulkan_spv_${fixture}_${stage} ALL
            DEPENDS "${f4c_shader}/${fixture}.${stage}.spv"
        )
    endforeach()
endforeach()

add_executable(render_vulkan_graph_draw "${f4c_test}/NativeGraphDraw.cpp" "${f4c_test}/NativeGraphSupport.cpp")

target_link_libraries(render_vulkan_graph_draw PRIVATE render_vulkan_shader_compiler)

target_include_directories(render_vulkan_graph_draw PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated" "${f4c_shader}/generated" "${f4c_test}/shader"
    "${f4c_test}/../../../../core/type_info/include"
)

add_dependencies(render_vulkan_graph_draw render_vulkan_schema_F4Readback render_vulkan_schema_F4CopyBuffer
    render_vulkan_spv_F4CArguments_comp
)

foreach(fixture IN ITEMS F4CVertex F4CIndexed F4CIndirect F4CIndexedIndirect)
    add_dependencies(render_vulkan_graph_draw render_vulkan_spv_${fixture}_vert render_vulkan_spv_${fixture}_frag)
endforeach()

foreach(mode IN ITEMS vertex indexed indirect indexed-indirect)
    add_test(NAME render.vulkan.closeout_draw_${mode} COMMAND render_vulkan_graph_draw "${mode}" "${f4c_shader}")
    set_tests_properties(render.vulkan.closeout_draw_${mode} PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)
endforeach()
