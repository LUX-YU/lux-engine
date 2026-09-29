add_component(COMPONENT_NAME material_preview NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/PublishCompiledMaterial.cpp src/MaterialCompilation.cpp src/MaterialPreviewStore.cpp)
component_include_directories(material_preview BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(material_preview PUBLIC lux::engine::editor::editor_persistence lux::engine::editor::material_model
    lux::engine::toolchain::toolchain_material_compiler lux::engine::process::process_execution
    lux::engine::resource::asset lux::engine::editor::scene_projection
    PRIVATE lux::engine::scene::scene_transform lux::engine::world::world_storage lux::engine::process::asset_loading)
component_add_transitive_commands(material_preview
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence)"
    "find_package(lux-engine-editor-material-model REQUIRED COMPONENTS material_model)"
    "find_package(lux-engine-toolchain-material REQUIRED COMPONENTS toolchain_material_compiler)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-editor-scene-projection REQUIRED COMPONENTS scene_projection)"
    "find_package(lux-engine-scene-transform REQUIRED COMPONENTS scene_transform)"
    "find_package(lux-engine-world-storage REQUIRED COMPONENTS world_storage)"
    "find_package(lux-engine-process-asset-loading REQUIRED COMPONENTS process_asset_loading)")
lux_classify_target(TARGET material_preview LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-material-preview VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS material_preview)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_compilation_p07_test test/compilation.cpp)
    target_link_libraries(editor_compilation_p07_test PRIVATE material_preview flowforge_compilation project_io)
    lux_editor_test_options(editor_compilation_p07_test)
    find_program(P07_LINKER NAMES lld-link REQUIRED)
    add_test(NAME editor.compilation.actual_publish COMMAND editor_compilation_p07_test "${P07_LINKER}" "${CMAKE_CURRENT_BINARY_DIR}/publication")
    set_tests_properties(editor.compilation.actual_publish PROPERTIES LABELS "editor;native;P07;toolchain" TIMEOUT 180)
endif()
