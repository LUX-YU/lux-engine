message("-------- Core Layer --------")

generate_visibility_header(
    ENABLE_MACRO_NAME   LUX_CORE_LIBRARY
    PUBLIC_MACRO_NAME   LUX_CORE_PUBLIC
    GENERATE_FILE_PATH  lux/engine/core/visibility.h
)

add_subdirectory(math)
add_subdirectory(async_port)
add_subdirectory(semantic)
add_subdirectory(type_info)
add_subdirectory(meta)
add_subdirectory(object)
add_subdirectory(serialization)
add_subdirectory(log)
add_subdirectory(events)
add_subdirectory(task)

install_components(
	PROJECT_NAME	lux-engine-core
	VERSION			0.0.1
	NAMESPACE		lux::engine::core
	COMPONENTS		math
					async_port
					semantic
					type_info
					meta
					object
                    serialization
					log
					events
					task
)
