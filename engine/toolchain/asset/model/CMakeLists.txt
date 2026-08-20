find_package(assimp REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)

add_component(
    STATIC
    COMPONENT_NAME toolchain_model_importer
    NAMESPACE      lux::engine::toolchain
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ModelImporter.cpp
)

component_include_directories(
    toolchain_model_importer
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME
        include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/modules/resource/asset/pinclude
)

target_link_libraries(
    toolchain_model_importer
    PUBLIC
        lux::engine::resource::asset
        lux::engine::toolchain::toolchain_texture_importer
    PRIVATE
        assimp::assimp
        meshoptimizer::meshoptimizer
        lux::cxx::core
)

component_add_transitive_commands(
    toolchain_model_importer
    "find_package(lux-cxx REQUIRED COMPONENTS core)"
)

lux_classify_target(
    TARGET  toolchain_model_importer
    LAYER   TOOLCHAIN
    PRODUCT TOOLCHAIN
    ROLE    DOMAIN
)
