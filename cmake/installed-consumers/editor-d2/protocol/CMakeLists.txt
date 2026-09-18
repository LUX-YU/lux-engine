find_package(lux-engine-flowforge-compiler REQUIRED COMPONENTS flowforge_compiler)
find_package(lux-engine-toolchain-material REQUIRED COMPONENTS toolchain_material_compiler)
find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render scene_render_meta)
find_package(lux-engine-function REQUIRED COMPONENTS script_native render_client_meta)
find_package(lux-engine-simulation REQUIRED COMPONENTS transform_system)
find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS visual transform)
find_package(lux-engine-platform REQUIRED COMPONENTS window)
find_program(D2_FLOW_LINKER NAMES lld-link REQUIRED)
find_program(D2_POWERSHELL NAMES pwsh powershell REQUIRED)
file(TO_CMAKE_PATH "${D2_FLOW_LINKER}" D2_FLOW_LINKER_PATH)

# Feature tests have one maintained source next to their implementation. The
# consumer compiles staged test sources exclusively against installed packages.
set(D2_FEATURE_TEST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../engine/editor" CACHE PATH
    "Source root containing the feature test directories; no production includes are imported")
cmake_path(ABSOLUTE_PATH D2_FEATURE_TEST_ROOT NORMALIZE)
set(feature_test_sources "${CMAKE_CURRENT_BINARY_DIR}/feature-tests")
file(MAKE_DIRECTORY "${feature_test_sources}")
set(collected_test_names)
foreach(feature editors/material editors/flowforge editors/scene assets)
    file(GLOB sources CONFIGURE_DEPENDS "${D2_FEATURE_TEST_ROOT}/${feature}/test/*.cpp"
        "${D2_FEATURE_TEST_ROOT}/${feature}/test/*.hpp")
    if(NOT sources)
        message(FATAL_ERROR "Missing maintained feature tests: ${feature}")
    endif()
    foreach(source IN LISTS sources)
        get_filename_component(name "${source}" NAME)
        if(name IN_LIST collected_test_names)
            message(FATAL_ERROR "Duplicate maintained feature test: ${name}")
        endif()
        list(APPEND collected_test_names "${name}")
        configure_file("${source}" "${feature_test_sources}/${name}" COPYONLY)
    endforeach()
endforeach()

set(protocols fixture core_protocol render_terminal dynamic_run foundation_protocol process_completion publication_protocol publication_boundaries review_regressions
    container_protocol material_protocol flow_object_protocol flow_protocol vfs_protocol model_protocol import_protocol)
foreach(protocol IN LISTS protocols)
    if("${protocol}.cpp" IN_LIST collected_test_names)
        set(source "${feature_test_sources}/${protocol}.cpp")
    else()
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/${protocol}.cpp")
    endif()
    add_executable(d2_${protocol} "${source}")
    target_include_directories(d2_${protocol} PRIVATE "${feature_test_sources}" "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(d2_${protocol} PRIVATE
        lux::engine::editor::editor_scene_ui lux::engine::editor::editor_material_ui
        lux::engine::editor::editor_flowforge_ui lux::engine::editor::editor_assets
        lux::engine::flowforge::flowforge_compiler
        lux::engine::toolchain::toolchain_material_compiler
        lux::engine::scene::scene_render lux::engine::scene::scene_render_meta
        lux::engine::function::script_native lux::engine::function::render_client_meta
        lux::engine::simulation::transform_system)
    target_compile_definitions(d2_${protocol} PRIVATE "D2_FLOW_LINKER=\"${D2_FLOW_LINKER_PATH}\"")
    target_compile_options(d2_${protocol} PRIVATE /UNDEBUG /utf-8 /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr)
endforeach()
target_link_libraries(d2_fixture PRIVATE lux::engine::simulation::ecs::visual lux::engine::simulation::ecs::transform)
target_link_libraries(d2_render_terminal PRIVATE lux::engine::platform::window)
target_link_libraries(d2_dynamic_run PRIVATE lux::engine::platform::window)

foreach(mode pending forwarded)
    add_test(NAME editor.d3.terminal-${mode} COMMAND d2_render_terminal ${mode})
    set_tests_properties(editor.d3.terminal-${mode} PROPERTIES RUN_SERIAL TRUE TIMEOUT 30)
endforeach()

add_test(NAME editor.d2c.publication-format COMMAND d2_publication_boundaries
    "${CMAKE_CURRENT_BINARY_DIR}/publication-format" ordinary)
option(D2_PUBLICATION_DIAGNOSTICS "Run against a dedicated SDK with publication boundary probes" OFF)
if(D2_PUBLICATION_DIAGNOSTICS)
    foreach(boundary preparing:0 staged:0 staged:1 staged:2 prepared:0 published:0 published:1 published:2
        committed:0 cleanup-record:0 cleanup-record:1 cleanup-record:2 cleanup-journal:0)
        string(REPLACE ":" "-" name "${boundary}")
        add_test(NAME editor.d2c.publication-boundary-${name}
            COMMAND "${D2_POWERSHELL}" -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/RunPublicationBoundary.ps1"
                -Executable "$<TARGET_FILE:d2_publication_boundaries>"
                -Root "${CMAKE_CURRENT_BINARY_DIR}/publication-boundaries" -Boundary "${boundary}")
        set_tests_properties(editor.d2c.publication-boundary-${name} PROPERTIES RUN_SERIAL TRUE TIMEOUT 30)
    endforeach()
    add_test(NAME editor.d2c.publication-boundary-conflict
        COMMAND "${D2_POWERSHELL}" -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/RunPublicationBoundary.ps1"
            -Executable "$<TARGET_FILE:d2_publication_boundaries>"
            -Root "${CMAKE_CURRENT_BINARY_DIR}/publication-boundaries" -Boundary "published:1" -Conflict)
endif()

foreach(mode compile-notice close-intent exit-review)
    add_test(NAME editor.d2c.${mode}
        COMMAND d2_review_regressions "${mode}" "${CMAKE_CURRENT_BINARY_DIR}/review-runs")
    set_tests_properties(editor.d2c.${mode} PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
endforeach()

function(d2_case name executable)
    add_test(NAME editor.d2.${name}
        COMMAND "${D2_POWERSHELL}" -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/RunCase.ps1"
            -Case "${name}" -Executable "$<TARGET_FILE:${executable}>"
            -Fixture "$<TARGET_FILE:d2_fixture>" -Root "${CMAKE_CURRENT_BINARY_DIR}/runs"
            -MaterialFixture "$<TARGET_FILE:d2_material_protocol>" -FlowFixture "$<TARGET_FILE:d2_flow_protocol>"
            -Seed "${CMAKE_CURRENT_SOURCE_DIR}/Seed.luxpak")
    set_tests_properties(editor.d2.${name} PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
endfunction()

foreach(protocol foundation_protocol process_completion container_protocol flow_object_protocol vfs_protocol)
    d2_case(${protocol} d2_${protocol})
endforeach()
foreach(protocol material_protocol flow_protocol model_protocol import_protocol)
    d2_case(${protocol} d2_${protocol})
endforeach()
foreach(mode recovery cancel conflict crash)
    d2_case(publication-${mode} d2_publication_protocol)
endforeach()
foreach(mode save save-retry save-partial save-model save-hierarchy save-preservation save-structure save-import-model attach-rollback)
    d2_case(${mode} d2_core_protocol)
endforeach()
foreach(mode material-gui flow-gui material-publish close-signal cpu pane-lifecycle fixed-run fixed-run-failure render-association render-thread transform-sync)
    d2_case(${mode} d2_core_protocol)
endforeach()
foreach(mode dynamic-run simulation-failure dynamic-terminal closing-terminal failure-closing-terminal)
    d2_case(${mode} d2_dynamic_run)
endforeach()
