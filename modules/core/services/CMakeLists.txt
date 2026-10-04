find_package(lux-cxx REQUIRED COMPONENTS core compile_time memory)
generate_visibility_header(
    ENABLE_MACRO_NAME LUX_SERVICES_LIBRARY
    PUBLIC_MACRO_NAME LUX_SERVICES_PUBLIC
    GENERATE_FILE_PATH lux/engine/services/visibility.h
)
# Registry identities and shared control-block release bridges are shared by real DLL consumers.
add_component(COMPONENT_NAME services NAMESPACE lux::engine::core OUTPUT_NAME lux_engine_core_services
    SOURCE_FILES src/ServiceRegistry.cpp)
component_include_directories(services
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR} INSTALL_TIME include)
target_compile_definitions(services PRIVATE LUX_SERVICES_LIBRARY)
target_link_libraries(services PUBLIC lux::engine::core::object
    lux::cxx::core lux::cxx::compile_time lux::cxx::memory)
component_add_transitive_commands(services
    "find_package(lux-engine-core REQUIRED COMPONENTS object)"
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time memory)")

if(BUILD_TESTING)
    add_library(services_plugin MODULE test/plugin_module.cpp)
    target_link_libraries(services_plugin PRIVATE services)
    add_executable(services_plugin_test test/plugin.cpp)
    target_link_libraries(services_plugin_test PRIVATE services lux::engine::platform::dynamic_library)
    add_executable(services_test test/services.cpp)
    target_link_libraries(services_test PRIVATE services)
    if(MSVC)
        target_compile_options(services_test PRIVATE /UNDEBUG)
        target_compile_options(services_plugin PRIVATE /UNDEBUG)
        target_compile_options(services_plugin_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(services_test PRIVATE -UNDEBUG)
        target_compile_options(services_plugin PRIVATE -UNDEBUG)
        target_compile_options(services_plugin_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME services.core COMMAND services_test)
    add_test(NAME services.plugin COMMAND services_plugin_test $<TARGET_FILE:services_plugin>)
endif()
