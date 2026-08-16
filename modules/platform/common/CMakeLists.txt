# Component: common
# -----------------
# Pure C++ foundational types shared across platform, resource, and function layers.
# Zero external dependencies — no Vulkan, no Eigen, no GLFW.

add_interface_component(
    COMPONENT_NAME common
    NAMESPACE      lux::engine::platform
)

# ── std::format / libfmt 兼容层(lux::format)──────────────────────────
# 模板与生成归本组件所有:检测结果生成 <lux/engine/platform/FormatCompat.h>,
# 随本组件导出与安装 —— 任何层链接 lux::engine::platform::common 即得
# lux::format。(此前仓根用全局 include_directories 挂了一份、编辑器还有
# 一份私有复制;两处都已收编到这里 —— 头文件该归谁就住谁家。)
include(${CMAKE_SOURCE_DIR}/cmake/CheckFormatSupport.cmake)
check_format_support()
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/FormatCompat.h.in
    ${LUX_GENERATE_HEADER_DIR}/lux/engine/platform/FormatCompat.h
)

component_include_directories(
    common
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

lux_classify_target(
    TARGET  common
    LAYER   PLATFORM
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)
