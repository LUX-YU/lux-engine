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
    LAYER   CORE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

find_package(lux-cxx REQUIRED COMPONENTS core compile_time)

target_link_libraries(
    script_core
    PUBLIC
        lux::engine::core::semantic
        lux::cxx::core
        lux::cxx::compile_time
)

component_add_transitive_commands(
    script_core
    "find_package(lux-engine-core REQUIRED COMPONENTS semantic)"
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time)"
)

component_add_cmake_scripts(
    script_core
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_script_ability_codegen.cmake
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_script_semantic_schema.cmake
)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/cmake/script_semantic_schema.cpp.in
    DESTINATION share/lux-engine-function/script_core/cmake_scripts)

install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/template/
    DESTINATION share/lux-engine-function/script_core/cmake_scripts/template
)

add_subdirectory(test)
