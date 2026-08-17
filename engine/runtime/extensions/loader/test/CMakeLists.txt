add_library(minimal_extension_module MODULE minimal_extension_module.cpp)
target_link_libraries(
    minimal_extension_module
    PRIVATE lux::engine::runtime::runtime_extension_loader
)
set_target_properties(
    minimal_extension_module
    PROPERTIES
        PREFIX ""
        OUTPUT_NAME "lux_minimal_extension"
)

add_library(dependent_extension_module MODULE dependent_extension_module.cpp)
target_link_libraries(
    dependent_extension_module
    PRIVATE lux::engine::runtime::runtime_extension_loader
)
set_target_properties(
    dependent_extension_module
    PROPERTIES
        PREFIX ""
        OUTPUT_NAME "lux_dependent_extension"
)

add_library(component_extension_module MODULE component_extension_module.cpp)
target_link_libraries(
    component_extension_module
    PRIVATE
        lux::engine::runtime::runtime_extension_loader
        lux::engine::core::meta
        lux::engine::ecs::core
)
set_target_properties(
    component_extension_module
    PROPERTIES
        PREFIX ""
        OUTPUT_NAME "lux_component_extension"
)

add_library(rollback_extension_module MODULE rollback_extension_module.cpp)
target_link_libraries(
    rollback_extension_module
    PRIVATE
        lux::engine::runtime::runtime_extension_loader
        lux::engine::core::meta
        lux::engine::ecs::core
)
set_target_properties(
    rollback_extension_module
    PROPERTIES
        PREFIX ""
        OUTPUT_NAME "lux_rollback_extension"
)

add_executable(extension_module_manager_test extension_module_manager_test.cpp)
target_link_libraries(
    extension_module_manager_test
    PRIVATE lux::engine::runtime::runtime_extension_loader
)
add_dependencies(
    extension_module_manager_test
    minimal_extension_module
    dependent_extension_module
)


if(TARGET runtime_render_scene AND TARGET runtime_render_scene_test_support AND TARGET runtime_execution)
    add_executable(
        engine_extensions_test
        ${CMAKE_SOURCE_DIR}/engine/runtime/render/scene/test/engine_extensions_test.cpp
    )
    target_link_libraries(
        engine_extensions_test
        PRIVATE
            runtime_render_scene
            runtime_render_scene_test_support
            lux::engine::runtime::runtime_execution
            lux::engine::runtime::runtime_contribution_host
    )
    if(MSVC)
        target_compile_options(
            engine_extensions_test
            PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor
        )
    endif()
    add_dependencies(
        engine_extensions_test
        minimal_extension_module
        dependent_extension_module
        component_extension_module
        rollback_extension_module
    )
endif()
