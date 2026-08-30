message("-------- Resource Layer --------")

generate_visibility_header(
    ENABLE_MACRO_NAME   LUX_RESOURCE_LIBRARY
    PUBLIC_MACRO_NAME   LUX_RESOURCE_PUBLIC
    GENERATE_FILE_PATH  lux/engine/resource/visibility.h
)

# Resource identity is the leaf shared by descriptions and Asset storage.
add_subdirectory(identity)
add_subdirectory(description)
add_subdirectory(asset)

install_components(
	PROJECT_NAME	lux-engine-resource
	VERSION			0.0.1
	NAMESPACE		lux::engine::resource
	COMPONENTS		identity
                    description
                    asset
)
