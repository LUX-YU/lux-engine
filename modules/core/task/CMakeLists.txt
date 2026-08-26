## core::task — generic immutable task DAG + completion-driven worker executor.

find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_CORE_TASK_LIBRARY
    PUBLIC_MACRO_NAME       LUX_CORE_TASK_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_CORE_TASK_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/task/visibility.h
)

add_component(
    COMPONENT_NAME  task
    NAMESPACE       lux::engine::core
    OUTPUT_NAME     lux_engine_core_task
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TaskCallable.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TaskGraphBuilder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TaskExecutor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TaskExecutorWorker.cpp
)

component_include_directories(
    task
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)

target_compile_definitions(task PRIVATE LUX_CORE_TASK_LIBRARY)

target_link_libraries(
    task
    PUBLIC
        lux::cxx::compile_time
)

component_add_transitive_commands(
    task
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

lux_classify_target(
    TARGET  task
    LAYER   CORE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(task_graph_test test/task_graph_test.cpp)
target_link_libraries(task_graph_test PRIVATE lux::engine::core::task)
if(MSVC)
    target_compile_options(task_graph_test PRIVATE /UNDEBUG)
else()
    target_compile_options(task_graph_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  task_graph_test
    LAYER   CORE
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME task_graph_test COMMAND task_graph_test)
