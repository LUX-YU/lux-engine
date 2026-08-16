## core::events — main-thread committed domain facts (lux::events).
##
## Pumps are host-owned safe points. Cross-thread work returns through
## AsyncRuntime's MainThreadScheduler before state commit + fact publication.
##
## Layering: sits BELOW log (the log service publishes through a callback
## seam installed by the host), so this component depends on nothing in the
## engine — only lux-cxx compile_time (type_hash) and moodycamel.

set(LUX_EVENTS_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/DomainEvents.cpp
)

add_component(
    COMPONENT_NAME  events
    NAMESPACE       lux::engine::core
    OUTPUT_NAME     lux_engine_core_events
    SOURCE_FILES    ${LUX_EVENTS_SRCS}
)

component_include_directories(
    events
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
                        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME        include
)

target_compile_definitions(
    events
    PRIVATE
    LUX_CORE_LIBRARY
)

target_link_libraries(
    events
    PUBLIC
    lux::cxx::compile_time
)

component_add_transitive_commands(
    events
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

lux_classify_target(
    TARGET  events
    LAYER   CORE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

set(ENABLE_EVENTS_TEST OFF CACHE BOOL "Enable events module tests")
if(ENABLE_EVENTS_TEST)
    add_subdirectory(test)
endif()
