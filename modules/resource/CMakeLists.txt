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

install_components(
	PROJECT_NAME	lux-engine-resource
	VERSION			0.0.1
	NAMESPACE		lux::engine::resource
	COMPONENTS		description
                    asset_identity
                    asset_core
                    asset_codecs
                    asset_pak
)
