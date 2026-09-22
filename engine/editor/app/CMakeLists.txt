find_package(lux-cxx REQUIRED COMPONENTS arguments)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_APP_LIBRARY PUBLIC_MACRO_NAME LUX_EDITOR_APP_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_EDITOR_APP_DLL_DISABLE GENERATE_FILE_PATH lux/engine/editor/app/visibility.h)
add_component(COMPONENT_NAME editor_app NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_app
    SOURCE_FILES src/Editor.cpp src/EditorStartup.cpp src/EditorLoop.cpp src/EditorPlatform.cpp src/ProjectPane.cpp)
component_include_directories(editor_app BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude INSTALL_TIME include)
target_compile_definitions(editor_app PRIVATE LUX_EDITOR_APP_LIBRARY)
target_link_libraries(editor_app PUBLIC lux::engine::editor::editor_core lux::engine::editor::editor_ui
    PRIVATE lux::engine::platform::window)
component_add_transitive_commands(editor_app
    "find_package(lux-engine-editor-core REQUIRED COMPONENTS editor_core)"
    "find_package(lux-engine-editor-ui REQUIRED COMPONENTS editor_ui)")
if(WIN32)
    target_link_libraries(editor_app PRIVATE comdlg32 imm32)
endif()
lux_engine_install_components(PROJECT_NAME lux-engine-editor-app VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_app)
add_executable(lux_editor main.cpp)
target_link_libraries(lux_editor PRIVATE lux::engine::editor::editor_app lux::engine::editor::editor_assets
    lux::engine::editor::editor_scene_ui lux::engine::editor::editor_material_ui
    lux::engine::editor::editor_flowforge_ui lux::engine::function::render_features lux::cxx::arguments)
if(WIN32)
    target_link_libraries(lux_editor PRIVATE comdlg32)
endif()
install(TARGETS lux_editor RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    find_program(EDITOR_TEST_LINKER NAMES lld-link HINTS "D:/Development/Mircosoft/VisualStudio/VC/Tools/Llvm/x64/bin" REQUIRED)
    foreach(kind material flow)
        if(kind STREQUAL "material")
            set(package material)
        else()
            set(package flowforge)
        endif()
        add_executable(editor_${kind}_protocol ../editors/${package}/test/${kind}_protocol.cpp)
        target_include_directories(editor_${kind}_protocol PRIVATE ${PROJECT_SOURCE_DIR}/cmake/installed-consumers/editor-d2)
        target_link_libraries(editor_${kind}_protocol PRIVATE editor_app editor_${package}
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

if(BUILD_TESTING)
    add_executable(editor_review_regressions ${PROJECT_SOURCE_DIR}/cmake/installed-consumers/editor-d2/protocol/review_regressions.cpp)
    target_link_libraries(editor_review_regressions PRIVATE editor_app editor_material)
    if(MSVC)
        target_compile_options(editor_review_regressions PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(editor_review_regressions PRIVATE -UNDEBUG)
    endif()
    foreach(mode compile-notice close-intent exit-review)
        add_test(NAME editor.unified_${mode} COMMAND editor_review_regressions ${mode} ${CMAKE_CURRENT_BINARY_DIR}/review-runs)
        set_tests_properties(editor.unified_${mode} PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
    endforeach()
endif()

if(BUILD_TESTING)
    add_executable(editor_core_protocol ../editors/scene/test/core_protocol.cpp)
    target_include_directories(editor_core_protocol PRIVATE
        ${PROJECT_SOURCE_DIR}/cmake/installed-consumers/editor-d2
        ${CMAKE_CURRENT_SOURCE_DIR}/../editors/flowforge/test)
    target_link_libraries(editor_core_protocol PRIVATE editor_app editor_scene_ui editor_material_ui editor_flowforge_ui
        lux::engine::function::render_features lux::engine::function::render_feature_meta
        lux::engine::scene::scene_render_meta lux::engine::simulation::transform_system
        lux::engine::function::script_native lux::engine::flowforge::flowforge_compiler
        lux::engine::toolchain::toolchain_material_compiler)
    target_compile_definitions(editor_core_protocol PRIVATE "D2_FLOW_LINKER=\"${EDITOR_TEST_LINKER}\"")
    if(MSVC)
        target_compile_options(editor_core_protocol PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(editor_core_protocol PRIVATE -UNDEBUG)
    endif()
endif()

if(BUILD_TESTING)
    find_program(EDITOR_TEST_POWERSHELL NAMES pwsh powershell REQUIRED)
    foreach(mode pane-lifecycle save save-spatial save-model save-hierarchy save-preservation save-structure
                 save-import-model attach-rollback close-signal cpu render-association render-thread transform-sync
                 material-gui flow-gui material-publish fixed-run fixed-run-failure resource-sharing)
        add_test(NAME editor.unified_core_${mode}
            COMMAND "${EDITOR_TEST_POWERSHELL}" -NoProfile -File
                "${PROJECT_SOURCE_DIR}/cmake/installed-consumers/editor-d2/protocol/RunCase.ps1"
                -Case "${mode}" -Executable "$<TARGET_FILE:editor_core_protocol>"
                -Fixture "$<TARGET_FILE:editor_scene_fixture>" -MaterialFixture "$<TARGET_FILE:editor_material_protocol>"
                -FlowFixture "$<TARGET_FILE:editor_flow_protocol>" -Root "${CMAKE_CURRENT_BINARY_DIR}/core-runs"
                -Seed "${PROJECT_SOURCE_DIR}/cmake/installed-consumers/editor-d2/protocol/Seed.luxpak")
        set_tests_properties(editor.unified_core_${mode} PROPERTIES RUN_SERIAL TRUE TIMEOUT 100)
    endforeach()
endif()

if(BUILD_TESTING)
    # Use the exact installed component sources and generation rules during development.
    set(D2_CONSUMER_SOURCE_DIR "${PROJECT_SOURCE_DIR}/cmake/installed-consumers/editor-d2")
    include("${D2_CONSUMER_SOURCE_DIR}/Targets.cmake")
endif()
