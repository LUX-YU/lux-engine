include_directories("${CMAKE_CURRENT_SOURCE_DIR}/../pinclude")

add_executable(render_graph_test Graph.cpp)

target_link_libraries(render_graph_test PRIVATE lux::engine::render::render_graph)

foreach(case IN ITEMS declarations hazards lifetimes legacy bindings reuse generated)
    add_test(NAME render.graph.${case} COMMAND render_graph_test ${case})
endforeach()

foreach(header IN ITEMS Definition Plan Bindings Authoring Schema Builder)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header_${header}.cpp")
    file(WRITE "${source}" "#include <lux/engine/render/graph/${header}.hpp>\n")
    add_library(render_graph_header_${header} OBJECT "${source}")
    target_link_libraries(render_graph_header_${header} PRIVATE lux::engine::render::render_graph)
endforeach()

add_executable(render_graph_probe_control Probe.cpp)

target_link_libraries(render_graph_probe_control PRIVATE lux::engine::render::render_graph)

add_test(NAME render.graph.probe_control COMMAND render_graph_probe_control)

foreach(probe IN ITEMS ID TEMPORARY VULKAN SCENE RUNTIME TRANSPORT LEGACY EDITOR FEATURE)
    add_executable(render_graph_reject_${probe} EXCLUDE_FROM_ALL Probe.cpp)
    target_compile_definitions(render_graph_reject_${probe} PRIVATE LUX_PROBE_${probe})
    target_link_libraries(render_graph_reject_${probe} PRIVATE lux::engine::render::render_graph)
    add_test(
        NAME render.graph.reject_${probe}
        COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DPROBE=${probe}"
            "-DMSVC_PROBE=${MSVC}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/RejectCompile.cmake"
    )
    set_tests_properties(render.graph.reject_${probe} PROPERTIES RUN_SERIAL TRUE)
endforeach()

add_executable(render_graph_benchmark Benchmark.cpp)

target_link_libraries(render_graph_benchmark PRIVATE lux::engine::render::render_graph)

add_test(NAME render.graph.binding_allocation COMMAND render_graph_benchmark)

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/RenderGraphCodegen.cmake")

find_program(LUX_GLSLC glslc HINTS "${Vulkan_INCLUDE_DIR}/../Bin" REQUIRED)

foreach(fixture IN ITEMS Tonemap Blur Composite Storage Hzb Complex LocalRead)
    add_executable(render_graph_schema_${fixture} authoring/Fixture.cpp)

    target_link_libraries(
        render_graph_schema_${fixture}
        PRIVATE lux::engine::render::render_graph lux::engine::toolchain::toolchain_shader_emitter
            toolchain_shader_reflection
    )

    target_include_directories(
        render_graph_schema_${fixture} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/type_info/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/authoring"
    )

    target_compile_definitions(
        render_graph_schema_${fixture} PRIVATE
        "LUX_FIXTURE_HEADER=\"${fixture}.pass.hpp\"" "LUX_FIXTURE_TYPE=${fixture}"
    )

    render_pass_schema(render_graph_schema_${fixture} "${CMAKE_CURRENT_SOURCE_DIR}/authoring/${fixture}.hpp")

    if(fixture MATCHES "^(Tonemap|Blur|Composite|LocalRead)$")
        set(stage fragment)
    else()
        set(stage compute)
    endif()

    set(shader_source "${CMAKE_CURRENT_SOURCE_DIR}/authoring/${fixture}.lglsl")
    set(shader_output "${CMAKE_CURRENT_BINARY_DIR}/generated/${fixture}")
    add_custom_command(
        OUTPUT "${shader_output}.spv" "${shader_output}.glsl"
        COMMAND render_graph_schema_${fixture} "${shader_source}" "${shader_output}.glsl"
        COMMAND "${LUX_GLSLC}" "-fshader-stage=${stage}" --target-env=vulkan1.2
            "${shader_output}.glsl" -o "${shader_output}.spv"
        DEPENDS render_graph_schema_${fixture} "${shader_source}"
        VERBATIM
    )

    add_custom_target(render_graph_shader_${fixture} ALL DEPENDS "${shader_output}.spv")

    add_test(
        NAME render.graph.schema_${fixture}
        COMMAND render_graph_schema_${fixture} "${shader_output}.spv" validate
    )
endforeach()

add_executable(render_graph_builder_test authoring/Builder.cpp)

add_dependencies(
    render_graph_builder_test render_graph_schema_Tonemap_pass_schema render_graph_schema_Hzb_pass_schema
    render_graph_schema_Storage_pass_schema
)

target_include_directories(
    render_graph_builder_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated" "${CMAKE_CURRENT_SOURCE_DIR}/authoring"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/type_info/include"
)

target_link_libraries(render_graph_builder_test PRIVATE lux::engine::render::render_graph)

add_test(NAME render.graph.authoring_ownership COMMAND render_graph_builder_test)

foreach(probe IN ITEMS TEXTURE_BUFFER UNMARKED MISSING FINISH_BORROW RAW_DEFINITION)
    add_executable(render_graph_author_reject_${probe} EXCLUDE_FROM_ALL authoring/Reject.cpp)

    add_dependencies(render_graph_author_reject_${probe} render_graph_schema_Tonemap_pass_schema)

    target_compile_definitions(render_graph_author_reject_${probe} PRIVATE LUX_${probe})

    target_include_directories(
        render_graph_author_reject_${probe} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
        "${CMAKE_CURRENT_SOURCE_DIR}/authoring" "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/type_info/include"
    )

    target_link_libraries(render_graph_author_reject_${probe} PRIVATE lux::engine::render::render_graph)

    if(probe STREQUAL TEXTURE_BUFFER)
        set(expected "GraphBuffer")
    elseif(probe STREQUAL UNMARKED)
        set(expected "F1 requires generated semantic Schema")
    elseif(probe STREQUAL MISSING)
        set(expected "F1 requires generated metadata include")
    elseif(probe STREQUAL FINISH_BORROW)
        set(expected "finish")
    else()
        set(expected "private member")
    endif()

    add_test(
        NAME render.graph.author_reject_${probe}
        COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DREJECT_TARGET=render_graph_author_reject_${probe}" "-DEXPECTED=${expected}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/authoring/Reject.cmake"
    )

    set_tests_properties(render.graph.author_reject_${probe} PROPERTIES RUN_SERIAL TRUE)
endforeach()

foreach(probe IN ITEMS Role Borrow Layout DuplicateRole Unknown)
    add_executable(render_graph_codegen_reject_${probe} EXCLUDE_FROM_ALL authoring/RejectCodegen.cpp)

    target_compile_definitions(render_graph_codegen_reject_${probe} PRIVATE "LUX_FIXTURE_HEADER=\"Bad${probe}.pass.hpp\"")

    target_include_directories(
        render_graph_codegen_reject_${probe} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/authoring" "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/type_info/include"
    )

    target_link_libraries(render_graph_codegen_reject_${probe} PRIVATE lux::engine::render::render_graph)

    render_pass_schema(render_graph_codegen_reject_${probe} "${CMAKE_CURRENT_SOURCE_DIR}/authoring/Bad${probe}.hpp")

    add_test(
        NAME render.graph.codegen_reject_${probe}
        COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DREJECT_TARGET=render_graph_codegen_reject_${probe}" "-DEXPECTED=PassSchema rejected:"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/authoring/Reject.cmake"
    )

    set_tests_properties(render.graph.codegen_reject_${probe} PROPERTIES RUN_SERIAL TRUE)
endforeach()

foreach(case IN ITEMS offset extra_scalar descriptor_count access extra_resource)
    if(case STREQUAL descriptor_count)
        set(fixture Composite)
    elseif(case STREQUAL access)
        set(fixture Complex)
    else()
        set(fixture Tonemap)
    endif()

    add_test(
        NAME render.graph.shader_reject_${case}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/authoring/RejectShader.py"
            "${LUX_GLSLC}" "$<TARGET_FILE:render_graph_schema_${fixture}>"
            "${CMAKE_CURRENT_BINARY_DIR}/generated/${fixture}.glsl" "${CMAKE_CURRENT_BINARY_DIR}/shader_rejections"
            "${case}"
    )
endforeach()

foreach(fixture IN ITEMS NonShader Optional Attachments Transfer)
    add_executable(render_graph_schema_${fixture} authoring/RejectCodegen.cpp)

    target_compile_definitions(render_graph_schema_${fixture} PRIVATE "LUX_FIXTURE_HEADER=\"${fixture}.pass.hpp\"")

    target_include_directories(
        render_graph_schema_${fixture} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/authoring"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/type_info/include"
    )

    target_link_libraries(render_graph_schema_${fixture} PRIVATE lux::engine::render::render_graph)

    render_pass_schema(render_graph_schema_${fixture} "${CMAKE_CURRENT_SOURCE_DIR}/authoring/${fixture}.hpp")
endforeach()

add_dependencies(
    render_graph_builder_test render_graph_schema_NonShader_pass_schema render_graph_schema_Optional_pass_schema
    render_graph_schema_Attachments_pass_schema render_graph_schema_Transfer_pass_schema
)

add_executable(render_graph_authoring_benchmark authoring/Benchmark.cpp)

add_dependencies(render_graph_authoring_benchmark render_graph_schema_Tonemap_pass_schema)

target_include_directories(
    render_graph_authoring_benchmark PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated" "${CMAKE_CURRENT_SOURCE_DIR}/authoring"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/type_info/include"
)

target_link_libraries(render_graph_authoring_benchmark PRIVATE lux::engine::render::render_graph)

add_test(NAME render.graph.emitter_contract COMMAND render_graph_schema_Tonemap unused emitter-contract)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/header_PassContract.cpp" "#include <lux/engine/description/PassContract.hpp>\n")

add_library(render_graph_header_PassContract OBJECT "${CMAKE_CURRENT_BINARY_DIR}/header_PassContract.cpp")

target_link_libraries(render_graph_header_PassContract PRIVATE lux::engine::render::render_graph)
