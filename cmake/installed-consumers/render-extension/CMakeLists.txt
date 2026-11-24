cmake_minimum_required(VERSION 3.28)

project(render_extension_consumer LANGUAGES CXX)

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_extensions)

find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution)

find_package(lux-engine-function CONFIG REQUIRED COMPONENTS material_compiler)

find_package(lux-engine-resource CONFIG REQUIRED COMPONENTS description)

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
    lux::engine::resource::description
    lux::engine::function::render_extensions
    lux::engine::function::material_compiler
    lux::engine::core::core_execution
)

target_compile_features(installed_material_commands PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(installed_material_commands PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR- /bigobj)
endif()

add_test(NAME installed.render.material_commands_vulkan COMMAND installed_material_commands)

set(compute_source "${CMAKE_CURRENT_LIST_DIR}/ComputeOutput.slang")

foreach(entry IN ITEMS main typed)
    set(source "${compute_source}")
    if(entry STREQUAL "main")
        set(symbol GraphFill)
    else()
        set(source "${CMAKE_CURRENT_LIST_DIR}/TypedComputeOutput.slang")
        set(symbol TypedGraphFill)
    endif()

    set(header "${CMAKE_CURRENT_BINARY_DIR}/${symbol}.hpp")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${LUX_PROBE_SHADER_COMPILER}" "${source}" ${entry} compute bundle "${header}"
            "--symbol=k${symbol}"
        DEPENDS "${source}" "${compute_source}" "${LUX_PROBE_SHADER_COMPILER}"
        VERBATIM
    )

    list(APPEND compute_headers "${header}")
endforeach()

foreach(entry IN ITEMS Fullscreen Red Green SampleTarget RecordColor)
    set(raster_source "${CMAKE_CURRENT_SOURCE_DIR}/ResidentRaster.slang")
    if(entry STREQUAL "SampleTarget")
        set(raster_source "${CMAKE_CURRENT_SOURCE_DIR}/SampleTarget.slang")
    elseif(entry STREQUAL "RecordColor")
        set(raster_source "${CMAKE_CURRENT_SOURCE_DIR}/RecordColor.slang")
    endif()

    set(stage fragment)
    if(entry STREQUAL "Fullscreen")
        set(stage vertex)
    endif()

    set(header "${CMAKE_CURRENT_BINARY_DIR}/Sdk${entry}.hpp")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${LUX_PROBE_SHADER_COMPILER}" "${raster_source}" "${entry}" ${stage} bundle "${header}"
            "--symbol=kSdk${entry}"
        DEPENDS "${raster_source}" "${LUX_PROBE_SHADER_COMPILER}"
        VERBATIM
    )

    list(APPEND raster_headers "${header}")
endforeach()

find_path(render_shader_include NAMES lux/engine/render/resource/view/SceneView.slang REQUIRED)
set(view_shader "${CMAKE_CURRENT_SOURCE_DIR}/ViewTint.slang")
set(view_header "${CMAKE_CURRENT_BINARY_DIR}/ViewTint.hpp")
add_custom_command(
    OUTPUT "${view_header}"
    COMMAND "${LUX_PROBE_SHADER_COMPILER}" "${view_shader}" main fragment bundle "${view_header}"
        "--symbol=kViewTint" "-I${render_shader_include}" "--depfile=${view_header}.d"
    DEPENDS "${view_shader}" "${LUX_PROBE_SHADER_COMPILER}"
    DEPFILE "${view_header}.d"
    VERBATIM
)

set(particle_shader "${CMAKE_CURRENT_SOURCE_DIR}/ParticleColor.slang")
set(particle_header "${CMAKE_CURRENT_BINARY_DIR}/ParticleColor.hpp")
add_custom_command(
    OUTPUT "${particle_header}"
    COMMAND "${LUX_PROBE_SHADER_COMPILER}" "${particle_shader}" main compute bundle "${particle_header}" --symbol=kParticleColor
    DEPENDS "${particle_shader}" "${LUX_PROBE_SHADER_COMPILER}"
    VERBATIM
)

add_executable(compute_output ComputeOutput.cpp ${compute_headers} ${raster_headers} "${particle_header}" "${view_header}")

set(joint_shader "${render_test_sources}/shaders/EffectGraphImport.slang")
set(joint_header "${CMAKE_CURRENT_BINARY_DIR}/JointAverage.hpp")

add_custom_command(
    OUTPUT "${joint_header}"
    COMMAND "${LUX_PROBE_SHADER_COMPILER}" "${joint_shader}" main compute bundle "${joint_header}"
        --symbol=kJointAverage
    DEPENDS "${joint_shader}" "${LUX_PROBE_SHADER_COMPILER}"
    VERBATIM
)

add_executable(joint_lifecycle JointLifecycle.cpp "${joint_header}")

target_include_directories(joint_lifecycle PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${render_test_sources}")

target_link_libraries(joint_lifecycle PRIVATE
    lux::engine::function::render_extensions lux::engine::core::core_execution
)

target_compile_features(joint_lifecycle PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(joint_lifecycle PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR-)
endif()

set(image_read_shader "${CMAKE_CURRENT_SOURCE_DIR}/SharedImageRead.slang")
set(image_read_header "${CMAKE_CURRENT_BINARY_DIR}/SharedImageRead.hpp")

add_custom_command(
    OUTPUT "${image_read_header}"
    COMMAND "${LUX_PROBE_SHADER_COMPILER}" "${image_read_shader}" main compute bundle "${image_read_header}"
        --symbol=kSharedImageRead
    DEPENDS "${image_read_shader}" "${LUX_PROBE_SHADER_COMPILER}"
    VERBATIM
)

add_executable(shared_images SharedImages.cpp "${image_read_header}")

target_include_directories(shared_images PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${render_test_sources}")

target_link_libraries(shared_images PRIVATE lux::engine::function::render_extensions)

target_compile_features(shared_images PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(shared_images PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR-)
endif()

target_include_directories(compute_output PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${render_test_sources}")

target_link_libraries(compute_output PRIVATE lux::engine::function::render_extensions)

if(MSVC)
    target_compile_options(compute_output PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR-)
endif()

add_test(NAME installed.render.compute_output_vulkan COMMAND compute_output)
add_test(NAME installed.render.cold_raster_vulkan COMMAND compute_output vulkan cold-programs)
set_tests_properties(installed.render.cold_raster_vulkan PROPERTIES TIMEOUT 45)

add_test(NAME installed.render.joint_lifecycle_vulkan COMMAND joint_lifecycle vulkan)
set_tests_properties(installed.render.joint_lifecycle_vulkan PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)

foreach(mode IN ITEMS receipts receipt-partial receipt-unknown)
    add_test(NAME installed.render.buffer_${mode}_vulkan COMMAND compute_output vulkan ${mode})
    set_tests_properties(installed.render.buffer_${mode}_vulkan PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77)
endforeach()

set(native_backends vulkan)
if(WIN32)
    list(APPEND native_backends d3d12)
    add_test(NAME installed.render.compute_output_d3d12 COMMAND compute_output d3d12)
    set_tests_properties(installed.render.compute_output_d3d12 PROPERTIES TIMEOUT 45)
endif()

foreach(backend IN LISTS native_backends)
    add_test(NAME installed.render.raster_records_${backend} COMMAND compute_output ${backend} resident-records)
    add_test(NAME installed.render.particles_${backend} COMMAND compute_output ${backend} particles)
    add_test(NAME installed.render.debug_geometry_${backend} COMMAND compute_output ${backend} debug-draw)
    add_test(NAME installed.render.target_draw_list_${backend} COMMAND compute_output ${backend} target-list)
    set_tests_properties(installed.render.target_draw_list_${backend} PROPERTIES TIMEOUT 45)
    set_tests_properties(installed.render.particles_${backend} installed.render.debug_geometry_${backend} PROPERTIES TIMEOUT 45)
    add_test(NAME installed.render.shared_images_${backend} COMMAND shared_images ${backend})

    set_tests_properties(
        installed.render.raster_records_${backend} installed.render.shared_images_${backend}
        PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77
    )
endforeach()

add_test(NAME installed.render.replica_unknown_vulkan COMMAND compute_output vulkan unknown)
set_tests_properties(installed.render.compute_output_vulkan installed.render.replica_unknown_vulkan PROPERTIES TIMEOUT 45)

# Compile the public authoring entry points independently against the installed SDK.
add_library(render_sdk_headers OBJECT)
target_link_libraries(render_sdk_headers PRIVATE lux::engine::function::render_extensions)
target_compile_features(render_sdk_headers PRIVATE cxx_std_20)

foreach(header IN ITEMS
    effect/EffectGraph effect/EffectCompileContext effect/CpuCall graph/Work graph/DrawList
    resource/ResourceBinding resource/ResourceProjection resource/StorageBinding device/ResourceUploader
    runtime/commands/Registration
)
    get_filename_component(name "${header}" NAME)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/${name}Header.cpp")
    file(GENERATE OUTPUT "${source}" CONTENT "#include <lux/engine/render/${header}.hpp>\n")
    target_sources(render_sdk_headers PRIVATE "${source}")
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

add_executable(effect_build EffectBuild.cpp "${joint_header}")

target_include_directories(effect_build PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(effect_build PRIVATE
    lux::engine::function::render_extensions
    lux::engine::core::core_execution
)

target_compile_features(effect_build PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(effect_build PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR- /bigobj)
endif()

add_test(NAME installed.render.effect_build_vulkan COMMAND effect_build)

set_tests_properties(installed.render.effect_build_vulkan PROPERTIES TIMEOUT 45)

add_test(NAME installed.render.extension COMMAND render_extension)
set_tests_properties(installed.render.extension PROPERTIES TIMEOUT 45)

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
