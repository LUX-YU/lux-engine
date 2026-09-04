find_package(box2d CONFIG REQUIRED)
find_package(Eigen3 CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_PHYSICS2D_SIMULATION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_PHYSICS2D_SIMULATION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_PHYSICS2D_SIMULATION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/physics2d/visibility.h
)

include_component_cmake_scripts(meta)
include(${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/schema/cmake/engine_ecs_codegen.cmake)

add_component(
    COMPONENT_NAME physics2d_simulation
    NAMESPACE      lux::engine::physics2d
    OUTPUT_NAME    lux_engine_physics2d_simulation
    SOURCE_FILES
        src/Box2DWorld.cpp
        src/Physics2DSchema.cpp
        src/Physics2DSystem.cpp
)

component_include_directories(
    physics2d_simulation
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)

target_compile_definitions(
    physics2d_simulation
    PRIVATE LUX_ENGINE_PHYSICS2D_SIMULATION_LIBRARY
)
target_link_libraries(
    physics2d_simulation
    PUBLIC
        lux::engine::function::script_core
        lux::engine::simulation::system
        lux::engine::simulation::ecs::core
        lux::engine::simulation::ecs::transform
        Eigen3::Eigen
    PRIVATE
        lux::engine::core::serialization
        lux::engine::simulation::simulation_composition
        box2d::box2d
)
component_add_transitive_commands(
    physics2d_simulation
    "find_package(Eigen3 CONFIG REQUIRED)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core transform)"
)

engine_target_add_ecs_component_codegen(
    NAME physics2d_component_codegen
    TARGET physics2d_simulation
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/Physics2DSchema.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/physics2d/Physics2DComponents.hpp
    LOGICAL_PATH lux/engine/physics2d/Physics2DComponents.hpp
    SYMBOL physics2d
)
engine_target_add_static_type_info(
    NAME physics2d_configuration_codegen
    TARGET physics2d_simulation
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/Physics2DSystem.cpp
    HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/physics2d/Physics2DSystem.hpp
    LOGICAL_PATHS lux/engine/physics2d/Physics2DSystem.hpp
)
lux_script_abilities(
    TARGET physics2d_simulation
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/physics2d/abilities/PhysicsQuery2D.hpp
    LOGICAL_PATHS PhysicsQuery2D.hpp
)
get_target_property(_physics2d_ability_generated_dir physics2d_simulation LUX_SCRIPT_ABILITY_GENERATED_DIR)
install(
    FILES ${_physics2d_ability_generated_dir}/PhysicsQuery2D.ability.generated.hpp
    DESTINATION include/lux/engine/physics2d/abilities
    COMPONENT lux_sdk
)

lux_classify_target(
    TARGET  physics2d_simulation
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

if(BUILD_TESTING)
    add_executable(physics2d_simulation_test test/physics2d_simulation_test.cpp)
    target_link_libraries(
        physics2d_simulation_test
        PRIVATE
            lux::engine::physics2d::physics2d_simulation
            lux::engine::simulation::simulation_composition
            lux::engine::core::task
    )
    if(MSVC)
        target_compile_options(physics2d_simulation_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(physics2d_simulation_test PRIVATE -UNDEBUG)
    endif()
    lux_classify_target(
        TARGET  physics2d_simulation_test
        LAYER   SIMULATION
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    add_test(NAME physics2d_simulation_test COMMAND physics2d_simulation_test)

endif()

install_components(
    PROJECT_NAME lux-engine-physics2d
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::physics2d
    COMPONENTS   physics2d_simulation
)

set(
    LUX_PHYSICS2D_LUA_ARTIFACT
    ""
    CACHE FILEPATH
    "Toolchain-produced Physics2D Lua fixture used for cross-profile qualification"
)
set(
    LUX_PHYSICS2D_FLOWFORGE_ARTIFACT
    ""
    CACHE FILEPATH
    "Toolchain-produced Physics2D FlowForge fixture used for cross-profile qualification"
)
if(BUILD_TESTING AND TARGET simulation_script_lua AND LUX_PHYSICS2D_LUA_ARTIFACT)
    if(NOT EXISTS "${LUX_PHYSICS2D_LUA_ARTIFACT}")
        message(FATAL_ERROR "LUX_PHYSICS2D_LUA_ARTIFACT does not exist: ${LUX_PHYSICS2D_LUA_ARTIFACT}")
    endif()
    add_executable(physics2d_lua_integration_test test/physics2d_lua_integration_test.cpp)
    target_include_directories(
        physics2d_lua_integration_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
    )
    target_link_libraries(
        physics2d_lua_integration_test
        PRIVATE
            lux::engine::physics2d::physics2d_simulation
            lux::engine::simulation::simulation_composition
            lux::engine::simulation::simulation_script
            lux::engine::simulation::simulation_script_lua
            lux::engine::core::task
    )
    target_compile_definitions(
        physics2d_lua_integration_test
        PRIVATE LUX_PHYSICS2D_LUA_ARTIFACT="${LUX_PHYSICS2D_LUA_ARTIFACT}"
    )
    if(MSVC)
        target_compile_options(physics2d_lua_integration_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(physics2d_lua_integration_test PRIVATE -UNDEBUG)
    endif()
    lux_classify_target(
        TARGET  physics2d_lua_integration_test
        LAYER   SIMULATION
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    add_test(NAME physics2d_lua_integration_test COMMAND physics2d_lua_integration_test)
endif()

if(BUILD_TESTING AND TARGET simulation_script_lua AND TARGET simulation_script_native AND
   TARGET simulation_script_cpp_static AND LUX_PHYSICS2D_LUA_ARTIFACT AND LUX_PHYSICS2D_FLOWFORGE_ARTIFACT)
    if(NOT EXISTS "${LUX_PHYSICS2D_FLOWFORGE_ARTIFACT}")
        message(FATAL_ERROR
            "LUX_PHYSICS2D_FLOWFORGE_ARTIFACT does not exist: ${LUX_PHYSICS2D_FLOWFORGE_ARTIFACT}"
        )
    endif()
    add_executable(physics2d_script_benchmark benchmark/physics2d_script_benchmark.cpp)
    target_include_directories(
        physics2d_script_benchmark
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
    )
    target_link_libraries(
        physics2d_script_benchmark
        PRIVATE
            lux::engine::physics2d::physics2d_simulation
            lux::engine::simulation::simulation_composition
            lux::engine::simulation::simulation_script
            lux::engine::simulation::simulation_script_cpp_static
            lux::engine::simulation::simulation_script_lua
            lux::engine::simulation::simulation_script_native
            lux::engine::core::task
    )
    execute_process(
        COMMAND git rev-parse HEAD
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        OUTPUT_VARIABLE LUX_PHYSICS2D_BENCHMARK_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(LUX_PHYSICS2D_BENCHMARK_GIT_COMMIT STREQUAL "")
        set(LUX_PHYSICS2D_BENCHMARK_GIT_COMMIT "unknown")
    endif()
    target_compile_definitions(
        physics2d_script_benchmark
        PRIVATE
            LUX_BENCHMARK_GIT_COMMIT="${LUX_PHYSICS2D_BENCHMARK_GIT_COMMIT}"
            LUX_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    )
    lux_classify_target(
        TARGET  physics2d_script_benchmark
        LAYER   SIMULATION
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    add_test(
        NAME physics2d_script_benchmark_smoke
        COMMAND physics2d_script_benchmark
            --group scene-physics-mixed
            --size 9
            --frames 2
            --warmups 1
            --lua-artifact ${LUX_PHYSICS2D_LUA_ARTIFACT}
            --flowforge-artifact ${LUX_PHYSICS2D_FLOWFORGE_ARTIFACT}
            --output ${CMAKE_CURRENT_BINARY_DIR}/physics2d-script-benchmark-smoke.csv
    )
endif()
