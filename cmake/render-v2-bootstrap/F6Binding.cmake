# Isolated installed consumer of the Native binding value/API boundary.
# Data-only consumers continue to use RenderData without Native dependencies.
get_filename_component(f6_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set_target_properties(render_vulkan_graph PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
    "$<BUILD_INTERFACE:${f6_root}/modules/function/render/vulkan/include>;$<BUILD_INTERFACE:${f6_root}/engine/toolchain/shader/include>;$<INSTALL_INTERFACE:include>"
)

install(TARGETS render_graph render_vulkan render_vulkan_graph
    EXPORT RenderBindingTargets ARCHIVE DESTINATION lib COMPONENT RenderBindingSDK
)

install(EXPORT RenderBindingTargets NAMESPACE lux::engine::render::
    DESTINATION lib/cmake/RenderBinding COMPONENT RenderBindingSDK
)

install(DIRECTORY "${f6_root}/modules/function/render/graph/include/"
    "${f6_root}/modules/function/render/vulkan/include/"
    DESTINATION include COMPONENT RenderBindingSDK
)

install(FILES "${f6_root}/modules/resource/description/include/lux/engine/description/PassContract.hpp"
    "${f6_root}/modules/resource/description/include/lux/engine/description/Image.hpp"
    DESTINATION include/lux/engine/description COMPONENT RenderBindingSDK
)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/RenderBindingConfig.cmake" [=[
include(CMakeFindDependencyMacro)
find_dependency(RenderData CONFIG)
find_dependency(Vulkan 1.3)
find_dependency(VulkanMemoryAllocator CONFIG)
include("${CMAKE_CURRENT_LIST_DIR}/RenderBindingTargets.cmake")
]=])

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/RenderBindingConfig.cmake"
    DESTINATION lib/cmake/RenderBinding COMPONENT RenderBindingSDK
)

install(FILES "${f6_root}/engine/toolchain/shader/include/lux/engine/toolchain/shader/ShaderAssets.hpp"
    "${f6_root}/engine/toolchain/shader/include/lux/engine/toolchain/shader/SpirvRelocation.hpp"
    "${f6_root}/engine/toolchain/shader/include/lux/engine/toolchain/shader/PassValidation.hpp"
    "${f6_root}/engine/toolchain/shader/include/lux/engine/toolchain/shader/DataValidation.hpp"
    DESTINATION include/lux/engine/toolchain/shader COMPONENT RenderBindingSDK
)
