find_package(lux-cxx REQUIRED COMPONENTS algorithm)

add_interface_component(
    COMPONENT_NAME semantic
    NAMESPACE      lux::engine::core
)

component_include_directories(
    semantic
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME      include
)

target_link_libraries(semantic INTERFACE lux::cxx::algorithm)

component_add_transitive_commands(semantic "find_package(lux-cxx REQUIRED COMPONENTS algorithm)")

if(BUILD_TESTING)
    add_executable(semantic_identity_test test/identity.cpp)
    target_link_libraries(semantic_identity_test PRIVATE lux::engine::core::semantic)
    add_test(NAME semantic.identity COMMAND semantic_identity_test)
endif()
