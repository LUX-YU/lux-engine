cmake_minimum_required(VERSION 3.28)

project(render_extension_consumer LANGUAGES CXX)

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_extensions)

find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution)

find_package(lux-engine-function CONFIG REQUIRED COMPONENTS material_compiler)

add_executable(render_extension Probe.cpp)
target_link_libraries(render_extension PRIVATE lux::engine::function::render_extensions lux::engine::core::core_execution)
target_compile_features(render_extension PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(render_extension PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR- /bigobj)
endif()

enable_testing()

find_program(LUX_PROBE_SHADER_COMPILER lux_shader_compile REQUIRED)

set(render_test_sources "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/render/test")
add_executable(installed_material_commands MaterialCommands.cpp)

target_include_directories(installed_material_commands PRIVATE "${render_test_sources}")

target_link_libraries(installed_material_commands PRIVATE
    lux::engine::function::render_extensions
    lux::engine::function::material_compiler
    lux::engine::core::core_execution
)

target_compile_features(installed_material_commands PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(installed_material_commands PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR- /bigobj)
endif()

add_test(NAME installed.render.material_commands_vulkan COMMAND installed_material_commands)
if(WIN32)
    add_test(NAME installed.render.material_commands_d3d12 COMMAND installed_material_commands d3d12)
endif()

set(compute_source "${CMAKE_CURRENT_LIST_DIR}/ComputeOutput.slang")
set(compute_header "${CMAKE_CURRENT_BINARY_DIR}/GraphFill.hpp")
add_custom_command(
    OUTPUT "${compute_header}"
    COMMAND "${LUX_PROBE_SHADER_COMPILER}" "${compute_source}" main compute bundle "${compute_header}"
        --symbol=kGraphFill
    DEPENDS "${compute_source}" "${LUX_PROBE_SHADER_COMPILER}"
    VERBATIM
)

add_executable(compute_output ComputeOutput.cpp "${compute_header}")

target_include_directories(compute_output PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${render_test_sources}")

target_link_libraries(compute_output PRIVATE lux::engine::function::render_extensions)

if(MSVC)
    target_compile_options(compute_output PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR-)
endif()

add_test(NAME installed.render.compute_output_vulkan COMMAND compute_output)

set(receipt_backends vulkan)

if(WIN32)
    list(APPEND receipt_backends d3d12)
endif()

foreach(backend IN LISTS receipt_backends)
    foreach(mode IN ITEMS receipts receipt-partial receipt-unknown)
        add_test(NAME installed.render.buffer_${mode}_${backend} COMMAND compute_output ${backend} ${mode})

        set_tests_properties(installed.render.buffer_${mode}_${backend} PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77)
    endforeach()
endforeach()

add_test(NAME installed.render.replica_unknown_vulkan COMMAND compute_output vulkan unknown)

set_tests_properties(installed.render.compute_output_vulkan installed.render.replica_unknown_vulkan PROPERTIES TIMEOUT 45)

if(WIN32)
    add_test(NAME installed.render.compute_output_d3d12 COMMAND compute_output d3d12)

    add_test(NAME installed.render.replica_unknown_d3d12 COMMAND compute_output d3d12 unknown)

    set_tests_properties(installed.render.compute_output_d3d12 installed.render.replica_unknown_d3d12 PROPERTIES TIMEOUT 45)
endif()

foreach(header IN ITEMS
    core/ConfigValue core/FinalBufferVersion core/RenderErrorSink
    effect/CapabilityDefinition effect/EffectBundles effect/EffectCatalog effect/EffectDiagnostic effect/EffectGraph
    extensions/CommandBinding extensions/CommandHandler extensions/CommandMethod extensions/Registration
    extensions/effect/EffectBuildContext extensions/effect/EffectUniformBlock
    extensions/graph/RenderGraphBuilder extensions/graph/Work extensions/graph/PassValues extensions/graph/Diagnostics
    extensions/execution/GraphPreparation extensions/execution/ProducerWorkspace
    extensions/execution/ProducerFactory extensions/execution/LinkedProducer extensions/execution/RasterStream
    extensions/execution/PreparedCpuValue
    extensions/resource/ReplicaMutation extensions/resource/ResourceCreateContext
)
    get_filename_component(name "${header}" NAME)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/${name}Header.cpp")
    file(GENERATE OUTPUT "${source}" CONTENT "#include <lux/engine/render/${header}.hpp>\n")
    add_library(header_${name} OBJECT "${source}")
    target_link_libraries(header_${name} PRIVATE lux::engine::function::render_extensions)
    target_compile_features(header_${name} PRIVATE cxx_std_20)
endforeach()

option(LUX_TEST_INSTALLED_EDITOR_UI "Run the Editor UI output probe against installed packages" OFF)

if(LUX_TEST_INSTALLED_EDITOR_UI)
    find_package(lux-editor-ui-render CONFIG REQUIRED)

    set(repo "${CMAKE_CURRENT_LIST_DIR}/../../..")
    add_executable(editor_ui_frame_packet "${repo}/editor/ui/render/test/FramePacket.cpp")

    target_include_directories(editor_ui_frame_packet PRIVATE "${repo}/modules/function/render/test")

    target_link_libraries(editor_ui_frame_packet PRIVATE lux::editor::ui_render lux::engine::core::core_execution)

    target_compile_features(editor_ui_frame_packet PRIVATE cxx_std_20)

    if(MSVC)
        target_compile_options(editor_ui_frame_packet PRIVATE /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /GR-)
    endif()

    add_test(NAME installed.editor.ui_vulkan COMMAND editor_ui_frame_packet)

    if(WIN32)
        add_test(NAME installed.editor.ui_d3d12 COMMAND editor_ui_frame_packet d3d12)
    endif()
endif()

add_executable(effect_build EffectBuild.cpp)

target_link_libraries(effect_build PRIVATE
    lux::engine::function::render_extensions
    lux::engine::core::core_execution
)

target_compile_features(effect_build PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(effect_build PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR- /bigobj)
endif()

add_test(NAME installed.render.effect_build_vulkan COMMAND effect_build)
add_test(NAME installed.render.effect_build_d3d12 COMMAND effect_build d3d12)

set_tests_properties(installed.render.effect_build_vulkan installed.render.effect_build_d3d12 PROPERTIES TIMEOUT 45)

add_test(NAME installed.render.extension COMMAND render_extension)
set_tests_properties(installed.render.extension PROPERTIES TIMEOUT 45)

if(WIN32)
    add_test(NAME installed.render.extension_d3d12 COMMAND render_extension d3d12)
    set_tests_properties(installed.render.extension_d3d12 PROPERTIES TIMEOUT 45)
endif()

add_executable(frame_columns FrameColumns.cpp)

target_link_libraries(frame_columns PRIVATE lux::engine::function::render)

target_compile_features(frame_columns PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(frame_columns PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR-)
endif()

add_test(NAME installed.render.frame_columns COMMAND frame_columns)

foreach(case IN ITEMS reset move assign destroy missing-read)
    add_test(NAME installed.render.frame_columns_${case}
        COMMAND ${CMAKE_COMMAND}
            -DPROBE=$<TARGET_FILE:frame_columns>
            -DCASE=${case}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckFrameRejection.cmake"
    )
endforeach()
