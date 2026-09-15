find_package(tomlplusplus CONFIG REQUIRED)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_ASSETS_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_ASSETS_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_ASSETS_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/assets/visibility.h)
add_component(COMPONENT_NAME editor_assets NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_assets
    SOURCE_FILES src/AssetImporter.cpp)
component_include_directories(editor_assets
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${PROJECT_SOURCE_DIR}/engine/editor/core/sinclude INSTALL_TIME include)
target_compile_definitions(editor_assets PRIVATE LUX_EDITOR_ASSETS_LIBRARY TOML_HEADER_ONLY=1 TOML_EXCEPTIONS=0)
target_include_directories(editor_assets PRIVATE $<TARGET_PROPERTY:tomlplusplus::tomlplusplus,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(editor_assets PUBLIC lux::engine::editor::editor_core lux::engine::toolchain::toolchain_model_cooker)
component_add_transitive_commands(editor_assets
    "find_package(lux-engine-editor-core REQUIRED COMPONENTS editor_core)"
    "find_package(lux-engine-toolchain-asset REQUIRED COMPONENTS toolchain_model_cooker)")
lux_engine_install_components(PROJECT_NAME lux-engine-editor-assets VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_assets)
