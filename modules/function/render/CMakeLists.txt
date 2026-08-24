add_subdirectory(client)
add_subdirectory(standard_content)
add_subdirectory(graph)
add_subdirectory(vulkan)

add_subdirectory(features)

set(ENABLE_RENDER_TEST false CACHE BOOL "Enable render test")
if(ENABLE_RENDER_TEST AND TARGET render_features)
    add_subdirectory(test)
endif()
