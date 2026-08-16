add_executable(log_router_test log_router_test.cpp)
add_test(NAME log_router_test COMMAND log_router_test)
set_tests_properties(log_router_test PROPERTIES LABELS "cpu;logging")
target_link_libraries(
    log_router_test
    PRIVATE
        runtime_logging
        lux::engine::runtime::runtime_execution
        STDEXEC::stdexec
)

if(MSVC)
    target_compile_options(
        log_router_test
        PRIVATE
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
    )
endif()
