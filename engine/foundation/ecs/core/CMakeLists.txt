find_package(EnTT CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_CORE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_CORE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_CORE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/core/visibility.h
)

add_component(
    COMPONENT_NAME ecs_core
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_core
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EcsCommandBuffer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntityCreationPlan.cpp
)

component_include_directories(
    ecs_core
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    ecs_core
    PUBLIC ENTT_NO_ETO
    PRIVATE LUX_ENGINE_ECS_CORE_LIBRARY
)
target_link_libraries(ecs_core PUBLIC EnTT::EnTT lux::cxx::compile_time)
component_add_transitive_commands(
    ecs_core
    "find_package(EnTT CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

if(BUILD_TESTING)
    add_executable(ecs_component_observation_test test/ComponentObservation.cpp)
    target_link_libraries(ecs_component_observation_test PRIVATE ecs_core)
    if(MSVC)
        target_compile_options(ecs_component_observation_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(ecs_component_observation_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ecs.component_observation COMMAND ecs_component_observation_test)
endif()
