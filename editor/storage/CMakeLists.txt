generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_STORAGE_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_STORAGE_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_STORAGE_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/storage/visibility.h)
add_component(COMPONENT_NAME editor_storage NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/ProjectStorage.cpp src/ProjectOpenData.cpp src/ProjectAssetSource.cpp src/ProjectPublication.cpp src/ProjectCreation.cpp)
component_include_directories(editor_storage
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
        ${PROJECT_SOURCE_DIR}/editor/editing/sinclude
    INSTALL_TIME include)
target_compile_definitions(editor_storage PUBLIC LUX_EDITOR_STORAGE_DLL_DISABLE)
find_package(tomlplusplus CONFIG REQUIRED)
target_compile_definitions(editor_storage PRIVATE TOML_HEADER_ONLY=1 TOML_EXCEPTIONS=0)
target_include_directories(editor_storage PRIVATE $<TARGET_PROPERTY:tomlplusplus::tomlplusplus,INTERFACE_INCLUDE_DIRECTORIES>)
option(LUX_EDITOR_PUBLICATION_DIAGNOSTICS "Private publication interruption probes; never enable for a normal SDK" OFF)
if(LUX_EDITOR_PUBLICATION_DIAGNOSTICS)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        message(FATAL_ERROR "Editor publication diagnostics require RelWithDebInfo")
    endif()
    target_compile_definitions(editor_storage PRIVATE LUX_EDITOR_PUBLICATION_DIAGNOSTICS)
endif()
target_link_libraries(editor_storage PUBLIC lux::engine::editor::editor_project lux::engine::editor::editor_editing
    lux::engine::core::object lux::engine::core::log lux::engine::core::meta
    lux::engine::process::process_execution lux::engine::process::asset_loading lux::engine::resource::asset)
target_link_libraries(editor_storage PRIVATE lux::engine::scene::scene_asset)
component_add_transitive_commands(editor_storage "find_package(lux-engine-scene-asset REQUIRED COMPONENTS scene_asset)")
component_add_transitive_commands(editor_storage
    "find_package(lux-engine-editor-project REQUIRED COMPONENTS editor_project)"
    "find_package(lux-engine-editor-editing REQUIRED COMPONENTS editor_editing)"
    "find_package(lux-engine-core REQUIRED COMPONENTS object meta log)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-process-asset-loading REQUIRED COMPONENTS process_asset_loading)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)")
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_storage_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectStorage.hpp
    EXTRA_COMPILE_OPTIONS -fdelayed-template-parsing REGISTER_FUNC_NAME lux_editor_storage)
engine_target_add_type_runtime_info(TARGET editor_storage METAS editor_storage_meta OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
lux_classify_target(TARGET editor_storage LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-storage VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_storage)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_project_creation_test test/project_creation.cpp)
    target_link_libraries(editor_project_creation_test PRIVATE editor_storage lux::engine::scene::scene_asset)
    if(MSVC)
        target_compile_options(editor_project_creation_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(editor_project_creation_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME editor.project_creation COMMAND editor_project_creation_test "${CMAKE_CURRENT_BINARY_DIR}/creation")
    set_tests_properties(editor.project_creation PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    add_executable(editor_plugin_publication_test test/plugin_publication.cpp)
    target_link_libraries(editor_plugin_publication_test PRIVATE editor_storage)
    if(MSVC)
        target_compile_options(editor_plugin_publication_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(editor_plugin_publication_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME editor.plugin_publication COMMAND editor_plugin_publication_test "${CMAKE_CURRENT_BINARY_DIR}/plugin-publication")
    set_tests_properties(editor.plugin_publication PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    set_tests_properties(editor.project_creation editor.plugin_publication
        PROPERTIES LABELS "editor;native")
endif()
