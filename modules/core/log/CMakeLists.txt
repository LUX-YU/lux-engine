## core::log — the engine's diagnostic-log service (architecture design §7.1).
##
## Front end: level + category + printf-style message, callable from any
## thread. Back end: host-assembled sink registry (stderr / file-with-writer-
## thread / logcat / editor panel). Deliberately dependency-free: every layer
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

lux_classify_target(
    TARGET  log
    LAYER   CORE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)
