include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/ValueTypes.cmake")

# Neutral cooked metadata codec. Neither the compiler nor Asset/Script/Native is a runtime dependency.
add_component(
    STATIC
    COMPONENT_NAME shader_metadata
    NAMESPACE lux::engine::resource
    SOURCE_FILES "${CMAKE_CURRENT_LIST_DIR}/../src/ShaderInfo.cpp"
)

component_include_directories(
    shader_metadata
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_LIST_DIR}/../include"
    INSTALL_TIME include
)

component_add_internal_dependencies(shader_metadata lux::engine::resource::description_types)
target_compile_features(shader_metadata PUBLIC cxx_std_20)
