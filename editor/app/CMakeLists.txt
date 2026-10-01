find_package(lux-cxx REQUIRED COMPONENTS arguments)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_APP_LIBRARY PUBLIC_MACRO_NAME LUX_EDITOR_APP_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_EDITOR_APP_DLL_DISABLE GENERATE_FILE_PATH lux/engine/editor/app/visibility.h)
add_component(COMPONENT_NAME editor_app STATIC NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_app
    SOURCE_FILES src/Editor.cpp src/EditorStartup.cpp src/EditorLoop.cpp src/EditorPlatform.cpp src/EditorMenu.cpp src/EditorWorkspace.cpp src/EditorWorkspaceStorage.cpp)
component_include_directories(editor_app BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${PROJECT_SOURCE_DIR}/editor/assets/sinclude ${PROJECT_SOURCE_DIR}/editor/activities/project/sinclude ${PROJECT_SOURCE_DIR}/editor/activities/sinclude ${PROJECT_SOURCE_DIR}/editor/context/sinclude INSTALL_TIME include)
target_compile_definitions(editor_app PRIVATE LUX_EDITOR_APP_LIBRARY)
lux_classify_target(TARGET editor_app LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
target_link_libraries(editor_app PUBLIC lux::engine::engine_context_render lux::engine::editor::editor_context lux::engine::editor::editor_assets lux::engine::editor::editor_ui
    PRIVATE lux::engine::function::input
        lux::engine::platform::window lux::engine::editor::editor_metadata)
component_add_transitive_commands(editor_app
    "find_package(lux-engine-context-render REQUIRED COMPONENTS engine_context_render)"
    "find_package(lux-engine-editor-context REQUIRED COMPONENTS editor_context)"
    "find_package(lux-engine-editor-assets REQUIRED COMPONENTS editor_assets)"
    "find_package(lux-engine-editor-ui REQUIRED COMPONENTS editor_ui)")
component_add_transitive_commands(editor_app
    "find_package(lux-engine-function REQUIRED COMPONENTS input)"
    "find_package(lux-engine-platform REQUIRED COMPONENTS window)"
    "find_package(lux-engine-editor-metadata REQUIRED COMPONENTS editor_metadata)")
lux_engine_install_components(PROJECT_NAME lux-engine-editor-app VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_app)
add_executable(lux_editor main.cpp product/ProductAssembly.cpp product/ProductCommands.cpp)
lux_classify_target(TARGET lux_editor LAYER EDITOR PRODUCT EDITOR ROLE EXECUTABLE)
target_link_libraries(lux_editor PRIVATE lux::engine::platform::platform_process lux::engine::editor::editor_app lux::engine::editor::editor_assets
    lux::engine::platform::window
    lux::engine::editor::editor_scene lux::engine::editor::editor_material
    lux::engine::editor::editor_flowforge lux::engine::editor::editor_project_tools
    lux::engine::editor::editor_settings lux::engine::editor::editor_launcher lux::engine::editor::editor_launch lux::cxx::arguments)
install(TARGETS lux_editor RUNTIME DESTINATION bin)

if(LUX_EDITOR_TEST_DESKTOP_GPU)
    target_sources(editor_app PRIVATE src/EditorTestAccess.cpp)
    target_compile_definitions(editor_app PRIVATE LUX_EDITOR_BASELINE_PROBES)
    add_executable(editor_baseline_failures test/baseline_failures.cpp)
    target_include_directories(editor_baseline_failures PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
        ${PROJECT_SOURCE_DIR}/editor/context/sinclude ${PROJECT_SOURCE_DIR}/editor/assets/sinclude
        ${PROJECT_SOURCE_DIR}/editor/activities/project/sinclude ${PROJECT_SOURCE_DIR}/editor/activities/sinclude)
    target_link_libraries(editor_baseline_failures PRIVATE editor_app)
    lux_editor_test_options(editor_baseline_failures)
    add_executable(editor_context_lifecycle test/context_lifecycle.cpp)
    target_link_libraries(editor_context_lifecycle PRIVATE editor_app editor_launcher)
    if(MSVC)
        target_compile_options(editor_context_lifecycle PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(editor_context_lifecycle PRIVATE -UNDEBUG)
    endif()
    add_test(NAME editor.context_lifecycle COMMAND editor_context_lifecycle
        "${CMAKE_BINARY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/context-runs")
    set_tests_properties(editor.context_lifecycle PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
endif()

if(LUX_EDITOR_TEST_DESKTOP_GPU AND LUX_EDITOR_BUILD_TOOLCHAIN_TESTS)
    lux_editor_find_linker(EDITOR_TEST_LINKER)
    foreach(kind material flow)
        if(kind STREQUAL "material")
            set(package material)
        else()
            set(package flowforge)
        endif()
        add_executable(editor_${kind}_protocol ../tools/${package}/test/${kind}_protocol.cpp)
        target_include_directories(editor_${kind}_protocol PRIVATE ${PROJECT_SOURCE_DIR}/cmake/installed-consumers/editor-d2)
        target_link_libraries(editor_${kind}_protocol PRIVATE editor_app editor_${package} editor_scene editor_material editor_flowforge
            lux::engine::function::script_native lux::engine::flowforge::flowforge_compiler
            lux::engine::toolchain::toolchain_material_compiler)
        target_compile_definitions(editor_${kind}_protocol PRIVATE "D2_FLOW_LINKER=\"${EDITOR_TEST_LINKER}\"")
        if(MSVC)
            target_compile_options(editor_${kind}_protocol PRIVATE /UNDEBUG /utf-8)
        else()
            target_compile_options(editor_${kind}_protocol PRIVATE -UNDEBUG)
        endif()
    endforeach()
endif()

if(LUX_EDITOR_TEST_DESKTOP_GPU)
    add_executable(editor_review_regressions ${PROJECT_SOURCE_DIR}/cmake/installed-consumers/editor-d2/protocol/review_regressions.cpp)
    target_include_directories(editor_review_regressions PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
        ${PROJECT_SOURCE_DIR}/editor/context/sinclude ${PROJECT_SOURCE_DIR}/editor/assets/sinclude ${PROJECT_SOURCE_DIR}/editor/activities/project/sinclude ${PROJECT_SOURCE_DIR}/editor/activities/sinclude)
    target_link_libraries(editor_review_regressions PRIVATE editor_app editor_material editor_scene editor_flowforge)
    if(MSVC)
        target_compile_options(editor_review_regressions PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(editor_review_regressions PRIVATE -UNDEBUG)
    endif()
    foreach(mode exit-review)
        add_test(NAME editor.unified_${mode} COMMAND editor_review_regressions ${mode} ${CMAKE_CURRENT_BINARY_DIR}/review-runs)
        set_tests_properties(editor.unified_${mode} PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
    endforeach()
endif()

if(LUX_EDITOR_TEST_DESKTOP_GPU)
    add_executable(editor_core_protocol ../tools/scene/test/core_protocol.cpp)
    target_include_directories(editor_core_protocol PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${PROJECT_SOURCE_DIR}/editor/assets/sinclude ${PROJECT_SOURCE_DIR}/editor/activities/project/sinclude ${PROJECT_SOURCE_DIR}/editor/activities/sinclude
        ${PROJECT_SOURCE_DIR}/editor/context/include ${PROJECT_SOURCE_DIR}/editor/context/sinclude
        ${PROJECT_SOURCE_DIR}/cmake/installed-consumers/editor-d2
        ${PROJECT_SOURCE_DIR}/cmake/installed-consumers/common
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/flowforge/test)
    target_link_libraries(editor_core_protocol PRIVATE editor_app editor_scene editor_material editor_flowforge
        lux::engine::function::render_features lux::engine::editor::render_feature_meta
        lux::engine::editor::scene_render_meta lux::engine::scene::scene_transform
        lux::engine::function::script_native lux::engine::flowforge::flowforge_compiler
        lux::engine::toolchain::toolchain_material_compiler)
    if(LUX_EDITOR_BUILD_TOOLCHAIN_TESTS)
        target_compile_definitions(editor_core_protocol PRIVATE "D2_FLOW_LINKER=\"${EDITOR_TEST_LINKER}\"")
    endif()
    if(MSVC)
        target_compile_options(editor_core_protocol PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(editor_core_protocol PRIVATE -UNDEBUG)
    endif()
endif()

if(LUX_EDITOR_TEST_DESKTOP_GPU)
    find_program(EDITOR_TEST_POWERSHELL NAMES pwsh powershell REQUIRED)
    set(core_modes new-assets save-preservation save-retry save-partial factory-rollback fixed-run)
    set(material_fixture "not-selected")
    set(flow_fixture "not-selected")
    if(LUX_EDITOR_BUILD_TOOLCHAIN_TESTS)
        list(APPEND core_modes material-gui flow-gui)
        set(material_fixture "$<TARGET_FILE:editor_material_protocol>")
        set(flow_fixture "$<TARGET_FILE:editor_flow_protocol>")
    endif()
    foreach(mode IN LISTS core_modes)
        add_test(NAME editor.unified_core_${mode}
            COMMAND "${EDITOR_TEST_POWERSHELL}" -NoProfile -File
                "${PROJECT_SOURCE_DIR}/cmake/installed-consumers/editor-d2/protocol/RunCase.ps1"
                -Case "${mode}" -Executable "$<TARGET_FILE:editor_core_protocol>"
                -Fixture "$<TARGET_FILE:editor_scene_fixture>" -MaterialFixture "${material_fixture}"
                -FlowFixture "${flow_fixture}" -Root "${CMAKE_BINARY_DIR}/tests/core"
                -Seed "${PROJECT_SOURCE_DIR}/cmake/installed-consumers/editor-d2/protocol/Seed.luxpak")
        set_tests_properties(editor.unified_core_${mode} PROPERTIES RUN_SERIAL TRUE TIMEOUT 100)
    endforeach()
endif()

if(LUX_EDITOR_TEST_INSTALLED_UI)
    # Use the exact installed component sources and generation rules during development.
    set(D2_CONSUMER_SOURCE_DIR "${PROJECT_SOURCE_DIR}/cmake/installed-consumers/editor-d2")
    include("${D2_CONSUMER_SOURCE_DIR}/Targets.cmake")
    add_test(NAME editor.component_elements COMMAND consumer_protocol "${CMAKE_CURRENT_BINARY_DIR}/component-project")
    set_tests_properties(editor.component_elements PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
endif()

foreach(target editor_core_protocol consumer_protocol editor_material_protocol editor_flow_protocol editor_review_regressions)
    if(TARGET ${target})
        target_compile_definitions(${target} PRIVATE "LUX_TEST_PLUGIN_ROOT=\"${CMAKE_BINARY_DIR}\"")
        add_dependencies(${target} builtin_runtime_plugin lux_plugin_capabilities)
        target_link_libraries(${target} PRIVATE editor_project_tools editor_settings editor_launcher)
        target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/editor/tools/test)
    endif()
endforeach()

find_package(tomlplusplus CONFIG REQUIRED)
target_include_directories(editor_app PRIVATE $<TARGET_PROPERTY:tomlplusplus::tomlplusplus,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(editor_app PRIVATE TOML_HEADER_ONLY=1 TOML_EXCEPTIONS=0)

target_include_directories(lux_editor PRIVATE ${PROJECT_SOURCE_DIR}/editor/activities/sinclude $<TARGET_PROPERTY:tomlplusplus::tomlplusplus,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(lux_editor PRIVATE TOML_HEADER_ONLY=1 TOML_EXCEPTIONS=0 "LUX_EDITOR_VERSION=\"${PROJECT_VERSION}\"")

get_property(editor_app_tests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS editor_app_tests)
    set(labels "editor;desktop;gpu")
    if(test MATCHES "(material-gui|flow-gui)$")
        list(APPEND labels toolchain)
    endif()
    if(test STREQUAL "editor.component_elements")
        list(APPEND labels installed)
    endif()
    set_tests_properties(${test} PROPERTIES LABELS "${labels}")
endforeach()

target_compile_definitions(editor_app PUBLIC LUX_EDITOR_APP_DLL_DISABLE)
