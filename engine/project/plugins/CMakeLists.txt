add_interface_component(COMPONENT_NAME project_identity NAMESPACE lux::engine::project)
component_include_directories(project_identity BUILD_TIME_EXPORT
    ${CMAKE_CURRENT_SOURCE_DIR}/identity/include INSTALL_TIME include)
lux_classify_target(TARGET project_identity LAYER SCENE PRODUCT SHARED ROLE LIBRARY)

find_package(nlohmann_json CONFIG REQUIRED)
# The architecture walk runs at the root; preserve this imported target's real identity and edges there.
set_property(TARGET nlohmann_json::nlohmann_json PROPERTY IMPORTED_GLOBAL TRUE)
add_component(COMPONENT_NAME project_plugins NAMESPACE lux::engine::project STATIC
    SOURCE_FILES src/PluginCatalog.cpp src/PluginLibrary.cpp src/PluginManager.cpp)
lux_classify_target(TARGET project_plugins LAYER SCENE PRODUCT SHARED ROLE LIBRARY)
component_include_directories(project_plugins
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude INSTALL_TIME include)
target_compile_definitions(project_plugins PRIVATE "LUX_PLUGIN_SDK_ABI=\"${LUX_PLUGIN_SDK_ABI}\"")
target_link_libraries(project_plugins
    PUBLIC project_identity lux::engine::platform::dynamic_library
        lux::engine::simulation::system lux::engine::simulation::simulation_composition
        lux::engine::simulation::ecs::schema lux::engine::scene::scene_system
    PRIVATE nlohmann_json::nlohmann_json)
component_add_transitive_commands(project_plugins
    "find_package(lux-engine-project-plugins REQUIRED COMPONENTS project_identity)"
    "find_package(lux-engine-platform REQUIRED COMPONENTS dynamic_library)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-composition REQUIRED COMPONENTS simulation_composition)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS schema)"
    "find_package(lux-engine-scene-system REQUIRED COMPONENTS scene_system)"
    "find_package(nlohmann_json CONFIG REQUIRED)")
lux_engine_install_components(PROJECT_NAME lux-engine-project-plugins VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::project COMPONENTS project_identity project_plugins)

install(DIRECTORY schema/ DESTINATION share/lux-engine/plugins/schema)
if(TARGET scene_render)
    add_subdirectory(rendering)
endif()
if(BUILD_TESTING)
    add_executable(engine_headless_test ${PROJECT_SOURCE_DIR}/engine/context/test/headless.cpp)
    target_link_libraries(engine_headless_test PRIVATE lux::engine::engine_context project_plugins
        lux::engine::scene::scene_world_loading lux::engine::scene::scene_asset)
    if(MSVC)
        target_compile_options(engine_headless_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(engine_headless_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME engine.headless_context COMMAND engine_headless_test
        "${CMAKE_BINARY_DIR}/share/lux-engine/plugins/catalog.json" "${CMAKE_BINARY_DIR}")
    set_tests_properties(engine.headless_context PROPERTIES TIMEOUT 30)
endif()
