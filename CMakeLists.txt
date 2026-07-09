cmake_minimum_required(VERSION 3.22)
project(lux-engine)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RELEASE")
endif()

if(WIN32) 
    set(CMAKE_DEBUG_POSTFIX d)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add our cmake modules to the module path
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(CMAKE_TOOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tools)
set(PROJECT_NAMESPACE lux::engine)

# load the build system helper
find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-cxx                  REQUIRED
    COMPONENTS
    compile_time
    reflection_generator
)

# The editor tier (engine/) is built by default. Turn this OFF for a
# runtime-only build: the editor UI panels and the FlowForge MLIR compiler are
# excluded, and the resulting link commands carry no imgui_node_editor / MLIR /
# LLVM. (engine/asset_pipeline still builds — it is a build-time tool the
# runtime itself depends on; see the foreach loop below.)
option(LUX_BUILD_EDITOR "Build the editor tier (engine/: editor UI panels + FlowForge MLIR compiler)" ON)

set(lux_engine_layers
    "platform"
    "core"
    "resource"
    "function"
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Enable CTest so module test suites can register via add_test() and CI can run
# them with `ctest` from the build root. Must be called in the top-level source
# directory, before the add_subdirectory() calls below, for CTest to aggregate
# tests across all modules. Modules without add_test() contribute nothing, so
# this is a no-op for the runtime-only build. Render registers its suite under
# the ENABLE_RENDER_TEST gate (modules/function/render/test/CMakeLists.txt).
enable_testing()

foreach(layer ${lux_engine_layers})
    add_subdirectory(modules/${layer})
    if(layer STREQUAL "resource")
        # engine/asset_pipeline defines lux_asset_packer, a build-time dependency
        # of the function layer: function/render embeds builtin shaders through
        # it and resource::asset's add_asset() helper drives it. It must exist as
        # a target before the function layer is configured. It links only
        # resource::asset, so it is built regardless of LUX_BUILD_EDITOR.
        add_subdirectory(engine/asset_pipeline)
    endif()
endforeach()

# The rest of the editor tier (engine-coupled editor panels + the FlowForge
# MLIR compiler) is processed after all runtime layers so its components can
# link against function::* / core::* / resource::* targets. The LUX_BUILD_EDITOR
# gate lives inside engine/CMakeLists.txt at component granularity.
add_subdirectory(engine)

# ──────────────────────────────────────────────────────────────────────────────
# Packaging (CPack) — one .deb PER LAYER.
#
# Each layer calls install_components(PROJECT_NAME lux-engine-<layer>), and
# install_components tags every file with a CPack component (<layer> with hyphens
# -> underscores, e.g. lux_engine_platform). CPACK_DEB_COMPONENT_INSTALL + IGNORE
# grouping then emit one .deb per layer, named via CPACK_DEBIAN_<COMP>_PACKAGE_NAME
# and wired with per-layer Depends. SHLIBDEPS adds the concrete system .so deps;
# the *-dev / inter-layer entries below are the build/transitive ones it can't infer.
#
# Runtime tier only for now (LUX_BUILD_EDITOR=OFF). Editor-tier components get
# their config in the editor phase. Configure with -DCMAKE_INSTALL_PREFIX=/usr.
# ──────────────────────────────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME    "lux-engine")
if(NOT DEFINED CPACK_PACKAGE_VERSION)
    set(CPACK_PACKAGE_VERSION "0.0.1")
endif()
set(CPACK_PACKAGE_VENDOR  "LUX-YU")
set(CPACK_PACKAGE_CONTACT "LUX-YU <chenhui.lux.yu@outlook.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "LUX engine (layered: platform/core/resource/function)")
set(CPACK_GENERATOR                "DEB")
set(CPACK_DEBIAN_FILE_NAME         "DEB-DEFAULT")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# one .deb per component, no grouping
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_GROUPING   IGNORE)
set(CPACK_COMPONENTS_ALL
    lux_engine_platform
    lux_engine_core
    lux_engine_resource
    lux_engine_function
)

# platform (bottom layer)
set(CPACK_DEBIAN_LUX_ENGINE_PLATFORM_PACKAGE_NAME    "lux-engine-platform")
set(CPACK_DEBIAN_LUX_ENGINE_PLATFORM_PACKAGE_DEPENDS "lux-cxx (>= 0.0.2), libglfw3-dev, libvulkan-dev")
# core -> platform
set(CPACK_DEBIAN_LUX_ENGINE_CORE_PACKAGE_NAME    "lux-engine-core")
set(CPACK_DEBIAN_LUX_ENGINE_CORE_PACKAGE_DEPENDS "lux-engine-platform, lux-cxx (>= 0.0.2), libeigen3-dev, libluajit-5.1-dev, entt")
# resource -> core, platform
set(CPACK_DEBIAN_LUX_ENGINE_RESOURCE_PACKAGE_NAME    "lux-engine-resource")
set(CPACK_DEBIAN_LUX_ENGINE_RESOURCE_PACKAGE_DEPENDS "lux-engine-core, lux-engine-platform, lux-cxx (>= 0.0.2), stduuid, libassimp-dev, libvulkan-dev")
# function -> core, resource, platform
set(CPACK_DEBIAN_LUX_ENGINE_FUNCTION_PACKAGE_NAME    "lux-engine-function")
set(CPACK_DEBIAN_LUX_ENGINE_FUNCTION_PACKAGE_DEPENDS "lux-engine-core, lux-engine-resource, lux-engine-platform, lux-cxx (>= 0.0.2), imgui, libvulkan-dev")

include(CPack)
