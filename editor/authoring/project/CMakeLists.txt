find_package(lux-cxx REQUIRED COMPONENTS compile_time)
find_package(tomlplusplus CONFIG REQUIRED)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_PROJECT_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_PROJECT_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_PROJECT_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/project/visibility.h)
add_component(COMPONENT_NAME editor_project STATIC NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_project
    SOURCE_FILES src/ProjectManifest.cpp src/ProjectBuilder.cpp src/ProjectCatalogModel.cpp src/AssetCatalog.cpp)
component_include_directories(editor_project
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR} INSTALL_TIME include)
target_compile_definitions(editor_project PRIVATE LUX_EDITOR_PROJECT_LIBRARY TOML_HEADER_ONLY=1 TOML_EXCEPTIONS=0)
target_include_directories(editor_project PRIVATE $<TARGET_PROPERTY:tomlplusplus::tomlplusplus,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(editor_project PUBLIC lux::engine::resource::identity lux::engine::resource::asset lux::cxx::compile_time lux::engine::core::object)
target_link_libraries(editor_project PRIVATE lux::engine::scene::scene_asset)
component_add_transitive_commands(editor_project "find_package(lux-engine-scene-asset REQUIRED COMPONENTS scene_asset)")
component_add_transitive_commands(editor_project
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity asset)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS object)")
lux_classify_target(TARGET editor_project LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-project VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_project)

target_compile_definitions(editor_project PUBLIC LUX_EDITOR_PROJECT_DLL_DISABLE)
