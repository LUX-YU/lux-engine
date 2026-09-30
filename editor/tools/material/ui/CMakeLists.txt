find_package(imgui REQUIRED COMPONENTS core)
add_component(COMPONENT_NAME material_ui NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/MaterialNodeControls.cpp src/MaterialView.cpp src/ViewFactory.cpp)
component_include_directories(material_ui BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(material_ui PUBLIC lux::engine::editor::material_interaction lux::engine::editor::material_preview
    lux::engine::editor::scene_ui lux::engine::editor::project_ui lux::engine::editor::editor_widgets PRIVATE imgui::core)
component_add_transitive_commands(material_ui
    "find_package(lux-engine-editor-material-interaction REQUIRED COMPONENTS material_interaction)"
    "find_package(lux-engine-editor-material-preview REQUIRED COMPONENTS material_preview)"
    "find_package(lux-engine-editor-scene-ui REQUIRED COMPONENTS scene_ui)"
    "find_package(lux-engine-editor-project-ui REQUIRED COMPONENTS project_ui)"
    "find_package(lux-engine-editor-widgets REQUIRED COMPONENTS editor_widgets)"
    "find_package(imgui REQUIRED COMPONENTS core)")
lux_classify_target(TARGET material_ui LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-material-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS material_ui)
