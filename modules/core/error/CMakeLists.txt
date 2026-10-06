find_package(lux-cxx CONFIG REQUIRED COMPONENTS algorithm compile_time)
add_component(COMPONENT_NAME error NAMESPACE lux::engine::core
    OUTPUT_NAME lux_engine_core_error SOURCE_FILES src/ErrorRegistry.cpp)
component_include_directories(error BUILD_TIME_EXPORT
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include PRIVATE pinclude)
target_compile_definitions(error PRIVATE LUX_CORE_LIBRARY)
target_link_libraries(error PUBLIC lux::cxx::algorithm lux::cxx::compile_time)
component_add_transitive_commands(error "find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time)")

if(BUILD_TESTING)
    add_library(error_plugin MODULE test/plugin.cpp)
    target_link_libraries(error_plugin PRIVATE error)
    add_executable(error_registry_test test/registry.cpp)
    target_include_directories(error_registry_test PRIVATE pinclude)
    target_link_libraries(error_registry_test PRIVATE error lux::engine::platform::dynamic_library)
    if(MSVC)
        target_compile_options(error_registry_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(error_registry_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME error.registry COMMAND error_registry_test $<TARGET_FILE:error_plugin>)
endif()
