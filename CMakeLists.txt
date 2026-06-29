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
