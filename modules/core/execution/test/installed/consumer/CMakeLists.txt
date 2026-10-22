find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution)

add_executable(execution_consumer ../Main.cpp)

target_link_libraries(execution_consumer PRIVATE lux::engine::core::core_execution)

if(MSVC)
    target_compile_options(execution_consumer PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR- /we4541)

    add_custom_command(TARGET execution_consumer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:execution_consumer>
            $<TARGET_FILE_DIR:execution_consumer>
        COMMAND_EXPAND_LISTS
    )
endif()

add_test(NAME installed.execution_owner COMMAND execution_consumer)

set_tests_properties(installed.execution_owner PROPERTIES TIMEOUT 15)
