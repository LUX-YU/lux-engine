# Component: filewatch
# --------------------
# 跨平台文件系统监视(efsw 后端:Win32 ReadDirectoryChangesW / inotify /
# FSEvents / kqueue,自带 generic 轮询兜底)。efsw 类型不出 .cpp,上层只见
# lux::platform::FileWatcher(监视线程 → drain() 主线程排空)。
#
# Android 不构建:唯一消费者是编辑器(资产热更新触发源),运行时宿主不需要;
# efsw 也没有为 arm64-android 三元组安装。要在移动端做热更新时再评估。
if(ANDROID)
    return()
endif()

find_package(efsw CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME   LUX_PLATFORM_FILEWATCH_LIBRARY
    PUBLIC_MACRO_NAME   LUX_PLATFORM_FILEWATCH_PUBLIC
    GENERATE_FILE_PATH  lux/engine/filewatch/visibility.h
)

add_component(
    COMPONENT_NAME filewatch
    NAMESPACE      lux::engine::platform
    SOURCE_FILES   src/FileWatcher.cpp
)

component_include_directories(
    filewatch
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(
    filewatch
    PRIVATE
    LUX_PLATFORM_FILEWATCH_LIBRARY
)

target_link_libraries(
    filewatch
    PRIVATE
    efsw::efsw
)

component_add_transitive_commands(
    filewatch
    "find_package(efsw CONFIG REQUIRED)"
)

lux_classify_target(
    TARGET  filewatch
    LAYER   PLATFORM
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)
