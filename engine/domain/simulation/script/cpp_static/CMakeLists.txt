generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/script/cpp_static/visibility.h
)

add_component(
    COMPONENT_NAME simulation_script_cpp_static
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_script_cpp_static
    SOURCE_FILES src/CppStaticScriptBridge.cpp
)
component_include_directories(
    simulation_script_cpp_static
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    simulation_script_cpp_static
    PRIVATE LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_LIBRARY
)
target_link_libraries(
    simulation_script_cpp_static
    PUBLIC
        lux::engine::simulation::simulation_script
        lux::engine::core::meta
)
component_add_transitive_commands(
    simulation_script_cpp_static
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta)"
)
lux_classify_target(
    TARGET  simulation_script_cpp_static
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

include_component_cmake_scripts(meta)
engine_add_type_runtime_info(
    NAME cpp_static_script_bridge_test_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/test/CppStaticScriptBridgeFixture.hpp
    REGISTER_FUNC_NAME lux_cpp_static_script_bridge_test
)
add_executable(
    simulation_script_cpp_static_test
    test/cpp_static_script_bridge_test.cpp
)
target_include_directories(
    simulation_script_cpp_static_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
)
target_link_libraries(
    simulation_script_cpp_static_test
    PRIVATE lux::engine::simulation::simulation_script_cpp_static
)
engine_target_add_type_runtime_info(
    TARGET simulation_script_cpp_static_test
    METAS cpp_static_script_bridge_test_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen
)
if(MSVC)
    target_compile_options(
        simulation_script_cpp_static_test PRIVATE /UNDEBUG
    )
else()
    target_compile_options(
        simulation_script_cpp_static_test PRIVATE -UNDEBUG
    )
endif()
lux_classify_target(
    TARGET  simulation_script_cpp_static_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    INTEGRATION
)
add_test(
    NAME simulation_script_cpp_static_test
    COMMAND simulation_script_cpp_static_test
)
