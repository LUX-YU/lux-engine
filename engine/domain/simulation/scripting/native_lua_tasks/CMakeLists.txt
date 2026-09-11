generate_visibility_header(
    ENABLE_MACRO_NAME LUX_ENGINE_SIMULATION_SCRIPT_NATIVE_LUA_TASKS_LIBRARY
    PUBLIC_MACRO_NAME LUX_ENGINE_SIMULATION_SCRIPT_NATIVE_LUA_TASKS_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_ENGINE_SIMULATION_SCRIPT_NATIVE_LUA_TASKS_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/simulation/scripting/native_lua_tasks/visibility.h
)
add_component(COMPONENT_NAME simulation_script_native_lua_tasks NAMESPACE lux::engine::simulation
    OUTPUT_NAME lux_engine_simulation_script_native_lua_tasks SOURCE_FILES src/NativeLuaTaskBackend.cpp)
component_include_directories(simulation_script_native_lua_tasks BUILD_TIME_EXPORT
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR} INSTALL_TIME include)
target_compile_definitions(simulation_script_native_lua_tasks PRIVATE
    LUX_ENGINE_SIMULATION_SCRIPT_NATIVE_LUA_TASKS_LIBRARY)
target_link_libraries(simulation_script_native_lua_tasks PUBLIC
    lux::engine::simulation::simulation_script_lua lux::engine::simulation::simulation_script_cpp_static)
component_add_transitive_commands(simulation_script_native_lua_tasks
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script_lua simulation_script_cpp_static)")
lux_classify_target(TARGET simulation_script_native_lua_tasks LAYER SIMULATION PRODUCT RUNTIME ROLE INTEGRATION)

add_executable(native_lua_task_schema_writer test/EventSchema.cpp)
target_link_libraries(native_lua_task_schema_writer PRIVATE simulation_script)
lux_classify_target(TARGET native_lua_task_schema_writer LAYER SIMULATION PRODUCT BUILD_TOOL ROLE DOMAIN)
set(task_schema "${CMAKE_CURRENT_BINARY_DIR}/generated/task.events.json")
add_custom_command(OUTPUT "${task_schema}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND native_lua_task_schema_writer "${task_schema}" DEPENDS native_lua_task_schema_writer VERBATIM)
add_custom_target(native_lua_task_schema DEPENDS "${task_schema}")
lux_classify_target(TARGET native_lua_task_schema LAYER SIMULATION PRODUCT BUILD_TOOL ROLE DOMAIN)
add_executable(simulation_script_native_lua_tasks_test test/native_lua_tasks_test.cpp)
target_link_libraries(simulation_script_native_lua_tasks_test PRIVATE
    simulation_script_native_lua_tasks simulation_composition lux::engine::core::type_info)
target_include_directories(simulation_script_native_lua_tasks_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
if(MSVC)
    target_compile_options(native_lua_task_schema_writer PRIVATE /UNDEBUG)
    target_compile_options(simulation_script_native_lua_tasks_test PRIVATE /UNDEBUG)
else()
    target_compile_options(native_lua_task_schema_writer PRIVATE -UNDEBUG)
    target_compile_options(simulation_script_native_lua_tasks_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(TARGET simulation_script_native_lua_tasks_test LAYER SIMULATION PRODUCT TEST ROLE INTEGRATION)
include_component_cmake_scripts(simulation_script_cpp_static)
lux_cpp_static_scripts(TARGET simulation_script_native_lua_tasks_test NAME Na1Task
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/test/NativeTask.hpp LOGICAL_PATH NativeTask.hpp
    MODULE lux.na1.tasks DESCRIPTOR na1-tasks-v1 OWNER lux::simulation::na1::NativeTask SCOPE ENTITY
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/test/task.symbols.json
    ABILITY_TYPES lux::simulation::script::DelayAbility
    ABILITY_HEADERS lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp
    EVENT_SCHEMA "${task_schema}" EVENT_NAMES Task.event DEPENDS native_lua_task_schema)
add_test(NAME simulation_script_native_lua_tasks_test COMMAND simulation_script_native_lua_tasks_test)

include_component_cmake_scripts(script_core)
lux_script_abilities(TARGET simulation_script_native_lua_tasks_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test/TaskProbe.hpp)
