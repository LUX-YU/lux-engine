message(FATAL_ERROR "Frozen reference source: build editor_legacy from its historical Git revision.")

include(${CMAKE_CURRENT_LIST_DIR}/MigrationStage.cmake)
include(${PROJECT_SOURCE_DIR}/cmake/EditorTests.cmake)

# Legacy-generated ABI/visibility headers must never enter the shared generated include
# directory installed by engine/modules providers. This subtree is EXCLUDE_FROM_ALL.
set(LUX_GENERATE_HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen/include")

# Formal providers, configured inside-out. Layers are source ownership, not aggregate libraries.
add_subdirectory(editing)
add_subdirectory(authoring)
add_subdirectory(activities)
add_subdirectory(workbench)
add_subdirectory(application)

# All cross-layer targets see their real providers before target_link_libraries.
add_subdirectory(tests/integration)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    option(LUX_EDITOR_LEGACY_ARCHITECTURE_TESTS "Enable historical migration architecture qualification" OFF)
    if(LUX_EDITOR_LEGACY_ARCHITECTURE_TESTS)
        add_subdirectory(tests/architecture)
    endif()
    add_subdirectory(tests/persistence)
    add_subdirectory(tests/workspace)
endif()

option(LUX_EDITOR_LEGACY_PLUGIN_EXAMPLE "Build the previous external Editor extension example" OFF)
if(LUX_EDITOR_LEGACY_PLUGIN_EXAMPLE)
    set(LUX_SAMPLE_EDITOR ON)
    add_subdirectory(${PROJECT_SOURCE_DIR}/examples/render-plugin plugin-example EXCLUDE_FROM_ALL)
endif()
