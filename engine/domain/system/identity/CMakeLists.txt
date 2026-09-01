find_package(lux-cxx REQUIRED COMPONENTS core)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SYSTEM_IDENTITY_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SYSTEM_IDENTITY_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SYSTEM_IDENTITY_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/system/identity/visibility.h
)

add_component(
    COMPONENT_NAME domain_system_identity
    NAMESPACE      lux::engine::system
    OUTPUT_NAME    lux_engine_domain_system_identity
    SOURCE_FILES  ${CMAKE_CURRENT_SOURCE_DIR}/src/SystemTypeId.cpp
)
component_include_directories(
    domain_system_identity
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(domain_system_identity PRIVATE LUX_ENGINE_SYSTEM_IDENTITY_LIBRARY)
target_link_libraries(domain_system_identity PUBLIC lux::cxx::core)
component_add_transitive_commands(
    domain_system_identity
    "find_package(lux-cxx REQUIRED COMPONENTS core)"
)
lux_classify_target(TARGET domain_system_identity LAYER DOMAIN PRODUCT RUNTIME ROLE FOUNDATION)

install_components(
    PROJECT_NAME lux-engine-domain-system-identity
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::system
    COMPONENTS   domain_system_identity
)
