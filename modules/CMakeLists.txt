add_subdirectory(platform)

# Script ABI/signature primitives are L0 foundation even though their physical
# package path remains under function/script for this migration.  Configure the
# target before Core's cold meta adapter and Resource descriptions so they can
# depend on the durable vocabulary without depending on a backend.
add_subdirectory(function/script/core)
add_subdirectory(core)

add_subdirectory(resource)

# Host asset tools must exist before Function/Render declares cooked-content
# custom commands. This is a build-order seam only: no Runtime target links a
# Toolchain target. Cross builds resolve the installed host executable through
# LUX_HOST_TOOLS_PREFIX instead of configuring a target binary.
if(NOT CMAKE_CROSSCOMPILING AND
   (LUX_PROFILE_HAS_TOOLCHAIN OR LUX_BUILD_PACKED_RENDER_CONTENT))
    add_subdirectory(
        "${CMAKE_SOURCE_DIR}/engine/toolchain/asset"
        "${CMAKE_BINARY_DIR}/engine/toolchain/asset"
    )
endif()

add_subdirectory(function)
