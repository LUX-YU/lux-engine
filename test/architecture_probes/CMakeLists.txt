foreach(probe IN ITEMS spatial3d spatial2d pixel robot)
    add_executable(architecture_probe_${probe} ${probe}_probe.cpp)
    target_link_libraries(
        architecture_probe_${probe}
        PRIVATE
            lux::engine::scene::scene_runtime_presentation
            lux::engine::simulation::ecs::transform
    )
    lux_classify_target(
        TARGET  architecture_probe_${probe}
        LAYER   HOST
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    add_test(NAME architecture_probe_${probe} COMMAND architecture_probe_${probe})
    set_tests_properties(
        architecture_probe_${probe}
        PROPERTIES LABELS "architecture;probe;host"
    )
endforeach()

find_package(Jolt CONFIG REQUIRED)

add_executable(architecture_probe_jolt_l1 jolt_l1_probe.cpp)
target_link_libraries(
    architecture_probe_jolt_l1
    PRIVATE
        Jolt::Jolt
        lux::engine::simulation::simulation_core
        lux::engine::simulation::ecs::transform
)
lux_classify_target(
    TARGET  architecture_probe_jolt_l1
    LAYER   HOST
    PRODUCT TEST
    ROLE    COMPOSITION
)
add_test(NAME architecture_probe_jolt_l1 COMMAND architecture_probe_jolt_l1)
set_tests_properties(
    architecture_probe_jolt_l1
    PROPERTIES LABELS "architecture;probe;host;physics3d;jolt"
)

if(TARGET render_vulkan)
    add_executable(architecture_probe_render_lanes render_lane_probe.cpp)
    target_include_directories(
        architecture_probe_render_lanes
        PRIVATE ${PROJECT_SOURCE_DIR}/modules/function/render/test
    )
    target_link_libraries(
        architecture_probe_render_lanes
        PRIVATE
            lux::engine::function::render_vulkan
            lux::engine::scene::scene_runtime_presentation
    )
    lux_classify_target(
        TARGET  architecture_probe_render_lanes
        LAYER   HOST
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    add_test(NAME architecture_probe_render_lanes COMMAND architecture_probe_render_lanes)
    set_tests_properties(
        architecture_probe_render_lanes
        PROPERTIES
            LABELS "architecture;probe;host;presentation;render;gpu"
            SKIP_RETURN_CODE 77
            TIMEOUT 30
    )
endif()

option(
    LUX_BUILD_RENDER_LANE_WINDOW_QUALIFICATION
    "Build the opt-in visible-window Simulation/Presentation/Render lane qualification"
    OFF
)
if(LUX_BUILD_RENDER_LANE_WINDOW_QUALIFICATION AND TARGET render_vulkan)
    add_executable(
        l1_l3_render_lane_window_qualification
        l1_l3_render_lane_window_qualification.cpp
    )
    target_include_directories(
        l1_l3_render_lane_window_qualification
        PRIVATE ${PROJECT_SOURCE_DIR}/modules/function/render/test
    )
    target_link_libraries(
        l1_l3_render_lane_window_qualification
        PRIVATE
            lux::engine::function::render_vulkan
            lux::engine::scene::scene_runtime_presentation
            lux::engine::simulation::simulation_core
            lux::engine::simulation::transform_system
    )
    lux_classify_target(
        TARGET  l1_l3_render_lane_window_qualification
        LAYER   HOST
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    add_test(
        NAME l1_l3_render_lane_window_qualification
        COMMAND l1_l3_render_lane_window_qualification
    )
    set_tests_properties(
        l1_l3_render_lane_window_qualification
        PROPERTIES
            LABELS "architecture;qualification;host;simulation;presentation;render;gpu;window"
            SKIP_RETURN_CODE 77
            TIMEOUT 30
    )
endif()

if(TARGET render_features AND TARGET lux_asset_packer AND LUX_BUILD_PACKED_RENDER_CONTENT)
    set(texture_qualification_content ${CMAKE_CURRENT_BINARY_DIR}/texture_qualification/content)
    set(texture_qualification_asset ${texture_qualification_content}/checker.luxasset)
    set(texture_qualification_pak ${CMAKE_CURRENT_BINARY_DIR}/texture_qualification/content.luxpak)
    add_custom_command(
        OUTPUT ${texture_qualification_asset}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${texture_qualification_content}
        COMMAND $<TARGET_FILE:lux_asset_packer>
            --type texture
            --source_path ${PROJECT_SOURCE_DIR}/modules/function/render/features/assets/textures/ringed_gas_giant_planet.png
            --target_path ${texture_qualification_asset}
            --texture_format bc3_srgb
            --texture_color_space srgb
        DEPENDS
            lux_asset_packer
            ${PROJECT_SOURCE_DIR}/modules/function/render/features/assets/textures/ringed_gas_giant_planet.png
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${texture_qualification_pak}
        COMMAND $<TARGET_FILE:lux_asset_packer>
            --pack
            --source_path ${texture_qualification_content}
            --target_path ${texture_qualification_pak}
        DEPENDS lux_asset_packer ${texture_qualification_asset}
        VERBATIM
    )
    add_custom_target(
        texture_asset_vulkan_qualification_content
        DEPENDS ${texture_qualification_pak}
    )
    add_executable(
        texture_asset_vulkan_qualification
        texture_asset_vulkan_qualification.cpp
    )
    add_dependencies(
        texture_asset_vulkan_qualification
        texture_asset_vulkan_qualification_content
    )
    lux_add_build_tool_dependency(
        CONSUMER texture_asset_vulkan_qualification
        TOOL lux_asset_packer
    )
    target_include_directories(
        texture_asset_vulkan_qualification
        PRIVATE ${PROJECT_SOURCE_DIR}/modules/function/render/test
    )
    target_compile_definitions(
        texture_asset_vulkan_qualification
        PRIVATE LUX_TEXTURE_QUALIFICATION_PAK="${texture_qualification_pak}"
    )
    target_link_libraries(
        texture_asset_vulkan_qualification
        PRIVATE
            render_features
            lux::engine::function::render_vulkan
            lux::engine::resource::asset
    )
    lux_classify_target(
        TARGET  texture_asset_vulkan_qualification
        LAYER   HOST
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    add_test(NAME texture_asset_vulkan_qualification COMMAND texture_asset_vulkan_qualification)
    set_tests_properties(
        texture_asset_vulkan_qualification
        PROPERTIES
            LABELS "architecture;qualification;host;render;gpu;texture;asset"
            SKIP_RETURN_CODE 77
            TIMEOUT 120
    )

    set(model_qualification_source
        ${CMAKE_BINARY_DIR}/engine/toolchain/asset/model/fixtures/static/static_pbr.gltf
    )
    set(model_qualification_content
        ${CMAKE_CURRENT_BINARY_DIR}/model_qualification/content
    )
    set(model_qualification_pak
        ${CMAKE_CURRENT_BINARY_DIR}/model_qualification/content.luxpak
    )
    add_custom_command(
        OUTPUT ${model_qualification_pak}
        COMMAND $<TARGET_FILE:toolchain_model_cooker_static_test> --fixture-only
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${model_qualification_content}
        COMMAND $<TARGET_FILE:lux_asset_packer>
            --type model
            --source_path ${model_qualification_source}
            --target_path ${model_qualification_content}
        COMMAND $<TARGET_FILE:lux_asset_packer>
            --pack
            --source_path ${model_qualification_content}
            --target_path ${model_qualification_pak}
        DEPENDS
            lux_asset_packer
            toolchain_model_cooker_static_test
        VERBATIM
    )
    add_custom_target(
        model_asset_vulkan_qualification_content
        DEPENDS ${model_qualification_pak}
    )
    add_executable(
        model_asset_vulkan_qualification
        model_asset_vulkan_qualification.cpp
    )
    add_dependencies(
        model_asset_vulkan_qualification
        model_asset_vulkan_qualification_content
    )
    lux_add_build_tool_dependency(
        CONSUMER model_asset_vulkan_qualification
        TOOL lux_asset_packer
    )
    target_include_directories(
        model_asset_vulkan_qualification
        PRIVATE ${PROJECT_SOURCE_DIR}/modules/function/render/test
    )
    target_compile_definitions(
        model_asset_vulkan_qualification
        PRIVATE LUX_MODEL_QUALIFICATION_PAK="${model_qualification_pak}"
    )
    target_link_libraries(
        model_asset_vulkan_qualification
        PRIVATE
            render_features
            lux::engine::function::render_vulkan
            lux::engine::resource::asset
            lux::engine::scene::scene_runtime_render
    )
    lux_classify_target(
        TARGET  model_asset_vulkan_qualification
        LAYER   HOST
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    add_test(
        NAME model_asset_vulkan_qualification
        COMMAND model_asset_vulkan_qualification
    )
    set_tests_properties(
        model_asset_vulkan_qualification
        PROPERTIES
            LABELS "architecture;qualification;host;render;gpu;model;asset"
            SKIP_RETURN_CODE 77
            TIMEOUT 120
    )

    add_executable(
        l1_l3_render_sync_3d_qualification
        l1_l3_render_sync_3d_qualification.cpp
        support/QualificationRenderRuntime.cpp
    )
    add_dependencies(
        l1_l3_render_sync_3d_qualification
        model_asset_vulkan_qualification_content
    )
    target_include_directories(
        l1_l3_render_sync_3d_qualification
        PRIVATE
            ${PROJECT_SOURCE_DIR}/modules/function/render/test
            ${CMAKE_CURRENT_SOURCE_DIR}/support
    )
    target_compile_definitions(
        l1_l3_render_sync_3d_qualification
        PRIVATE
            LUX_MODEL_QUALIFICATION_PAK="${model_qualification_pak}"
    )
    target_link_libraries(
        l1_l3_render_sync_3d_qualification
        PRIVATE
            render_features
            lux::engine::function::render_vulkan
            lux::engine::resource::asset
            lux::engine::scene::scene_runtime_render
            scene_runtime_render_meta
            render_client_meta
    )
    lux_classify_target(
        TARGET  l1_l3_render_sync_3d_qualification
        LAYER   HOST
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    add_test(NAME l1_l3_render_sync_3d_qualification COMMAND l1_l3_render_sync_3d_qualification)
    set_tests_properties(
        l1_l3_render_sync_3d_qualification
        PROPERTIES
            LABELS "architecture;qualification;host;render;gpu;l1;l3"
            SKIP_RETURN_CODE 77
            TIMEOUT 120
    )

    set(large_3d_scene_screenshot
        ${CMAKE_CURRENT_BINARY_DIR}/large_3d_scene_performance.ppm
    )
    add_executable(
        large_3d_scene_performance_qualification
        large_3d_scene_performance_qualification.cpp
    )
    add_dependencies(
        large_3d_scene_performance_qualification
        model_asset_vulkan_qualification_content
    )
    target_include_directories(
        large_3d_scene_performance_qualification
        PRIVATE ${PROJECT_SOURCE_DIR}/modules/function/render/test
    )
    target_compile_definitions(
        large_3d_scene_performance_qualification
        PRIVATE
            LUX_MODEL_QUALIFICATION_PAK="${model_qualification_pak}"
            LUX_LARGE_3D_SCENE_SCREENSHOT="${large_3d_scene_screenshot}"
    )
    target_link_libraries(
        large_3d_scene_performance_qualification
        PRIVATE
            render_features
            lux::engine::function::render_vulkan
            lux::engine::resource::asset
            lux::engine::scene::scene_runtime_render
    )
    lux_classify_target(
        TARGET  large_3d_scene_performance_qualification
        LAYER   HOST
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    add_test(
        NAME large_3d_scene_performance_qualification
        COMMAND large_3d_scene_performance_qualification --require-gpu
    )
    set_tests_properties(
        large_3d_scene_performance_qualification
        PROPERTIES
            LABELS "architecture;qualification;host;render;gpu;performance;large-scene"
            SKIP_RETURN_CODE 77
            TIMEOUT 180
    )
endif()

option(
    LUX_BUILD_PRELOADED_SPATIAL3D_PROBE
    "Build the gated World/Jolt/Render preloaded Spatial3D vertical slice"
    OFF
)
if(LUX_BUILD_PRELOADED_SPATIAL3D_PROBE)
    set(spatial3d_probe_fragment_spv
        ${CMAKE_CURRENT_BINARY_DIR}/preloaded_spatial3d.frag.spv
    )
    set(spatial3d_probe_gbuffer_fragment_spv
        ${CMAKE_CURRENT_BINARY_DIR}/preloaded_spatial3d_gbuffer.frag.spv
    )
    add_custom_command(
        OUTPUT ${spatial3d_probe_fragment_spv}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE}
            --target-env=vulkan1.2
            -fshader-stage=frag
            ${PROJECT_SOURCE_DIR}/modules/function/render/test/render_3d_semantic.frag
            -o ${spatial3d_probe_fragment_spv}
        DEPENDS ${PROJECT_SOURCE_DIR}/modules/function/render/test/render_3d_semantic.frag
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${spatial3d_probe_gbuffer_fragment_spv}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE}
            --target-env=vulkan1.2
            -fshader-stage=frag
            ${PROJECT_SOURCE_DIR}/modules/function/render/test/render_3d_semantic_gbuffer.frag
            -o ${spatial3d_probe_gbuffer_fragment_spv}
        DEPENDS ${PROJECT_SOURCE_DIR}/modules/function/render/test/render_3d_semantic_gbuffer.frag
        VERBATIM
    )
    add_custom_target(
        architecture_probe_spatial3d_preloaded_shaders
        DEPENDS ${spatial3d_probe_fragment_spv} ${spatial3d_probe_gbuffer_fragment_spv}
    )

    add_executable(architecture_probe_spatial3d_preloaded spatial3d_preloaded_probe.cpp)
    add_dependencies(
        architecture_probe_spatial3d_preloaded
        architecture_probe_spatial3d_preloaded_shaders
    )
    target_include_directories(
        architecture_probe_spatial3d_preloaded
        PRIVATE ${PROJECT_SOURCE_DIR}/modules/function/render/test
    )
    target_compile_definitions(
        architecture_probe_spatial3d_preloaded
        PRIVATE
            LUX_SPATIAL3D_FRAGMENT_SPV="${spatial3d_probe_fragment_spv}"
            LUX_SPATIAL3D_GBUFFER_FRAGMENT_SPV="${spatial3d_probe_gbuffer_fragment_spv}"
    )
    target_link_libraries(
        architecture_probe_spatial3d_preloaded
        PRIVATE
            Jolt::Jolt
            render_features
            lux::engine::function::render_vulkan
            lux::engine::scene::scene_core
            lux::engine::scene::scene_runtime_presentation
            lux::engine::scene::scene_runtime_world
            lux::engine::simulation::ecs::transform
    )
    lux_classify_target(
        TARGET  architecture_probe_spatial3d_preloaded
        LAYER   HOST
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    add_test(NAME architecture_probe_spatial3d_preloaded COMMAND architecture_probe_spatial3d_preloaded)
    set_tests_properties(
        architecture_probe_spatial3d_preloaded
        PROPERTIES
            LABELS "architecture;probe;host;spatial3d;jolt;render;gpu"
            SKIP_RETURN_CODE 77
            TIMEOUT 120
    )
endif()
