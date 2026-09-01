find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SYSTEM_DESCRIPTION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SYSTEM_DESCRIPTION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SYSTEM_DESCRIPTION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/system/description/visibility.h
)

add_component(
    COMPONENT_NAME domain_system_description
    NAMESPACE      lux::engine::system
    OUTPUT_NAME    lux_engine_domain_system_description
    SOURCE_FILES  ${CMAKE_CURRENT_SOURCE_DIR}/src/SystemDependencyOrder.cpp
)
component_include_directories(
    domain_system_description
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(domain_system_description PRIVATE LUX_ENGINE_SYSTEM_DESCRIPTION_LIBRARY)
target_link_libraries(
    domain_system_description
    PUBLIC lux::engine::system::domain_system_identity lux::cxx::compile_time
)
component_add_transitive_commands(
    domain_system_description
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-domain-system-identity REQUIRED COMPONENTS domain_system_identity)"
)
lux_classify_target(TARGET domain_system_description LAYER DOMAIN PRODUCT RUNTIME ROLE FOUNDATION)

add_executable(
    domain_system_description_test
    test/system_description_test.cpp
)
target_link_libraries(
    domain_system_description_test
    PRIVATE lux::engine::system::domain_system_description
)
if(MSVC)
    target_compile_options(domain_system_description_test PRIVATE /UNDEBUG)
else()
    target_compile_options(domain_system_description_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(TARGET domain_system_description_test LAYER DOMAIN PRODUCT TEST ROLE FOUNDATION)
add_test(NAME domain_system_description_test COMMAND domain_system_description_test)

install_components(
    PROJECT_NAME lux-engine-domain-system-description
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::system
    COMPONENTS   domain_system_description
)
