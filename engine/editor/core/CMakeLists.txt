find_package(lux-cxx REQUIRED COMPONENTS compile_time container)
find_package(tomlplusplus CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME LUX_EDITOR_CORE_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_CORE_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_EDITOR_CORE_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/core/visibility.h
)
add_component(COMPONENT_NAME editor_core NAMESPACE lux::engine::editor
    OUTPUT_NAME lux_engine_editor_core
    SOURCE_FILES src/Editor.cpp src/DocumentEditor.cpp src/Project.cpp src/ProjectManifest.cpp src/ProjectPublication.cpp
)
component_include_directories(editor_core
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)
target_compile_definitions(editor_core PRIVATE LUX_EDITOR_CORE_LIBRARY TOML_HEADER_ONLY=1 TOML_EXCEPTIONS=0)
option(LUX_EDITOR_PUBLICATION_DIAGNOSTICS "Private publication interruption probes; never enable for a normal SDK" OFF)
if(LUX_EDITOR_PUBLICATION_DIAGNOSTICS)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        message(FATAL_ERROR "Editor publication diagnostics require RelWithDebInfo")
    endif()
    target_compile_definitions(editor_core PRIVATE LUX_EDITOR_PUBLICATION_DIAGNOSTICS)
endif()
target_include_directories(editor_core PRIVATE $<TARGET_PROPERTY:tomlplusplus::tomlplusplus,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(editor_core PUBLIC
    lux::engine::editor::editor_editing
    lux::engine::core::object
    lux::engine::process::process_execution
    lux::engine::process::asset_loading
    lux::engine::resource::asset
    lux::cxx::container
)
component_add_transitive_commands(editor_core
    "find_package(lux-engine-editor-editing REQUIRED COMPONENTS editor_editing)"
    "find_package(lux-engine-core REQUIRED COMPONENTS object)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-process-asset-loading REQUIRED COMPONENTS process_asset_loading)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time container)"
)
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_project_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/project/Project.hpp
    EXTRA_COMPILE_OPTIONS -fdelayed-template-parsing
    REGISTER_FUNC_NAME lux_editor_project)
engine_target_add_type_runtime_info(TARGET editor_core METAS editor_project_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-core VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_core)
