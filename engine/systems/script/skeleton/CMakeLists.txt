add_component(
    COMPONENT_NAME scene_script_skeleton NAMESPACE lux::engine::scene OUTPUT_NAME lux_engine_scene_script_skeleton
    STATIC SOURCE_FILES src/SkeletonAbility.cpp
)
component_include_directories(scene_script_skeleton
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include
)
target_link_libraries(scene_script_skeleton PUBLIC scene_script_assets)
component_add_transitive_commands(scene_script_skeleton
    "find_package(lux-engine-scene-script-assets REQUIRED COMPONENTS scene_script_assets)"
)
include_component_cmake_scripts(script_core)
lux_script_abilities(
    TARGET scene_script_skeleton
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/scene/scripting/SkeletonAbility.hpp
    LOGICAL_PATHS lux/engine/scene/scripting/SkeletonAbility.hpp
)
get_target_property(_skeleton_generated scene_script_skeleton LUX_SCRIPT_ABILITY_GENERATED_DIR)
install(FILES ${_skeleton_generated}/SkeletonAbility.ability.generated.hpp
    ${_skeleton_generated}/SkeletonAbility.ability.lua.generated.hpp
    DESTINATION include/lux/engine/scene/scripting COMPONENT lux_sdk
)
lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-script-skeleton VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::scene COMPONENTS scene_script_skeleton
)
