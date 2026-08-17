add_executable(log_router_test log_router_test.cpp)
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
