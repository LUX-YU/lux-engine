include(${PROJECT_SOURCE_DIR}/cmake/EditorTests.cmake)

# Formal providers, configured inside-out. Layers are source ownership, not aggregate libraries.
add_subdirectory(editing)
add_subdirectory(authoring)
add_subdirectory(activities)
add_subdirectory(workbench)
add_subdirectory(application)

# All cross-layer targets see their real providers before target_link_libraries.
add_subdirectory(tests/integration)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_subdirectory(tests/architecture)
    add_subdirectory(tests/persistence)
    add_subdirectory(tests/workspace)
endif()
