set(LUX_SCRIPT_CORE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptCore.cpp
)

add_component(
    COMPONENT_NAME  script_core
    NAMESPACE       lux::engine::function
    OUTPUT_NAME     lux_engine_function_script_core
    SOURCE_FILES    ${LUX_SCRIPT_CORE_SRCS}
)

component_include_directories(
    script_core
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(
    script_core
    PRIVATE
        LUX_FUNCTION_LIBRARY
)

lux_classify_target(
    TARGET  script_core
    LAYER   FUNCTION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

find_package(lux-cxx REQUIRED COMPONENTS compile_time)

target_link_libraries(
    script_core
    PUBLIC
        lux::cxx::compile_time
)

component_add_transitive_commands(
    script_core
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

add_subdirectory(test)
