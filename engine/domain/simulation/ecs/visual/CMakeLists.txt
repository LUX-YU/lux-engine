generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_ECS_VISUAL_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_ECS_VISUAL_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_ECS_VISUAL_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/ecs/visual/visibility.h
)

add_component(
    COMPONENT_NAME visual
    NAMESPACE      lux::engine::simulation::ecs
    OUTPUT_NAME    lux_engine_simulation_ecs_visual
    SOURCE_FILES  ${CMAKE_CURRENT_SOURCE_DIR}/src/VisualSchema.cpp
)
component_include_directories(
    visual
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(visual PRIVATE LUX_ENGINE_SIMULATION_ECS_VISUAL_LIBRARY)
target_link_libraries(
    visual
    PUBLIC
        lux::engine::simulation::ecs::core
        lux::engine::simulation::ecs::schema
        lux::engine::simulation::ecs::snapshot
        lux::engine::resource::description
        lux::engine::core::serialization
)
component_add_internal_dependencies(visual core schema snapshot)
component_add_transitive_commands(
    visual
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS description)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core schema snapshot)"
)

include_component_cmake_scripts(meta)
include(${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/schema/cmake/engine_ecs_codegen.cmake)
engine_target_add_ecs_component_codegen(
    NAME visual_component_codegen
    TARGET visual
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/VisualSchema.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/simulation/ecs/Visual.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Visual.hpp
    SYMBOL visual
)
lux_classify_target(TARGET visual LAYER SIMULATION PRODUCT RUNTIME ROLE DOMAIN)

add_executable(visual_component_test test/visual_component_test.cpp)
target_link_libraries(visual_component_test PRIVATE lux::engine::simulation::ecs::visual)
if(MSVC)
    target_compile_options(visual_component_test PRIVATE /UNDEBUG)
else()
    target_compile_options(visual_component_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(TARGET visual_component_test LAYER SIMULATION PRODUCT TEST ROLE DOMAIN)
add_test(NAME visual_component_test COMMAND visual_component_test)
