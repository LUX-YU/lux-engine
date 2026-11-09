find_package(lux-cxx CONFIG REQUIRED COMPONENTS core compile_time memory container)

add_component(
    COMPONENT_NAME object
    NAMESPACE lux::engine::core
    OUTPUT_NAME lux_engine_core_object
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Connection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Object.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ObjectTarget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ObjectRuntime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Reclamation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CodeLease.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ObjectOwnership.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ObjectScheduler.cpp
)

component_include_directories(
    object
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE pinclude
)

target_compile_definitions(object PRIVATE LUX_CORE_LIBRARY)

target_link_libraries(
    object
    PUBLIC
        lux::engine::core::core_execution
        lux::cxx::core
        lux::cxx::compile_time
        lux::cxx::memory
        lux::cxx::container
)

component_add_transitive_commands(
    object
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time memory container)"
    "find_package(lux-engine-core-execution REQUIRED COMPONENTS core_execution)"
)

if(BUILD_TESTING)
    add_library(object_ownership_plugin MODULE test/ownership_plugin.cpp)
    target_link_libraries(object_ownership_plugin PRIVATE object)
    add_executable(object_ownership_test test/ownership.cpp)

    target_include_directories(object_ownership_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/test-support")

    target_link_libraries(object_ownership_test PRIVATE object lux::engine::platform::dynamic_library)
    if(MSVC)
        target_compile_options(object_ownership_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(object_ownership_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME object.ownership COMMAND object_ownership_test $<TARGET_FILE:object_ownership_plugin>)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME object.retirement_contract COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/retirement_contract.py" $<TARGET_FILE:object_ownership_test>
    )

    add_executable(object_tree_test test/tree.cpp)
    target_link_libraries(object_tree_test PRIVATE object)
    if(MSVC)
        target_compile_options(object_tree_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(object_tree_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME object.tree COMMAND object_tree_test)
endif()

if(BUILD_TESTING)
    add_executable(object_queue_test test/queue.cpp)
    target_link_libraries(object_queue_test PRIVATE object)
    if(MSVC)
        target_compile_options(object_queue_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(object_queue_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME object.queue COMMAND object_queue_test)
    add_test(
        NAME object.runtime_shutdown COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/runtime_shutdown.py" $<TARGET_FILE:object_queue_test>
    )
endif()

if(BUILD_TESTING)
    add_executable(object_target_test test/target.cpp)

    target_include_directories(object_target_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/test-support")

    target_link_libraries(object_target_test PRIVATE object)
    if(MSVC)
        target_compile_options(object_target_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(object_target_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME object.target COMMAND object_target_test)
    add_test(
        NAME object.target_contract COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/target_contract.py" $<TARGET_FILE:object_target_test>
    )
endif()

if(BUILD_TESTING)
    add_library(object_signal_sender_compile OBJECT test/SignalSenderCompile.cpp)
    target_link_libraries(object_signal_sender_compile PRIVATE object)
endif()

if(BUILD_TESTING)
    cmake_file_api(QUERY API_VERSION 1 CODEMODEL 2)
    add_executable(object_scheduler_test test/execution/ObjectSchedulerTest.cpp)
    target_link_libraries(object_scheduler_test PRIVATE object)
    if(MSVC)
        target_compile_options(object_scheduler_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(object_scheduler_test PRIVATE -UNDEBUG)
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME object.scheduler COMMAND object_scheduler_test)
    add_test(
        NAME object.execution_dependencies COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/execution/check_dependencies.py" "${CMAKE_BINARY_DIR}"
    )
endif()
