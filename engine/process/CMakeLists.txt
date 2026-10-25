# Process is a collection root. Production code belongs to concrete child packages.
add_subdirectory(world_loading)
add_subdirectory(asset_loading)
add_subdirectory(object_execution)

if(TARGET lux::engine::function::render)
    add_subdirectory(render_upload)
endif()
