add_executable(engine_scheduling_test Scheduling.cpp)

target_link_libraries(
    engine_scheduling_test
    PRIVATE
        lux::engine::engine_initialization
        lux::engine::ecs::ecs_visual
        lux::engine::ecs::ecs_hierarchy
        lux::engine::scene::scene_asset
)
target_compile_features(engine_scheduling_test PRIVATE cxx_std_20)
target_compile_options(engine_scheduling_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)

add_test(NAME engine.scheduling COMMAND engine_scheduling_test)
set_tests_properties(engine.scheduling PROPERTIES TIMEOUT 30)

add_executable(engine_ui_test Ui.cpp)


target_link_libraries(
    engine_ui_test
    PRIVATE
        lux::engine::engine_initialization
        lux::engine::systems::system_render
        lux::engine::function::ui
        lux::engine::function::input
        lux::engine::assets::engine_assets
        lux::engine::platform::window
)

target_compile_features(engine_ui_test PRIVATE cxx_std_20)
target_compile_options(engine_ui_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)

add_test(NAME engine.ui COMMAND engine_ui_test --frames 120)
set_tests_properties(engine.ui PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 LABELS "gpu;window")

add_test(NAME engine.ui_initialization_failure COMMAND engine_ui_test --reject-collector)
set_tests_properties(engine.ui_initialization_failure PROPERTIES RUN_SERIAL TRUE TIMEOUT 60 LABELS "gpu;window")

add_test(NAME engine.ui_effect_failure COMMAND engine_ui_test --reject-effect)
set_tests_properties(engine.ui_effect_failure PROPERTIES RUN_SERIAL TRUE TIMEOUT 60 LABELS "gpu;window")

add_executable(engine_model_scene_test ModelSceneLifecycle.cpp ModelSceneSupport.cpp)

target_include_directories(
    engine_model_scene_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../systems/render/pinclude
)

target_link_libraries(
    engine_model_scene_test
    PRIVATE
        lux::engine::engine_initialization
        lux::engine::systems::system_render
        lux::engine::assets::engine_assets
        lux::engine::ecs::ecs_visual
        lux::engine::ecs::ecs_transform
        lux::engine::toolchain::toolchain_model_cooker
        lux::engine::function::render_extensions
        lux::engine::platform::window
)

target_compile_features(engine_model_scene_test PRIVATE cxx_std_20)
target_compile_options(engine_model_scene_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
# Interactive/local-asset qualification: invoked explicitly with a source path, not a machine-specific CTest.

# Ordinary scene composition, kept separate from the lifecycle/failure-injection executable above.
add_executable(engine_model_example ModelScene.cpp ModelSceneSupport.cpp)

target_include_directories(engine_model_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../systems/render/pinclude)

target_link_libraries(
    engine_model_example
    PRIVATE
        lux::engine::engine_initialization
        lux::engine::systems::system_render
        lux::engine::assets::engine_assets
        lux::engine::ecs::ecs_visual
        lux::engine::ecs::ecs_transform
        lux::engine::toolchain::toolchain_model_cooker
        lux::engine::platform::window
)

target_compile_features(engine_model_example PRIVATE cxx_std_20)
target_compile_options(engine_model_example PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
