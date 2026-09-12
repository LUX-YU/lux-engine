add_library(editor_application STATIC src/Application.cpp)
target_include_directories(editor_application PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sinclude)
target_link_libraries(editor_application PUBLIC lux::engine::editor::editor_scene_ui
    lux::engine::editor::editor_tooling PRIVATE lux::engine::platform::window)
lux_classify_target(TARGET editor_application LAYER EDITOR PRODUCT EDITOR ROLE COMPOSITION)

set(LUX_EDITOR_SEED_PAK "" CACHE FILEPATH "SV-1 asset package produced by lux_scene_seed")
if(LUX_EDITOR_SEED_PAK)
    if(NOT EXISTS "${LUX_EDITOR_SEED_PAK}")
        message(FATAL_ERROR "LUX_EDITOR_SEED_PAK does not exist: ${LUX_EDITOR_SEED_PAK}")
    endif()
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/share/lux-engine/editor)
    configure_file(${LUX_EDITOR_SEED_PAK} ${CMAKE_BINARY_DIR}/share/lux-engine/editor/sv1.luxpak COPYONLY)
    install(FILES ${LUX_EDITOR_SEED_PAK} DESTINATION share/lux-engine/editor RENAME sv1.luxpak)
endif()

add_executable(lux_editor app/main.cpp ../examples/scene_workbench/DevelopmentScene.cpp)
target_include_directories(lux_editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../examples/scene_workbench)
target_link_libraries(lux_editor PRIVATE editor_application
    lux::engine::simulation::transform_system lux::engine::scene::scene_render_meta
    lux::engine::function::render_client_meta lux::engine::simulation::ecs::hierarchy)
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_demo_transform_meta
    TARGET_FILES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/builtin/transform/include/lux/engine/simulation/TransformSystem.hpp
    REGISTER_FUNC_NAME lux_editor_demo_transform)
engine_target_add_type_runtime_info(TARGET lux_editor METAS editor_demo_transform_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
lux_classify_target(TARGET lux_editor LAYER EDITOR PRODUCT EDITOR ROLE COMPOSITION)


install(TARGETS lux_editor RUNTIME DESTINATION bin COMPONENT lux_editor)
if(WIN32)
    install(IMPORTED_RUNTIME_ARTIFACTS imgui::core RUNTIME DESTINATION bin COMPONENT lux_editor)
endif()
