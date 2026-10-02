generate_visibility_header(
    ENABLE_MACRO_NAME LUX_EDITOR_METADATA_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_METADATA_PUBLIC
    GENERATE_FILE_PATH lux/engine/editor/metadata/visibility.h
)
add_component(COMPONENT_NAME editor_metadata NAMESPACE lux::engine::editor
    SOURCE_FILES src/ComponentEditorRegistry.cpp src/EditorPlugin.cpp)
lux_classify_target(TARGET editor_metadata LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
component_include_directories(editor_metadata
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include)
target_compile_definitions(editor_metadata PRIVATE LUX_EDITOR_METADATA_LIBRARY "LUX_PLUGIN_SDK_ABI=\"${LUX_PLUGIN_SDK_ABI}\"")
target_link_libraries(editor_metadata PUBLIC lux::engine::project::project_plugin_rendering lux::engine::editor::editor_configuration lux::engine::function::ui lux::engine::editor::editor_editing lux::engine::project::project_plugins lux::engine::core::meta
        lux::engine::simulation::system lux::engine::simulation::simulation_composition lux::engine::simulation::ecs::schema
        lux::engine::scene::scene_system lux::engine::scene::scene_render
    )
component_add_transitive_commands(editor_metadata
    "find_package(lux-engine-editor-configuration REQUIRED COMPONENTS editor_configuration)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package(lux-engine-project-plugin-rendering REQUIRED COMPONENTS project_plugin_rendering)"
    "find_package(lux-engine-editor-editing REQUIRED COMPONENTS editor_editing)"
    "find_package(lux-engine-project-plugins REQUIRED COMPONENTS project_plugins)"
    "find_package(lux-engine-simulation-composition REQUIRED COMPONENTS simulation_composition)"
    "find_package(lux-engine-platform REQUIRED COMPONENTS dynamic_library)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS schema)"
    "find_package(lux-engine-scene-system REQUIRED COMPONENTS scene_system)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)")
lux_engine_install_components(PROJECT_NAME lux-engine-editor-metadata VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_metadata)

