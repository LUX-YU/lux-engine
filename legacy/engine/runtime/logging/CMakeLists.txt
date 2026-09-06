## engine/runtime/logging — process-domain diagnostic routing.
##
## Producers submit records through lux::log.  LogRouter uses AsyncRuntime only
## as an execution substrate; the execution target remains logging-blind.

if(NOT TARGET STDEXEC::stdexec)
    find_package(stdexec CONFIG REQUIRED)
endif()
if(NOT TARGET concurrentqueue::concurrentqueue)
    find_package(concurrentqueue CONFIG REQUIRED)
endif()

add_component(
    COMPONENT_NAME  runtime_logging
    STATIC
    NAMESPACE       lux::engine::runtime
    OUTPUT_NAME     lux_engine_runtime_logging
    SOURCE_FILES    ${CMAKE_CURRENT_SOURCE_DIR}/src/LogRouter.cpp
)

lux_classify_target(
    TARGET  runtime_logging
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

component_include_directories(
    runtime_logging
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME        include
)

target_include_directories(
    runtime_logging
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sinclude>
)

target_link_libraries(
    runtime_logging
    PUBLIC
        lux::engine::runtime::runtime_execution
        lux::engine::core::log
        concurrentqueue::concurrentqueue
    PRIVATE
        STDEXEC::stdexec
)

if(MSVC)
    target_compile_options(
        runtime_logging
        PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor
    )
endif()

component_add_transitive_commands(
    runtime_logging
    "find_package(stdexec CONFIG REQUIRED)"
    "find_package(concurrentqueue CONFIG REQUIRED)"
    "find_package(lux-engine-core REQUIRED COMPONENTS log)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_execution)"
)
