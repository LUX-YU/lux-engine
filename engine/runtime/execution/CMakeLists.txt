## engine/runtime/execution — domain-blind asynchronous orchestration.
##
## AsyncRuntime owns one Asio coordinator, a small blocking-IO compatibility
## pool, oneTBB background execution and the generation-safe main-thread
## scheduler.  Assets, rendering and logging are clients of this target; none
## of their types or libraries may flow back into this boundary.

if(NOT TARGET STDEXEC::stdexec)
    find_package(stdexec CONFIG REQUIRED)
endif()
if(NOT TARGET concurrentqueue::concurrentqueue)
    find_package(concurrentqueue CONFIG REQUIRED)
endif()
if(NOT TARGET asio::asio)
    find_package(asio CONFIG REQUIRED)
endif()
if(NOT TARGET TBB::tbb)
    find_package(TBB CONFIG REQUIRED)
endif()

add_component(
    COMPONENT_NAME  runtime_execution
    STATIC
    NAMESPACE       lux::engine::runtime
    OUTPUT_NAME     lux_engine_runtime_execution
    SOURCE_FILES    ${CMAKE_CURRENT_SOURCE_DIR}/src/AsyncFileService.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/AsyncRuntime.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/AsyncScope.cpp
)

lux_classify_target(
    TARGET  runtime_execution
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

component_include_directories(
    runtime_execution
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME        include
)

target_include_directories(
    runtime_execution
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sinclude>
)

target_link_libraries(
    runtime_execution
    PUBLIC
        lux::engine::core::async_port
        lux::cxx::compile_time
        concurrentqueue::concurrentqueue
        asio::asio
        TBB::tbb
    PRIVATE
        STDEXEC::stdexec
)

target_compile_definitions(
    runtime_execution
    PUBLIC
        ASIO_STANDALONE
        ASIO_NO_DEPRECATED
        ASIO_NO_TYPEID
        ASIO_NO_TS_EXECUTORS
        ASIO_NO_EXCEPTIONS
)

if(WIN32)
    target_compile_definitions(
        runtime_execution
        PUBLIC _WIN32_WINNT=0x0A00
    )
endif()

if(MSVC)
    target_compile_options(
        runtime_execution
        PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor
    )
endif()

set_target_properties(
    runtime_execution
    PROPERTIES POSITION_INDEPENDENT_CODE ON
)

component_add_transitive_commands(
    runtime_execution
    "find_package(stdexec CONFIG REQUIRED)"
    "find_package(concurrentqueue CONFIG REQUIRED)"
    "find_package(asio CONFIG REQUIRED)"
    "find_package(TBB CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS async_port)"
)

set(ENABLE_EXECUTION_TEST OFF CACHE BOOL "Enable execution tests")
if(ENABLE_EXECUTION_TEST)
    add_subdirectory(test)
endif()
