find_package(lux-cxx CONFIG REQUIRED COMPONENTS core compile_time memory container)

add_component(
    COMPONENT_NAME object
    NAMESPACE lux::engine::core
    OUTPUT_NAME lux_engine_core_object
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Connection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Object.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ObjectDispatcher.cpp
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
