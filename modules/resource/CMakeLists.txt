message("-------- Resource Layer --------")

generate_visibility_header(
    ENABLE_MACRO_NAME   LUX_RESOURCE_LIBRARY
    PUBLIC_MACRO_NAME   LUX_RESOURCE_PUBLIC
    GENERATE_FILE_PATH  lux/engine/resource/visibility.h
)

# Foundation ordering is explicit: asset identity is consumed by later
# domain-owned content codecs, while asset itself depends on description.
add_subdirectory(description)
add_subdirectory(asset)

# get sublist
subdirectory_list(dir_list)

foreach(subdir ${dir_list})
    if(subdir STREQUAL "description" OR subdir STREQUAL "asset")
        continue()  # already processed above
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${subdir}/CMakeLists.txt)
        add_subdirectory(${subdir})
    endif()
endforeach()

install_components(
	PROJECT_NAME	lux-engine-resource
	VERSION			0.0.1
	NAMESPACE		lux::engine::resource
	COMPONENTS		description
                    spatial
                    spatial3d_scene
                    entity_scene
                    classic_mesh_content
                    terrain_content
                    tilemap_content
                    physics3d_content
                    asset_identity
                    asset_core
                    asset_codecs
                    asset_pak
)
