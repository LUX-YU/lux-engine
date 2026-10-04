# Editor ABI evolves independently from the engine runtime plugin identity.
set(_editor_abi "v10;${_lux_plugin_environment}")
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
    SOURCE_FILES src/Contributions.cpp src/EditorExtension.cpp)
component_include_directories(editor_extensions BUILD_TIME_EXPORT
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR} INSTALL_TIME include)
target_link_libraries(editor_extensions PUBLIC lux::engine::editor::editor_commands
    lux::engine::editor::editor_configuration
    lux::engine::editor::editor_composition
    lux::engine::editor::session_factories lux::engine::editor::view_host
    lux::engine::project::project_plugins)
component_add_transitive_commands(editor_extensions
    "find_package(lux-engine-editor-configuration REQUIRED COMPONENTS editor_configuration)"
    "find_package(lux-engine-editor-composition REQUIRED COMPONENTS editor_composition)"
    "find_package(lux-engine-editor-commands REQUIRED COMPONENTS editor_commands)"
    "find_package(lux-engine-editor-session-factories REQUIRED COMPONENTS session_factories)"
    "find_package(lux-engine-editor-desktop REQUIRED COMPONENTS view_host)"
    "find_package(lux-engine-project-plugins REQUIRED COMPONENTS project_plugins)")
lux_classify_target(TARGET editor_extensions LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)

# The selected-module index is produced from the original MetaUnit, never from a CMake symbol list.
if(CMAKE_CROSSCOMPILING)
    if(NOT LUX_EDITOR_MODULE_GENERATOR_EXECUTABLE)
        message(FATAL_ERROR "Cross builds require an explicit host LUX_EDITOR_MODULE_GENERATOR_EXECUTABLE")
    endif()
else()
    find_package(inja CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
    add_executable(lux_editor_module_generator codegen/ModuleGenerator.cpp)
    target_link_libraries(lux_editor_module_generator PRIVATE
        lux::cxx::reflection_runtime pantor::inja nlohmann_json::nlohmann_json)
    lux_classify_target(TARGET lux_editor_module_generator LAYER EDITOR PRODUCT TOOLCHAIN ROLE GENERATOR)
    if(WIN32)
        add_custom_command(TARGET lux_editor_module_generator POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:lux_editor_module_generator>
                $<TARGET_FILE_DIR:lux_editor_module_generator> COMMAND_EXPAND_LISTS VERBATIM)
    endif()
    install(TARGETS lux_editor_module_generator RUNTIME DESTINATION bin COMPONENT lux_editor)
    install(FILES $<TARGET_RUNTIME_DLLS:lux_editor_module_generator> DESTINATION bin COMPONENT lux_editor)
endif()
include_component_cmake_scripts(meta)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_editor_modules.cmake)
component_add_cmake_scripts(editor_extensions ${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_editor_modules.cmake)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/codegen/templates
    DESTINATION share/lux-engine-editor-extensions/editor_extensions/cmake_scripts/codegen)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/codegen/ir.template ${CMAKE_CURRENT_SOURCE_DIR}/codegen/GenerateModules.cmake
    DESTINATION share/lux-engine-editor-extensions/editor_extensions/cmake_scripts/codegen)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-extensions VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_extensions)
install(FILES "${_abi_file}" DESTINATION include/lux/engine/editor/extensions COMPONENT lux_engine_editor_extensions)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_contributions_test test/contributions.cpp)
    target_link_libraries(editor_contributions_test PRIVATE editor_extensions scene_control_api)
    lux_editor_test_options(editor_contributions_test)
    add_test(NAME editor.contributions COMMAND editor_contributions_test)
    foreach(scenario factory reflection cleanup notify)
        add_test(NAME editor.contributions.r11-${scenario} COMMAND editor_contributions_test ${scenario})
    endforeach()
    set_tests_properties(editor.contributions PROPERTIES LABELS "editor;native;P11")

    # One actual implementation is compiled both as an archive and a plugin. The foundation fixture
    # owns its original Process task and two UI factories; no isolated replacement registry is used.
    set(_composition_fixture "${PROJECT_SOURCE_DIR}/editor/workbench/desktop/test")
    foreach(kind static dynamic)
        if(kind STREQUAL "static")
            add_library(editor_module_${kind} STATIC test/ModuleFixture.cpp "${_composition_fixture}/composition_plugin.cpp")
        else()
            add_library(editor_module_${kind} MODULE test/ModuleFixture.cpp test/ModuleExports.cpp
                "${_composition_fixture}/composition_plugin.cpp")
        endif()
        target_include_directories(editor_module_${kind} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/test"
            PRIVATE "${_composition_fixture}")
        target_link_libraries(editor_module_${kind} PUBLIC editor_extensions PRIVATE lux::engine::process::process_execution)
        target_compile_definitions(editor_module_${kind} PRIVATE "MODULE_RUNTIME_ABI=\"${LUX_PLUGIN_SDK_ABI}\"")
        lux_editor_test_options(editor_module_${kind})
    endforeach()
    engine_declare_editor_module(TARGET editor_module_static HEADER "${CMAKE_CURRENT_SOURCE_DIR}/test/ModuleFixture.hpp"
        LOGICAL_PATH ModuleFixture.hpp SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/test/ModuleFixture.cpp")
    add_executable(editor_modules_test test/modules.cpp)
    target_include_directories(editor_modules_test PRIVATE "${_composition_fixture}")
    target_link_libraries(editor_modules_test PRIVATE editor_extensions lux::engine::process::process_execution)
    lux_editor_test_options(editor_modules_test)
    engine_target_add_editor_modules(TARGET editor_modules_test NAME selectedModules NAMESPACE module_fixture
        MODULES editor_module_static)
    add_test(NAME editor.modules COMMAND editor_modules_test $<TARGET_FILE:editor_module_dynamic>)
    set_tests_properties(editor.modules PROPERTIES LABELS "editor;native;EC4" TIMEOUT 60 RUN_SERIAL TRUE)
    add_test(NAME editor.module_codegen COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/module_codegen.py"
        "$<TARGET_FILE:lux_editor_module_generator>" "${CMAKE_CURRENT_SOURCE_DIR}/codegen/templates")
    set_tests_properties(editor.module_codegen PROPERTIES LABELS "editor;native;EC4")
endif()
