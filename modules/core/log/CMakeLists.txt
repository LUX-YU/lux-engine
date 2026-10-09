## core::log — the engine's diagnostic-log service (architecture design §7.1).
##
## Front end: level + category + checked format, callable from any thread.
## Back end: one borrowed host-owned output target, otherwise stderr.
## Deliberately dependency-free: every layer
## above core may log, so this component must sit below all of them.

set(LUX_LOG_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Log.cpp
)

add_component(
    COMPONENT_NAME  log
    NAMESPACE       lux::engine::core
    OUTPUT_NAME     lux_engine_core_log
    SOURCE_FILES    ${LUX_LOG_SRCS}
)

component_include_directories(
    log
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
                        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME        include
)

target_compile_definitions(
    log
    PRIVATE
    LUX_CORE_LIBRARY
)

if(ANDROID)
    # __android_log_print lives in the NDK's liblog. The quoted "-llog" form
    # keeps CMake from resolving the name against this very target.
    target_link_libraries(log PRIVATE "-llog")
endif()

if(BUILD_TESTING)
    add_executable(log_output_lifetime_test test/output_lifetime.cpp)

    target_link_libraries(log_output_lifetime_test PRIVATE lux::engine::core::log)

    add_test(NAME log.output_lifetime COMMAND log_output_lifetime_test)
endif()
