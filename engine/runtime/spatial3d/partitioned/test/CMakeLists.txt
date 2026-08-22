if(TARGET ecs_meta AND TARGET spatial3d_components_meta)
    add_executable(
        spatial3d_scene_runtime_test
        ${CMAKE_CURRENT_SOURCE_DIR}/spatial3d_scene_runtime_test.cpp
    )
    target_link_libraries(
        spatial3d_scene_runtime_test
        PRIVATE
            lux::engine::runtime::runtime_pack_spatial3d_partitioned
            lux::engine::runtime::runtime_scene_core
            lux::engine::runtime::runtime_execution
            lux::engine::function::input
            lux::engine::resource::asset
            lux::engine::core::meta
            ecs_meta
            spatial3d_components_meta
    )
    if(MSVC)
        target_compile_options(
            spatial3d_scene_runtime_test
            PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor /UNDEBUG
        )
    else()
        target_compile_options(spatial3d_scene_runtime_test PRIVATE -UNDEBUG)
    endif()
    lux_classify_target(
        TARGET  spatial3d_scene_runtime_test
        LAYER   RUNTIME
        PRODUCT TEST
        ROLE    INTEGRATION
    )
endif()
