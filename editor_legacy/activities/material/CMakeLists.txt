add_component(COMPONENT_NAME material_persistence_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/MaterialSaveSource.cpp src/MaterialCodec.cpp src/SessionFactory.cpp)
component_include_directories(material_persistence_legacy
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(material_persistence_legacy PUBLIC lux::engine::editor::material_model_legacy lux::engine::editor::editor_persistence_legacy)
component_add_transitive_commands(material_persistence_legacy
    "find_package(lux-engine-editor-material-model REQUIRED COMPONENTS material_model_legacy)"
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence_legacy)")
target_link_libraries(material_persistence_legacy PUBLIC lux::engine::editor::session_factories_legacy PRIVATE lux::engine::core::services)
target_include_directories(material_persistence_legacy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sessions/sinclude)
component_add_transitive_commands(material_persistence_legacy
    "find_package(lux-engine-editor-session-factories REQUIRED COMPONENTS session_factories_legacy)"
    "find_package(lux-engine-core REQUIRED COMPONENTS services)")
lux_classify_target(TARGET material_persistence_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-material-persistence VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS material_persistence_legacy)

add_component(COMPONENT_NAME material_preview_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/PublishCompiledMaterial.cpp src/MaterialCompilation.cpp src/MaterialCompilationService.cpp src/MaterialPreview.cpp src/MaterialPreviewRecipe.cpp)
target_include_directories(material_preview_legacy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)
component_include_directories(material_preview_legacy BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(material_preview_legacy PUBLIC lux::engine::editor::editor_persistence_legacy lux::engine::editor::material_model_legacy
    lux::engine::toolchain::toolchain_material_compiler lux::engine::process::process_execution
    lux::engine::resource::asset lux::engine::editor::scene_projection_legacy lux::engine::core::services
    PRIVATE lux::engine::scene::scene_transform lux::engine::world::world_storage lux::engine::process::asset_loading)
component_add_transitive_commands(material_preview_legacy
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence_legacy)"
    "find_package(lux-engine-editor-material-model REQUIRED COMPONENTS material_model_legacy)"
    "find_package(lux-engine-toolchain-material REQUIRED COMPONENTS toolchain_material_compiler)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-editor-scene-projection REQUIRED COMPONENTS scene_projection_legacy)"
    "find_package(lux-engine-core REQUIRED COMPONENTS services)"
    "find_package(lux-engine-scene-transform REQUIRED COMPONENTS scene_transform)"
    "find_package(lux-engine-world-storage REQUIRED COMPONENTS world_storage)"
    "find_package(lux-engine-process-asset-loading REQUIRED COMPONENTS process_asset_loading)")
lux_classify_target(TARGET material_preview_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-material-preview VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS material_preview_legacy)


# Exact SDK ownership: sibling providers share a build include root, never an install manifest.
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/MaterialCodec.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/MaterialSaveSource.hpp
    DESTINATION include/lux/engine/editor/material COMPONENT lux_engine_editor_material_persistence)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/MaterialCompilation.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/MaterialCompilationService.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/MaterialPreview.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/PublishCompiledMaterial.hpp
    DESTINATION include/lux/engine/editor/material COMPONENT lux_engine_editor_material_preview)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/MaterialSessionFactory.hpp
    DESTINATION include/lux/engine/editor/material COMPONENT lux_engine_editor_material_persistence)
