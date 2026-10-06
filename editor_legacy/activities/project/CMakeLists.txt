add_subdirectory(launch)

generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_STORAGE_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_STORAGE_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_STORAGE_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/storage/visibility.h)
add_component(COMPONENT_NAME editor_storage_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/ProjectStorage.cpp src/ProjectContentSaving.cpp src/ProjectContentReloading.cpp src/ProjectPluginSelection.cpp src/RecentProjects.cpp
        src/PreparedProjectOpen.cpp src/ProjectAssetSource.cpp src/ProjectPublication.cpp src/ProjectCreation.cpp
        src/ProjectPlugins.cpp src/ProjectPublicationOperation.cpp src/ArtifactPublicationOperation.cpp)
component_include_directories(editor_storage_legacy
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
        ${PROJECT_SOURCE_DIR}/editor_legacy/activities/sinclude
    INSTALL_TIME include)
target_link_libraries(editor_storage_legacy PUBLIC lux::engine::editor::session_execution_legacy)
component_add_transitive_commands(editor_storage_legacy
    "find_package(lux-engine-core REQUIRED COMPONENTS services)"
    "find_package(lux-engine-editor-session-factories REQUIRED COMPONENTS session_execution_legacy)")
target_compile_definitions(editor_storage_legacy PUBLIC LUX_EDITOR_STORAGE_DLL_DISABLE)
find_package(tomlplusplus CONFIG REQUIRED)
target_compile_definitions(editor_storage_legacy PRIVATE TOML_HEADER_ONLY=1 TOML_EXCEPTIONS=0)
target_include_directories(editor_storage_legacy PRIVATE $<TARGET_PROPERTY:tomlplusplus::tomlplusplus,INTERFACE_INCLUDE_DIRECTORIES>)
option(LUX_EDITOR_PUBLICATION_DIAGNOSTICS "Private publication interruption probes; never enable for a normal SDK" OFF)
if(LUX_EDITOR_PUBLICATION_DIAGNOSTICS)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        message(FATAL_ERROR "Editor publication diagnostics require RelWithDebInfo")
    endif()
    target_compile_definitions(editor_storage_legacy PRIVATE LUX_EDITOR_PUBLICATION_DIAGNOSTICS)
endif()
target_link_libraries(editor_storage_legacy PUBLIC lux::engine::editor::editor_project_legacy lux::engine::editor::editor_contracts_legacy
    lux::engine::core::object lux::engine::core::services lux::engine::core::log lux::engine::core::meta
    lux::engine::process::process_execution lux::engine::process::asset_loading lux::engine::resource::asset)
target_link_libraries(editor_storage_legacy PUBLIC lux::engine::project::project_plugins lux::engine::editor::editor_persistence_execution_legacy)
component_add_transitive_commands(editor_storage_legacy "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence_execution_legacy)")
component_add_transitive_commands(editor_storage_legacy "find_package(lux-engine-project-plugins REQUIRED COMPONENTS project_plugins)")
target_link_libraries(editor_storage_legacy PRIVATE lux::engine::platform::platform_process lux::engine::scene::scene_asset lux::engine::editor::editor_file_publication_legacy)
component_add_transitive_commands(editor_storage_legacy
    "find_package(lux-engine-platform REQUIRED COMPONENTS platform_process)"
    "find_package(lux-engine-editor-file-publication REQUIRED COMPONENTS editor_file_publication_legacy)")
component_add_transitive_commands(editor_storage_legacy "find_package(lux-engine-scene-asset REQUIRED COMPONENTS scene_asset)")
component_add_transitive_commands(editor_storage_legacy
    "find_package(lux-engine-platform REQUIRED COMPONENTS platform_process)"
    "find_package(lux-engine-editor-project REQUIRED COMPONENTS editor_project_legacy)"
    "find_package(lux-engine-editor-contracts REQUIRED COMPONENTS editor_contracts_legacy)"
    "find_package(lux-engine-core REQUIRED COMPONENTS object meta log)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-process-asset-loading REQUIRED COMPONENTS process_asset_loading)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)")
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_storage_meta_legacy
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectStorage.hpp
    EXTRA_COMPILE_OPTIONS -fdelayed-template-parsing REGISTER_FUNC_NAME lux_editor_storage)
engine_target_add_type_runtime_info(TARGET editor_storage_legacy METAS editor_storage_meta_legacy OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
lux_classify_target(TARGET editor_storage_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-storage VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_storage_legacy)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_project_publication_operation_test_legacy test/project_publication_operation.cpp)
    target_link_libraries(editor_project_publication_operation_test_legacy PRIVATE editor_storage_legacy editor_file_publication_legacy editor_assets_legacy)
    lux_editor_test_options(editor_project_publication_operation_test_legacy)
    add_test(NAME editor.project_publication_operation COMMAND editor_project_publication_operation_test_legacy "${CMAKE_CURRENT_BINARY_DIR}/coordinated-publication")
    set_tests_properties(editor.project_publication_operation PROPERTIES LABELS "editor;native;P12" RUN_SERIAL TRUE TIMEOUT 60)
    add_executable(editor_project_creation_test_legacy test/project_creation.cpp)
    target_link_libraries(editor_project_creation_test_legacy PRIVATE editor_storage_legacy lux::engine::scene::scene_asset)
    if(MSVC)
        target_compile_options(editor_project_creation_test_legacy PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(editor_project_creation_test_legacy PRIVATE -UNDEBUG)
    endif()
    add_test(NAME editor.project_creation COMMAND editor_project_creation_test_legacy "${CMAKE_CURRENT_BINARY_DIR}/creation")
    set_tests_properties(editor.project_creation PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    add_executable(editor_plugin_publication_test_legacy test/plugin_publication.cpp)
    target_link_libraries(editor_plugin_publication_test_legacy PRIVATE editor_storage_legacy editor_file_publication_legacy)
    if(MSVC)
        target_compile_options(editor_plugin_publication_test_legacy PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(editor_plugin_publication_test_legacy PRIVATE -UNDEBUG)
    endif()
    add_test(NAME editor.plugin_publication COMMAND editor_plugin_publication_test_legacy "${CMAKE_CURRENT_BINARY_DIR}/plugin-publication" "${CMAKE_BINARY_DIR}")
    set_tests_properties(editor.plugin_publication PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    set_tests_properties(editor.project_creation editor.plugin_publication
        PROPERTIES LABELS "editor;native")
endif()


generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_ASSETS_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_ASSETS_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_ASSETS_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/assets/visibility.h)
add_component(COMPONENT_NAME editor_assets_legacy NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/ModelImporter.cpp src/ModelImportRecipe.cpp)
component_include_directories(editor_assets_legacy
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
        ${PROJECT_SOURCE_DIR}/editor_legacy/activities/sinclude INSTALL_TIME include)
target_compile_definitions(editor_assets_legacy PUBLIC LUX_EDITOR_ASSETS_DLL_DISABLE)
find_package(tomlplusplus CONFIG REQUIRED)
target_compile_definitions(editor_assets_legacy PRIVATE TOML_HEADER_ONLY=1 TOML_EXCEPTIONS=0)
target_include_directories(editor_assets_legacy PRIVATE $<TARGET_PROPERTY:tomlplusplus::tomlplusplus,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(editor_assets_legacy PUBLIC lux::engine::editor::editor_storage_legacy lux::engine::toolchain::toolchain_model_cooker)
target_link_libraries(editor_assets_legacy PRIVATE lux::engine::platform::platform_process)
component_add_transitive_commands(editor_assets_legacy
    "find_package(lux-engine-platform REQUIRED COMPONENTS platform_process)"
    "find_package(lux-engine-editor-storage REQUIRED COMPONENTS editor_storage_legacy)"
    "find_package(lux-engine-toolchain-asset REQUIRED COMPONENTS toolchain_model_cooker)")
lux_classify_target(TARGET editor_assets_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-assets VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_assets_legacy)

# Exact SDK ownership: sibling providers share a build include root, never an install manifest.
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/assets/ModelImportRecipe.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/assets/ModelImporter.hpp
    DESTINATION include/lux/engine/editor/assets COMPONENT lux_engine_editor_assets)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ArtifactPublicationOperation.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectCommands.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectContentOpening.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectContentSaving.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectContentReloading.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/RecentProjects.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectPluginSelection.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectCreation.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectPlugins.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/PreparedProjectOpen.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectPublication.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectPublicationOperation.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectStorage.hpp
    DESTINATION include/lux/engine/editor/storage COMPONENT lux_engine_editor_storage)
install(FILES ${LUX_GENERATE_HEADER_DIR}/lux/engine/editor/storage/visibility.h
    DESTINATION include/lux/engine/editor/storage COMPONENT lux_engine_editor_storage)
install(FILES ${LUX_GENERATE_HEADER_DIR}/lux/engine/editor/assets/visibility.h
    DESTINATION include/lux/engine/editor/assets COMPONENT lux_engine_editor_assets)
