# engine/runtime/execution unit tests (opt-in).

if(NOT TARGET STDEXEC::stdexec)
    find_package(stdexec CONFIG REQUIRED)
endif()

function(lux_add_execution_test target source)
    add_executable(${target} ${source})
    target_link_libraries(
        ${target}
        PRIVATE runtime_execution STDEXEC::stdexec
    )
    if(MSVC)
        target_compile_options(
            ${target}
            PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor
        )
    endif()
endfunction()

lux_add_execution_test(
    main_thread_state_cache_test
    main_thread_state_cache_test.cpp
)
lux_add_execution_test(async_scope_test async_scope_test.cpp)
lux_add_execution_test(
    async_callback_sender_test
    async_callback_sender_test.cpp
)
lux_add_execution_test(async_runtime_test async_runtime_test.cpp)
lux_add_execution_test(async_file_service_test async_file_service_test.cpp)
