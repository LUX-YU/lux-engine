add_subdirectory(client)
include(features/cmake/feature_client.cmake)
add_subdirectory(standard_content)
add_subdirectory(graph)
add_subdirectory(vulkan)

add_subdirectory(features)
add_subdirectory(runtime)
