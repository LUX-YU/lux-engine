message("-------- Resource Layer --------")

generate_visibility_header(
    ENABLE_MACRO_NAME   LUX_RESOURCE_LIBRARY
    PUBLIC_MACRO_NAME   LUX_RESOURCE_PUBLIC
    GENERATE_FILE_PATH  lux/engine/resource/visibility.h
)

# Resource owns two installable packages. Asset contains its identity, core,
# codec, and pak implementation behind one public component.
add_subdirectory(description)
add_subdirectory(asset)

install_components(
	PROJECT_NAME	lux-engine-resource
	VERSION			0.0.1
	NAMESPACE		lux::engine::resource
	COMPONENTS		description
                    asset
)
