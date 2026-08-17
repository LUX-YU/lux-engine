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

# Source files are always UTF-8 (the whole repo has been verified to contain
# no non-UTF-8 files).
#
# Without this flag, MSVC interprets source code using the [system code page]
# — on Chinese Windows that is 936 (GBK), so UTF-8 Chinese comments get
# misread as GBK byte pairs. Most of the time that is just harmless garbling,
# but if a byte pair's trailing byte happens to be 0x5C (backslash) and lands
# at the end of a `//` comment line, it triggers line continuation and eats
# the next line of code — showing up as bizarre, unrelated errors like
# "C2447: '{': missing function header" or "C3646: unknown override specifier".
#
# Worse, it is [not reproducible]: the code page depends on the shell/
# environment, so the same code compiles fine one time and fails the next,
# and it only blows up when that translation unit is actually recompiled — so
# it can lie dormant for a long time. That is exactly what happened on
# 2026-07-19: touching one header's mtime triggered a recompile, and three
# files that had "always been fine" all failed at once.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Add our cmake modules to the module path
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(TargetArchitecture)
include(TargetPlatform)
include(HostTools)
lux_detect_build_platform()
cmake_language(DEFER CALL lux_validate_foundation_targets)
cmake_language(DEFER CALL lux_validate_target_dag)
cmake_language(DEFER CALL lux_validate_all_production_targets)
cmake_language(DEFER CALL lux_validate_ecs_dimension_closures)
cmake_language(DEFER CALL lux_write_unclassified_target_report)

set(CMAKE_TOOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tools)
set(PROJECT_NAMESPACE lux::engine)

# load the build system helper
find_package(lux-cmake-toolset CONFIG REQUIRED)
# reflection_generator is the HOST meta-generator toolchain (libclang-based;
# its FindLibClang is REQUIRED). A cross build (Android) never runs it on the
# target — generated meta sources come from the host side — so requesting it
# there just fails on a libclang that has no target-arch package. Host builds
# keep today's exact request.
if(CMAKE_CROSSCOMPILING)
    find_package(lux-cxx                  REQUIRED
        COMPONENTS
        core
        memory
        binary
        time
        units
        diagnostic
        abi
        compile_time
        algorithm
    )
else()
    find_package(lux-cxx                  REQUIRED
        COMPONENTS
        core
        memory
        binary
        time
        units
        diagnostic
        abi
        compile_time
        algorithm
        reflection_generator
    )
endif()

# (std::format 兼容层已下沉到 modules/platform/common —— 生成与安装随
#  组件走,消费方链接 lux::engine::platform::common 即得 lux::format;
#  此前这里的全局 include_directories 挂法与编辑器私有复制均已收编。)

# Product profiles are explicit: a Boolean "editor on/off" cannot express a
# toolchain-only host, Android's host-tool split, or a runtime-clean Player
# built in the same developer tree as the Editor.
set(LUX_BUILD_PROFILE "DEVELOPER" CACHE STRING "lux-engine product profile")
set_property(
    CACHE LUX_BUILD_PROFILE
    PROPERTY STRINGS DEVELOPER PLAYER EDITOR TOOLCHAIN
)
string(TOUPPER "${LUX_BUILD_PROFILE}" LUX_BUILD_PROFILE)
if(LUX_BUILD_PROFILE STREQUAL "ANDROID_PLAYER")
    message(FATAL_ERROR
        "LUX_BUILD_PROFILE=ANDROID_PLAYER has been removed. Use "
        "LUX_BUILD_PROFILE=PLAYER with the Android toolchain/triplet."
    )
endif()
set(_lux_valid_profiles
    DEVELOPER PLAYER EDITOR TOOLCHAIN
)
if(NOT LUX_BUILD_PROFILE IN_LIST _lux_valid_profiles)
    message(FATAL_ERROR
        "Unknown LUX_BUILD_PROFILE='${LUX_BUILD_PROFILE}'. Expected one of: "
        "${_lux_valid_profiles}"
    )
endif()
lux_validate_profile_platform("${LUX_BUILD_PROFILE}")
lux_configure_host_tools()

set(LUX_PROFILE_HAS_RUNTIME OFF)
set(LUX_PROFILE_HAS_REFERENCE_PLAYER OFF)
set(LUX_PROFILE_HAS_EDITOR OFF)
set(LUX_PROFILE_HAS_TOOLCHAIN OFF)
if(LUX_BUILD_PROFILE STREQUAL "DEVELOPER")
    set(LUX_PROFILE_HAS_RUNTIME ON)
    set(LUX_PROFILE_HAS_REFERENCE_PLAYER ON)
    set(LUX_PROFILE_HAS_EDITOR ON)
    set(LUX_PROFILE_HAS_TOOLCHAIN ON)
elseif(LUX_BUILD_PROFILE STREQUAL "PLAYER")
    set(LUX_PROFILE_HAS_RUNTIME ON)
    set(LUX_PROFILE_HAS_REFERENCE_PLAYER ON)
elseif(LUX_BUILD_PROFILE STREQUAL "EDITOR")
    set(LUX_PROFILE_HAS_RUNTIME ON)
    set(LUX_PROFILE_HAS_EDITOR ON)
    set(LUX_PROFILE_HAS_TOOLCHAIN ON)
elseif(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    set(LUX_PROFILE_HAS_TOOLCHAIN ON)
endif()

# Toolchain cooks publish generated component columns and therefore need the
# ordinary ECS reflection sidecars. Lua bindings are a Runtime/Editor product;
# keeping this switch off for the pure Toolchain profile prevents schema cooks
# from acquiring the Lua runtime merely because both sidecars share generators.
if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    set(LUX_BUILD_COMPONENT_LUA_META OFF)
else()
    option(LUX_BUILD_COMPONENT_LUA_META
        "Build Lua component reflection sidecars" ON)
endif()
lux_write_build_facts("${LUX_BUILD_PROFILE}")

# Host-side material cooking is part of every Toolchain-capable product.  This
# decision must be made before modules/resource causes the host shader tools to
# be configured; setting it later in engine/CMakeLists.txt leaves the compiler
# target absent and lets CMake silently reinterpret its name as a raw library.
if(LUX_PROFILE_HAS_TOOLCHAIN)
    set(LUX_ENABLE_SHADERGEN ON CACHE BOOL
        "Build the shader compiler backend (toolchain-side)" FORCE)
endif()

set(lux_engine_layers
    "platform"
    "core"
    "resource"
    "function"
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Test executables and their source directories remain available for local,
# ad-hoc use, but are not part of the configured build validation suite.
foreach(layer ${lux_engine_layers})
    add_subdirectory(modules/${layer})
    if(layer STREQUAL "resource" AND NOT CMAKE_CROSSCOMPILING)
        # Shader reflection and .luxasset emission are host-side toolchain
        # concerns. Configure them before the asset packer and render shader
        # custom commands, while keeping them outside every Player link edge.
        add_subdirectory(engine/toolchain/shader)

        # The texture importer is a host build tool used by lux_asset_packer.
        # It is configured here, not as a Runtime link dependency.
        add_subdirectory(engine/toolchain/asset/texture)
        add_subdirectory(engine/toolchain/asset/cook)

        # engine/toolchain/asset/packer defines lux_asset_packer, a build-time dependency
        # of the function layer: function/render embeds builtin shaders through
        # it and resource::asset's add_asset() helper drives it. It must exist as
        # a target before the function layer is configured. It links only
        # resource::asset, so it is built in every host profile.
        # Cross builds must NOT build it for the target arch (it runs on the
        # host at build time) — with the target absent, embed_builtin_shaders /
        # add_asset fall back to find_program, which resolves the HOST-installed
        # lux_asset_packer via CMAKE_PREFIX_PATH (mobile adaptation, 2026-07-20).
        add_subdirectory(engine/toolchain/asset/packer)
    endif()
endforeach()

# ecs/ — Runtime configures the complete ECS product. Toolchain configures the
# explicit schema/meta subset consumed by EntityScene cooks; this is a source
# target dependency, never an implicit find_package of an installed SDK.
if(LUX_PROFILE_HAS_RUNTIME OR LUX_PROFILE_HAS_TOOLCHAIN)
    add_subdirectory(ecs)
endif()


# Runtime, authoring, toolchain, editor and host composition are selected by
# LUX_BUILD_PROFILE inside engine/CMakeLists.txt.
add_subdirectory(engine)

# Deployable leaf modules are configured after the complete public engine API.
# No engine target may depend on a concrete extension target.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/extensions/CMakeLists.txt)
    add_subdirectory(extensions)
endif()

# Per-target platform shells (APK/app scaffolding + bring-up harnesses).
#
# platforms/ is deliberately NOT modules/platform/: that one is the platform
# ABSTRACTION layer the engine links (window / gapi / dynamic_library), whereas
# this one holds the per-target PORT — manifest, packaging, native entry point —
# which nothing in the engine links. Same split Godot draws with platform/.
# Processed last: these shells link the runtime tier they smoke-test.
if(LUX_TARGET_OS STREQUAL "ANDROID")
    add_subdirectory(platforms/android)
endif()

# TOOLCHAIN is an offline product, not shorthand for "build every configured
# library except the Editor executable". Configure Resource formats, the
# schema-only ECS dependencies of cooks, Authoring models and offline
# transforms, then make its default build the recursive closure of Toolchain
# and host Build-Tool roots.
if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    lux_limit_toolchain_default_build()
endif()

# ──────────────────────────────────────────────────────────────────────────────
# Packaging (CPack) — source layers and executable products stay distinct.
#
# Library components remain independently reusable SDK packages. Product
# components contain only their composition executables/extensions and depend on
# the exact lower-layer packages they need. Toolchain/Editor components are not
# configured at all in PLAYER profiles.
# ──────────────────────────────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME    "lux-engine")
if(NOT DEFINED CPACK_PACKAGE_VERSION)
    set(CPACK_PACKAGE_VERSION "0.0.1")
endif()
set(CPACK_PACKAGE_VENDOR  "LUX-YU")
set(CPACK_PACKAGE_CONTACT "LUX-YU <chenhui.lux.yu@outlook.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "LUX engine runtime, SDK and product components")
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
if(LUX_PROFILE_HAS_RUNTIME OR LUX_PROFILE_HAS_TOOLCHAIN)
    list(APPEND CPACK_COMPONENTS_ALL lux_engine_ecs)
endif()
if(LUX_PROFILE_HAS_RUNTIME)
    list(APPEND CPACK_COMPONENTS_ALL
        lux_engine_runtime
        lux_engine_packs
        lux_engine_host
        lux_runtime
    )
endif()
if(LUX_PROFILE_HAS_REFERENCE_PLAYER)
    list(APPEND CPACK_COMPONENTS_ALL lux_player)
endif()
if(LUX_PROFILE_HAS_TOOLCHAIN)
    list(APPEND CPACK_COMPONENTS_ALL
        lux_engine_authoring
        lux_engine_toolchain
        lux_toolchain
    )
endif()
if(LUX_PROFILE_HAS_RUNTIME)
    list(APPEND CPACK_COMPONENTS_ALL lux_sdk)
endif()
if(LUX_PROFILE_HAS_EDITOR)
    list(APPEND CPACK_COMPONENTS_ALL
        lux_engine_editor
        lux_editor
    )
endif()

# platform (bottom layer).  The TOOLCHAIN profile installs only platform/common
# and therefore must not acquire GLFW/Vulkan package dependencies by metadata.
set(CPACK_DEBIAN_LUX_ENGINE_PLATFORM_PACKAGE_NAME "lux-engine-platform")
if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    set(CPACK_DEBIAN_LUX_ENGINE_PLATFORM_PACKAGE_DEPENDS
        "lux-cxx (>= 0.0.2)"
    )
else()
    set(CPACK_DEBIAN_LUX_ENGINE_PLATFORM_PACKAGE_DEPENDS
        "lux-cxx (>= 0.0.2), libglfw3-dev, libvulkan-dev"
    )
endif()
# core -> platform
set(CPACK_DEBIAN_LUX_ENGINE_CORE_PACKAGE_NAME    "lux-engine-core")
set(CPACK_DEBIAN_LUX_ENGINE_CORE_PACKAGE_DEPENDS "lux-engine-platform, lux-cxx (>= 0.0.2), libeigen3-dev, entt")
# resource -> core, platform
set(CPACK_DEBIAN_LUX_ENGINE_RESOURCE_PACKAGE_NAME    "lux-engine-resource")
set(CPACK_DEBIAN_LUX_ENGINE_RESOURCE_PACKAGE_DEPENDS "lux-engine-core, lux-engine-platform, lux-cxx (>= 0.0.2), stduuid")
# function -> core, resource, platform
set(CPACK_DEBIAN_LUX_ENGINE_FUNCTION_PACKAGE_NAME "lux-engine-function")
if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    set(CPACK_DEBIAN_LUX_ENGINE_FUNCTION_PACKAGE_DEPENDS
        "lux-engine-core, lux-engine-resource, lux-engine-platform, lux-cxx (>= 0.0.2)"
    )
else()
    set(CPACK_DEBIAN_LUX_ENGINE_FUNCTION_PACKAGE_DEPENDS
        "lux-engine-core, lux-engine-resource, lux-engine-platform, lux-cxx (>= 0.0.2), imgui, libvulkan-dev"
    )
endif()

set(CPACK_DEBIAN_LUX_ENGINE_ECS_PACKAGE_NAME    "lux-engine-ecs")
set(CPACK_DEBIAN_LUX_ENGINE_ECS_PACKAGE_DEPENDS "lux-engine-function, lux-engine-resource, lux-engine-core, entt")
set(CPACK_DEBIAN_LUX_ENGINE_RUNTIME_PACKAGE_NAME    "lux-engine-runtime")
set(CPACK_DEBIAN_LUX_ENGINE_RUNTIME_PACKAGE_DEPENDS "lux-engine-ecs, lux-engine-function, lux-engine-resource, lux-engine-core")
set(CPACK_DEBIAN_LUX_ENGINE_PACKS_PACKAGE_NAME    "lux-engine-packs")
set(CPACK_DEBIAN_LUX_ENGINE_PACKS_PACKAGE_DEPENDS "lux-engine-runtime, lux-engine-ecs")
set(CPACK_DEBIAN_LUX_ENGINE_HOST_PACKAGE_NAME    "lux-engine-host")
set(CPACK_DEBIAN_LUX_ENGINE_HOST_PACKAGE_DEPENDS "lux-engine-runtime, lux-engine-packs")

set(CPACK_DEBIAN_LUX_RUNTIME_PACKAGE_NAME    "lux-runtime-extensions")
set(CPACK_DEBIAN_LUX_RUNTIME_PACKAGE_DEPENDS "lux-engine-runtime, lux-engine-ecs")
set(CPACK_DEBIAN_LUX_PLAYER_PACKAGE_NAME    "lux-player")
set(CPACK_DEBIAN_LUX_PLAYER_PACKAGE_DEPENDS "lux-engine-host, lux-engine-runtime, lux-engine-packs, lux-runtime-extensions")

set(CPACK_DEBIAN_LUX_ENGINE_AUTHORING_PACKAGE_NAME    "lux-engine-authoring")
set(CPACK_DEBIAN_LUX_ENGINE_AUTHORING_PACKAGE_DEPENDS "lux-engine-resource, lux-engine-function")
set(CPACK_DEBIAN_LUX_ENGINE_TOOLCHAIN_PACKAGE_NAME    "lux-engine-toolchain")
set(CPACK_DEBIAN_LUX_ENGINE_TOOLCHAIN_PACKAGE_DEPENDS "lux-engine-authoring, lux-engine-ecs, lux-engine-resource, lux-engine-function")
set(CPACK_DEBIAN_LUX_TOOLCHAIN_PACKAGE_NAME    "lux-toolchain")
set(CPACK_DEBIAN_LUX_TOOLCHAIN_PACKAGE_DEPENDS "lux-engine-toolchain")
set(CPACK_DEBIAN_LUX_ENGINE_EDITOR_PACKAGE_NAME    "lux-engine-editor")
set(CPACK_DEBIAN_LUX_ENGINE_EDITOR_PACKAGE_DEPENDS "lux-engine-authoring, lux-engine-toolchain, lux-engine-runtime")
set(CPACK_DEBIAN_LUX_EDITOR_PACKAGE_NAME    "lux-editor")
set(CPACK_DEBIAN_LUX_EDITOR_PACKAGE_DEPENDS "lux-engine-editor, lux-toolchain")
set(CPACK_DEBIAN_LUX_SDK_PACKAGE_NAME    "lux-sdk")
set(CPACK_DEBIAN_LUX_SDK_PACKAGE_DEPENDS "lux-engine-platform, lux-engine-core, lux-engine-resource, lux-engine-function, lux-engine-ecs, lux-engine-runtime")

include(CPack)
