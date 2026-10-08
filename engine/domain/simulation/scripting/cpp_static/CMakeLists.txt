generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/scripting/cpp_static/visibility.h
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
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/sinclude
)
target_compile_definitions(
    simulation_script_cpp_static
    PRIVATE LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_LIBRARY
)
target_link_libraries(
    simulation_script_cpp_static
    PUBLIC
        lux::engine::simulation::simulation_script
)
component_add_transitive_commands(
    simulation_script_cpp_static
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script)"
)

component_add_cmake_scripts(simulation_script_cpp_static ${CMAKE_CURRENT_SOURCE_DIR}/cmake/lux_cpp_static_scripts.cmake)
install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/template/
    DESTINATION share/lux-engine-simulation/simulation_script_cpp_static/cmake_scripts/template
)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/lux_cpp_static_scripts.cmake)

if(BUILD_TESTING)
    add_executable(
        cpp_script_construction_test
        ${PROJECT_SOURCE_DIR}/cmake/installed-consumers/script-construction/cpp.cpp
    )
    target_link_libraries(cpp_script_construction_test PRIVATE simulation_script_cpp_static)
    if(MSVC)
        target_compile_options(cpp_script_construction_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(cpp_script_construction_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME script.cpp_construction COMMAND cpp_script_construction_test)
    set_tests_properties(script.cpp_construction PROPERTIES TIMEOUT 30)
endif()
