add_subdirectory(script)
if(NOT TARGET authoring_material)
    add_subdirectory(material)
endif()

install_components(
    PROJECT_NAME lux-engine-authoring
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::authoring
    COMPONENTS
        authoring_script
        authoring_material
)
