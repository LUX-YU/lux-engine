## core::async_port — ECS-free typed asynchronous admission contract.
##
## This target deliberately owns no scheduler, queue, worker, registry or
## dependency graph. Runtime execution supplies Endpoint implementations.

add_interface_component(
    COMPONENT_NAME async_port
    NAMESPACE      lux::engine::core
)

component_include_directories(
    async_port
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME      include
)

target_link_libraries(
    async_port
    INTERFACE
        lux::cxx::compile_time
)

component_add_transitive_commands(
    async_port
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

lux_classify_target(
    TARGET  async_port
    LAYER   CORE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(
    async_port_contract_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/async_port_contract_test.cpp
)
target_link_libraries(
    async_port_contract_test
    PRIVATE lux::engine::core::async_port
)
if(MSVC)
    target_compile_options(async_port_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(async_port_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  async_port_contract_test
    LAYER   CORE
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME async_port_contract_test COMMAND async_port_contract_test)
