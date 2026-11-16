include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/../../../resource/description/cmake/ValueTypes.cmake")

# Asset readers and target compilers need values and validation, never the authoring graph.
add_component(
    STATIC
    COMPONENT_NAME material_ir
    NAMESPACE lux::engine::function
    SOURCE_FILES
        "${CMAKE_CURRENT_LIST_DIR}/../src/ShaderIR.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/MaterialIR.cpp"
)

component_include_directories(
    material_ir
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_LIST_DIR}/../include"
    INSTALL_TIME include
)

target_link_libraries(material_ir PUBLIC lux::engine::resource::description_types)
target_compile_features(material_ir PUBLIC cxx_std_20)

component_add_transitive_commands(
    material_ir
    "find_package(lux-engine-resource REQUIRED COMPONENTS description_types)"
)
