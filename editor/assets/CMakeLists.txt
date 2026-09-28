generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_ASSETS_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_ASSETS_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_ASSETS_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/assets/visibility.h)
add_component(COMPONENT_NAME editor_assets NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/AssetImporter.cpp)
component_include_directories(editor_assets
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude ${PROJECT_SOURCE_DIR}/editor/storage/sinclude
        ${PROJECT_SOURCE_DIR}/editor/editing/sinclude INSTALL_TIME include)
target_compile_definitions(editor_assets PUBLIC LUX_EDITOR_ASSETS_DLL_DISABLE)
find_package(tomlplusplus CONFIG REQUIRED)
target_compile_definitions(editor_assets PRIVATE TOML_HEADER_ONLY=1 TOML_EXCEPTIONS=0)
target_include_directories(editor_assets PRIVATE $<TARGET_PROPERTY:tomlplusplus::tomlplusplus,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(editor_assets PUBLIC lux::engine::editor::editor_storage lux::engine::toolchain::toolchain_model_cooker)
component_add_transitive_commands(editor_assets
    "find_package(lux-engine-editor-storage REQUIRED COMPONENTS editor_storage)"
    "find_package(lux-engine-toolchain-asset REQUIRED COMPONENTS toolchain_model_cooker)")
lux_classify_target(TARGET editor_assets LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-assets VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_assets)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_asset_save_test test/asset_save.cpp)
    target_link_libraries(editor_asset_save_test PRIVATE editor_assets)
    target_include_directories(editor_asset_save_test PRIVATE sinclude
        ${PROJECT_SOURCE_DIR}/editor/storage/sinclude ${PROJECT_SOURCE_DIR}/editor/editing/sinclude)
    if(MSVC)
        target_compile_options(editor_asset_save_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(editor_asset_save_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME editor.asset_save_target COMMAND editor_asset_save_test "${CMAKE_CURRENT_BINARY_DIR}/asset-save")
    set_tests_properties(editor.asset_save_target PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    set_tests_properties(editor.asset_save_target
        PROPERTIES LABELS "editor;native")
endif()
