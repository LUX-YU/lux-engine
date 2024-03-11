set(LUX_EDITOR_SRCS
    src/builtin-obj/BasicCamera.cpp
)

add_component(
    COMPONENT_NAME builtin_object
    NAMESPACE      lux::engine::tools
    SOURCE_FILES   ${LUX_EDITOR_SRCS}
)

component_include_directories(
    builtin_object
    BUILD_TIME_EXPORT  
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(
    builtin_object
    PRIVATE
    LUX_TOOLS_LIBRARY
)

target_link_libraries(
    builtin_object
    PUBLIC
    lux::engine::core::meta
    lux::engine::core::math
)

component_add_transitive_commands(
    builtin_object
    "find_package (Eigen3 REQUIRED)"
)

add_executable(
    LuxEditor
    src/editor/LuxEditor.cpp
)

target_link_libraries(
    LuxEditor
    PRIVATE
    lux::engine::resource::asset
    lux::engine::function::render
    lux::engine::tools::builtin_object
)

component_add_internal_dependencies(
    LuxEditor
    lux::engine::tools::ui
)