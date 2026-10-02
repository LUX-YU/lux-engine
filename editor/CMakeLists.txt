include(${PROJECT_SOURCE_DIR}/cmake/EditorTests.cmake)

# Formal providers, configured inside-out. Layers are source ownership, not aggregate libraries.
add_subdirectory(editing)
add_subdirectory(authoring)
add_subdirectory(activities)
add_subdirectory(workbench)
add_subdirectory(application)

# Existing product only: contribution protocols expire at P11, product conversion at P12.
# No formal provider above may depend on these implementations.
add_subdirectory(metadata)
add_subdirectory(context)
add_subdirectory(ui)
add_subdirectory(tools)
add_subdirectory(launcher)
add_subdirectory(app)

# All cross-layer targets see their real providers before target_link_libraries.
add_subdirectory(assets) # Remaining P12 save-adapter tests.
add_subdirectory(tests/integration)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_subdirectory(tests/architecture)
    add_subdirectory(tests/persistence)
    add_subdirectory(tests/workspace)
endif()
