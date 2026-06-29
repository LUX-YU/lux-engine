## engine/execution — 引擎异步运行时(stdexec/P2300)。
## 唯一 CPU 池 + 主线程汇合点 + 资产懒加载的外部编排者(EngineExecutor)。
## STATIC 组件,链进 lux_editor / lux_launcher。**stdexec 严格收敛在本库的 .cpp**:
## 公开头 EngineExecutor.hpp 是 stdexec-free pimpl,故 /Zc 一致性开关 PRIVATE,
## 消费者(editor/launcher)不被 stdexec 头 + 开关传染。

find_package(stdexec CONFIG REQUIRED)

set(LUX_EXECUTION_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/EngineExecutor.cpp
)

add_component(
    COMPONENT_NAME  execution
    STATIC
    NAMESPACE       lux::engine
    SOURCE_FILES    ${LUX_EXECUTION_SRCS}
)

component_include_directories(
    execution
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME        include
)

target_link_libraries(
    execution
    PUBLIC
    lux::engine::resource::asset   # 公开头用 asset_id_t;.cpp 用 AssetManager/decodeAssetData/AssetVfs
    lux::engine::function::render  # 公开头 RenderRequestSender.hpp 含 RenderRequest.hpp(纯头、无 Vulkan;
                                   # editor/launcher 本就链 render)
    PRIVATE
    STDEXEC::stdexec
)

# stdexec 重模板 + 两阶段查找:仅本库 .cpp 编译需要这些一致性开关(头 stdexec-free → PRIVATE)。
if(MSVC)
    target_compile_options(execution PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor)
endif()

set_target_properties(execution PROPERTIES POSITION_INDEPENDENT_CODE ON)

# 安装-期消费者链 execution(STATIC → PRIVATE 依赖以 $<LINK_ONLY> 留在 export 集)
# 需要能找到 stdexec 与 resource::asset。
component_add_transitive_commands(
    execution
    "find_package(stdexec CONFIG REQUIRED)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-function REQUIRED COMPONENTS render)"
)

# Unit tests (opt-in). Self-checking exes; render_request_sender_test compiles the
# RenderRequestSender adapter (the compile IS the stdexec-conformance check) and
# drives it deterministically headless (no GPU/server). The test carries its own
# stdexec link + MSVC /Zc flags (the adapter header is an opt-in stdexec includer).
set(ENABLE_EXECUTION_TEST OFF CACHE BOOL "Enable execution tests")
if(ENABLE_EXECUTION_TEST)
    add_subdirectory(test)
endif()
