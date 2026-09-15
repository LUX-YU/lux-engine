find_package(lux-engine-flowforge-compiler REQUIRED COMPONENTS flowforge_compiler)
find_package(lux-engine-toolchain-material REQUIRED COMPONENTS toolchain_material_compiler)
find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render scene_render_meta)
find_package(lux-engine-function REQUIRED COMPONENTS script_native render_client_meta)
find_package(lux-engine-simulation REQUIRED COMPONENTS transform_system)
find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS visual transform)
find_program(D2_FLOW_LINKER NAMES lld-link REQUIRED)
find_program(D2_POWERSHELL NAMES pwsh powershell REQUIRED)
file(TO_CMAKE_PATH "${D2_FLOW_LINKER}" D2_FLOW_LINKER_PATH)

set(protocols fixture core_protocol foundation_protocol process_completion publication_protocol review_regressions
    container_protocol material_protocol flow_object_protocol flow_protocol vfs_protocol model_protocol import_protocol)
foreach(protocol IN LISTS protocols)
    add_executable(d2_${protocol} ${protocol}.cpp)
    target_link_libraries(d2_${protocol} PRIVATE
        lux::engine::editor::editor_gui
        lux::engine::flowforge::flowforge_compiler
        lux::engine::toolchain::toolchain_material_compiler
        lux::engine::scene::scene_render lux::engine::scene::scene_render_meta
        lux::engine::function::script_native lux::engine::function::render_client_meta
        lux::engine::simulation::transform_system)
    target_compile_definitions(d2_${protocol} PRIVATE "D2_FLOW_LINKER=\"${D2_FLOW_LINKER_PATH}\"")
    target_compile_options(d2_${protocol} PRIVATE /UNDEBUG /utf-8 /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr)
endforeach()
target_link_libraries(d2_fixture PRIVATE lux::engine::simulation::ecs::visual lux::engine::simulation::ecs::transform)

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
foreach(mode material-gui flow-gui material-publish close-signal cpu)
    d2_case(${mode} d2_core_protocol)
endforeach()
