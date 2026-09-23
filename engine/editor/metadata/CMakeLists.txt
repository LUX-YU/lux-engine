find_package(nlohmann_json CONFIG REQUIRED)
generate_visibility_header(
    ENABLE_MACRO_NAME LUX_EDITOR_METADATA_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_METADATA_PUBLIC
    GENERATE_FILE_PATH lux/engine/editor/metadata/visibility.h
)
add_component(COMPONENT_NAME editor_metadata NAMESPACE lux::engine::editor
    SOURCE_FILES src/EngineMetadata.cpp src/PluginLibrary.cpp src/ConfigurationValue.cpp src/EditorReflection.cpp)
lux_classify_target(TARGET editor_metadata LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
component_include_directories(editor_metadata
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include)
target_compile_definitions(editor_metadata PRIVATE LUX_EDITOR_METADATA_LIBRARY "LUX_PLUGIN_SDK_ABI=\"${LUX_PLUGIN_SDK_ABI}\"")
target_link_libraries(editor_metadata
    PUBLIC lux::engine::platform::dynamic_library lux::engine::core::meta
        lux::engine::simulation::system lux::engine::simulation::ecs::schema
        lux::engine::scene::scene_system lux::engine::scene::scene_render
    PRIVATE nlohmann_json::nlohmann_json)
component_add_transitive_commands(editor_metadata
    "find_package(lux-engine-platform REQUIRED COMPONENTS dynamic_library)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS schema)"
    "find_package(lux-engine-scene-system REQUIRED COMPONENTS scene_system)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)")
lux_engine_install_components(PROJECT_NAME lux-engine-editor-metadata VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_metadata)
install(DIRECTORY schema/ DESTINATION share/lux-engine/editor/schema)

if(BUILD_TESTING)
    add_executable(plugin_abilities_test test/abilities.cpp)
    target_link_libraries(plugin_abilities_test PRIVATE lux::engine::scene::scene_composition)
    if(MSVC)
        target_compile_options(plugin_abilities_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(plugin_abilities_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME plugin.abilities COMMAND plugin_abilities_test)
    add_executable(plugin_render_contracts test/render_contracts.cpp)
    target_link_libraries(plugin_render_contracts PRIVATE lux::engine::function::render_vulkan)
    if(MSVC)
        target_compile_options(plugin_render_contracts PRIVATE /UNDEBUG)
    else()
        target_compile_options(plugin_render_contracts PRIVATE -UNDEBUG)
    endif()
    add_test(NAME plugin.render_contracts COMMAND plugin_render_contracts)
    add_executable(plugin_configuration_test test/configuration.cpp)
    target_link_libraries(plugin_configuration_test PRIVATE editor_metadata lux::engine::function::ui imgui::core)
    if(MSVC)
        target_compile_options(plugin_configuration_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(plugin_configuration_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME plugin.configuration COMMAND plugin_configuration_test)
    if(TARGET physics2d_simulation)
        add_executable(plugin_library_test test/plugin.cpp)
        target_link_libraries(plugin_library_test PRIVATE editor_metadata lux::engine::simulation::simulation_composition)
        add_dependencies(plugin_library_test physics2d_simulation builtin_runtime_plugin lux_plugin_capabilities)
        if(TARGET physics2d_editor)
            add_dependencies(plugin_library_test physics2d_editor)
        endif()
        if(MSVC)
            target_compile_options(plugin_library_test PRIVATE /UNDEBUG)
        else()
            target_compile_options(plugin_library_test PRIVATE -UNDEBUG)
        endif()
        get_target_property(_physics_plugin_description physics2d_simulation LUX_PLUGIN_DESCRIPTION)
        add_test(NAME plugin.physics2d COMMAND plugin_library_test "${CMAKE_BINARY_DIR}/share/lux-engine/editor/capabilities.json" "${CMAKE_BINARY_DIR}")
    endif()
endif()

if(BUILD_TESTING)
    add_subdirectory(${PROJECT_SOURCE_DIR}/modules/function/render/vulkan/samples
        ${CMAKE_CURRENT_BINARY_DIR}/external_sample)
endif()
