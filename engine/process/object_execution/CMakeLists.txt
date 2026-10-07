add_component(
    COMPONENT_NAME object_execution
    NAMESPACE lux::engine::process
    STATIC
    SOURCE_FILES src/ObjectScheduler.cpp
)
component_include_directories(object_execution
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)
target_link_libraries(object_execution PUBLIC
    lux::engine::process::process_execution
    lux::engine::core::object
    STDEXEC::stdexec
)
component_add_transitive_commands(object_execution
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-core REQUIRED COMPONENTS object)"
)
lux_engine_install_components(
    PROJECT_NAME lux-engine-process-object-execution
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::process
    COMPONENTS object_execution
)


if(BUILD_TESTING)
    cmake_file_api(QUERY API_VERSION 1 CODEMODEL 2)
    add_executable(object_scheduler_test test/ObjectSchedulerTest.cpp)
    target_link_libraries(object_scheduler_test PRIVATE object_execution)
    if(MSVC)
        target_compile_options(object_scheduler_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(object_scheduler_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME process.object_scheduler COMMAND object_scheduler_test)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME process.object_dependencies COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/check_dependencies.py" "${CMAKE_BINARY_DIR}")
endif()
