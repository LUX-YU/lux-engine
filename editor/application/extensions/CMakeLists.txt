# Editor ABI evolves independently from the engine runtime plugin identity.
set(_editor_abi "v7;${_lux_plugin_environment}")
file(GLOB_RECURSE _extension_headers CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/editor/editing/*.hpp"
    "${PROJECT_SOURCE_DIR}/editor/authoring/*.hpp"
    "${PROJECT_SOURCE_DIR}/editor/activities/*.hpp"
    "${PROJECT_SOURCE_DIR}/editor/workbench/*.hpp"
    "${PROJECT_SOURCE_DIR}/editor/application/*.hpp")
list(FILTER _extension_headers INCLUDE REGEX "/include/[^;]+\\.hpp$")
list(SORT _extension_headers)
foreach(_header IN LISTS _extension_headers)
    file(SHA256 "${_header}" _hash)
    file(RELATIVE_PATH _logical "${PROJECT_SOURCE_DIR}" "${_header}")
    string(APPEND _editor_abi ";${_logical}:${_hash}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_header}")
endforeach()
string(SHA256 EDITOR_EXTENSION_ABI "${_editor_abi}")
set(_abi_file "${LUX_GENERATE_HEADER_DIR}/lux/engine/editor/extensions/EditorExtensionAbi.hpp")
file(CONFIGURE OUTPUT "${_abi_file}" CONTENT "#pragma once\nnamespace lux::editor::extensions { inline constexpr char kEditorExtensionAbi[] = \"@EDITOR_EXTENSION_ABI@\"; }\n" @ONLY)
add_component(COMPONENT_NAME editor_extensions NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/Contributions.cpp src/EditorExtension.cpp src/BuiltinContributions.cpp)
component_include_directories(editor_extensions BUILD_TIME_EXPORT
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR} INSTALL_TIME include)
target_link_libraries(editor_extensions PUBLIC lux::engine::editor::editor_commands
    lux::engine::editor::session_factories lux::engine::editor::view_host lux::engine::editor::scene_ui
    lux::engine::editor::scene_persistence lux::engine::editor::material_persistence
    lux::engine::editor::flowforge_persistence lux::engine::editor::material_ui lux::engine::editor::flow_ui
    lux::engine::project::project_plugins)
component_add_transitive_commands(editor_extensions
    "find_package(lux-engine-editor-commands REQUIRED COMPONENTS editor_commands)"
    "find_package(lux-engine-editor-session-factories REQUIRED COMPONENTS session_factories)"
    "find_package(lux-engine-editor-desktop REQUIRED COMPONENTS view_host)"
    "find_package(lux-engine-editor-scene-ui REQUIRED COMPONENTS scene_ui)"
    "find_package(lux-engine-editor-scene-persistence REQUIRED COMPONENTS scene_persistence)"
    "find_package(lux-engine-editor-material-persistence REQUIRED COMPONENTS material_persistence)"
    "find_package(lux-engine-editor-flowforge-persistence REQUIRED COMPONENTS flowforge_persistence)"
    "find_package(lux-engine-editor-material-ui REQUIRED COMPONENTS material_ui)"
    "find_package(lux-engine-editor-flow-ui REQUIRED COMPONENTS flow_ui)"
    "find_package(lux-engine-project-plugins REQUIRED COMPONENTS project_plugins)")
lux_classify_target(TARGET editor_extensions LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-extensions VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_extensions)
install(FILES "${_abi_file}" DESTINATION include/lux/engine/editor/extensions COMPONENT lux_engine_editor_extensions)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_contributions_test test/contributions.cpp)
    target_link_libraries(editor_contributions_test PRIVATE editor_extensions)
    lux_editor_test_options(editor_contributions_test)
    add_test(NAME editor.contributions COMMAND editor_contributions_test)
    foreach(scenario factory reflection cleanup notify)
        add_test(NAME editor.contributions.r11-${scenario} COMMAND editor_contributions_test ${scenario})
    endforeach()
    set_tests_properties(editor.contributions PROPERTIES LABELS "editor;native;P11")
endif()

add_subdirectory(builtin)
