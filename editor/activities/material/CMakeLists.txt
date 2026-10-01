add_component(COMPONENT_NAME material_persistence NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/MaterialSaveSource.cpp src/MaterialCodec.cpp src/SessionFactory.cpp)
component_include_directories(material_persistence
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(material_persistence PUBLIC lux::engine::editor::material_model lux::engine::editor::editor_persistence)
component_add_transitive_commands(material_persistence
    "find_package(lux-engine-editor-material-model REQUIRED COMPONENTS material_model)"
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence)")
lux_classify_target(TARGET material_persistence LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-material-persistence VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS material_persistence)

add_component(COMPONENT_NAME material_preview NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/PublishCompiledMaterial.cpp src/MaterialCompilation.cpp src/MaterialCompilationService.cpp src/MaterialPreviewStore.cpp)
component_include_directories(material_preview BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
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


# Exact SDK ownership: sibling providers share a build include root, never an install manifest.
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/MaterialCodec.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/MaterialSaveSource.hpp
    DESTINATION include/lux/engine/editor/material COMPONENT lux_engine_editor_material_persistence)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/MaterialCompilation.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/MaterialCompilationService.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/MaterialPreviewStore.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/PublishCompiledMaterial.hpp
    DESTINATION include/lux/engine/editor/material COMPONENT lux_engine_editor_material_preview)

target_link_libraries(material_persistence PUBLIC lux::engine::editor::session_factories)
target_include_directories(material_persistence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sessions/sinclude)
component_add_transitive_commands(material_persistence "find_package(lux-engine-editor-session-factories REQUIRED COMPONENTS session_factories)")
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/MaterialSessionFactory.hpp
    DESTINATION include/lux/engine/editor/material COMPONENT lux_engine_editor_material_persistence)
