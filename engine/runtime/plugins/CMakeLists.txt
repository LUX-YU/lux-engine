add_interface_component(COMPONENT_NAME plugin_identity NAMESPACE lux::engine::plugins)

component_include_directories(
    plugin_identity
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/identity/include
    INSTALL_TIME include
)

lux_classify_target(TARGET plugin_identity LAYER ENGINE PRODUCT SHARED ROLE LIBRARY)

find_package(nlohmann_json CONFIG REQUIRED)

# Root architecture traversal must see the imported target's actual dependency edges.
set_property(TARGET nlohmann_json::nlohmann_json PROPERTY IMPORTED_GLOBAL TRUE)

add_component(
    COMPONENT_NAME plugin_runtime
    NAMESPACE lux::engine::plugins
    STATIC
    SOURCE_FILES
        src/PluginCatalog.cpp
        src/PluginLibrary.cpp
        src/PluginManager.cpp
        src/SceneRegistrations.cpp
)

lux_classify_target(TARGET plugin_runtime LAYER ENGINE PRODUCT SHARED ROLE LIBRARY)

component_include_directories(
    plugin_runtime
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)

target_compile_definitions(plugin_runtime PRIVATE "LUX_PLUGIN_SDK_ABI=\"${LUX_PLUGIN_SDK_ABI}\"")

target_link_libraries(
    plugin_runtime
    PUBLIC
        plugin_identity
        lux::engine::platform::dynamic_library
        lux::engine::simulation::system
        lux::engine::simulation::simulation_composition
        lux::engine::ecs::ecs_schema
        lux::engine::scene::scene_system
        lux::engine::scene::scene_composition
    PRIVATE nlohmann_json::nlohmann_json
)

component_add_transitive_commands(
    plugin_runtime
    "find_package(lux-engine-plugins REQUIRED COMPONENTS plugin_identity)"
    "find_package(lux-engine-platform REQUIRED COMPONENTS dynamic_library)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-composition REQUIRED COMPONENTS simulation_composition)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_schema)"
    "find_package(lux-engine-scene-system REQUIRED COMPONENTS scene_system)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(nlohmann_json CONFIG REQUIRED)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-plugins
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::plugins
    COMPONENTS plugin_identity plugin_runtime
)

install(DIRECTORY schema/ DESTINATION share/lux-engine/plugins/schema)

if(BUILD_TESTING)
    add_executable(engine_headless_test ${PROJECT_SOURCE_DIR}/engine/runtime/engine/test/headless.cpp)

    target_link_libraries(
        engine_headless_test
        PRIVATE
            lux::engine::engine_runtime
            plugin_runtime
            lux::engine::scene::system_world_streaming
            lux::engine::scene::scene_asset
    )

    if(MSVC)
        target_compile_options(engine_headless_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(engine_headless_test PRIVATE -UNDEBUG)
    endif()

    add_test(
        NAME engine.headless_context
        COMMAND engine_headless_test
            "${CMAKE_BINARY_DIR}/share/lux-engine/plugins/catalog.json" "${CMAKE_BINARY_DIR}"
    )

    set_tests_properties(engine.headless_context PROPERTIES TIMEOUT 30)
endif()
