find_package(lux-cxx CONFIG REQUIRED COMPONENTS core compile_time memory container)

add_component(
    COMPONENT_NAME object
    NAMESPACE lux::engine::core
    OUTPUT_NAME lux_engine_core_object
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Connection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Object.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ObjectDispatcher.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CodeLease.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ObjectOwnership.cpp
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
        lux::cxx::core
        lux::cxx::compile_time
        lux::cxx::memory
        lux::cxx::container
)

component_add_transitive_commands(
    object
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time memory container)"
)

if(BUILD_TESTING)
    add_library(object_ownership_plugin MODULE test/ownership_plugin.cpp)
    target_link_libraries(object_ownership_plugin PRIVATE object)
    add_executable(object_ownership_test test/ownership.cpp)
    target_link_libraries(object_ownership_test PRIVATE object lux::engine::platform::dynamic_library)
    if(MSVC)
        target_compile_options(object_ownership_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(object_ownership_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME object.ownership COMMAND object_ownership_test $<TARGET_FILE:object_ownership_plugin>)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME object.retirement_contract COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/retirement_contract.py" $<TARGET_FILE:object_ownership_test>)
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
endif()
