cmake_minimum_required(VERSION 3.22)

project(ExecutionInstalledConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution)

add_executable(execution_consumer Main.cpp)
target_link_libraries(execution_consumer PRIVATE lux::engine::core::core_execution)

if(MSVC)
    target_compile_options(execution_consumer PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR- /we4541)
    add_custom_command(TARGET execution_consumer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:execution_consumer>
            $<TARGET_FILE_DIR:execution_consumer>
        COMMAND_EXPAND_LISTS
    )
endif()

enable_testing()
add_test(NAME installed.execution_owner COMMAND execution_consumer)
set_tests_properties(installed.execution_owner PROPERTIES TIMEOUT 15)
