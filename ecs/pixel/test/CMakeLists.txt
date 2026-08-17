# ─────────────────────────────────────────────────────────────────────────────
#  ecs/pixel/test — test policy (2026-07-06)
#
#  Functional-verification tests are DEVELOPMENT-TIME artifacts here: they gate a
#  change while it is being built (including mutation verification) and are
#  DELETED once the change has landed green. Only stress / performance targets
#  and interactive demos stay in-tree permanently — gameplay currently has none.
#
#  HeadlessBridgeFixture.hpp is kept: it is development INFRASTRUCTURE (a real
#  RenderFrameSession + generic fake RenderServer + recording dispatcher for headless,
#  GPU-free bridge round-trips), not a test. The upcoming 2D slices (F2-08
#  PixelField2DSubsystem, A2-02 Tilemap2DSubsystem) drive their development-time tests
#  through it, exactly as the Gate -1 / Slice A bridge tests did.
# ─────────────────────────────────────────────────────────────────────────────

# PERMANENT stress: I2-02 parallel-CA determinism (1 ≡ N threads, hash-checked).
# Thread-schedule races regress silently under rule changes — this stays.
add_executable(pixel_parallel_determinism_stress pixel_parallel_determinism_stress.cpp)
target_link_libraries(pixel_parallel_determinism_stress PRIVATE lux::engine::ecs::pixel)

add_executable(pixel_sparse_runtime_test pixel_sparse_runtime_test.cpp)
target_link_libraries(pixel_sparse_runtime_test PRIVATE lux::engine::ecs::pixel)

add_executable(pixel_field_system_test pixel_field_system_test.cpp)
target_link_libraries(pixel_field_system_test PRIVATE lux::engine::ecs::pixel)

# PERMANENT:渲染桥不变量的常驻回归网(①-⑪,见文件头目录)。最初是
# DEVELOPMENT-TIME 探针,但此后每条工作线都把自己「测试与实机日志都抓不到」
# 的不变量钉在这里(引用计数端到端/attach 解析序/进程域共享缓存跨世界语义),
# 它已是这些语义唯一的自动化覆盖 —— 与 pixel_parallel_determinism_stress
# 同级常驻,顶部政策对它不再适用。
add_executable(render_subsystem_probes render_subsystem_probes.cpp)
target_link_libraries(render_subsystem_probes
    PRIVATE
    lux::engine::ecs::render_2d
    lux::engine::ecs::render_3d
    lux::engine::ecs::render_pixel_integration
    lux::engine::ecs::render_tilemap_integration
    lux::engine::core::events   # 探针自建 bus+pump(批E:资产广播经总线)
    # 驻留三件套(T12:探针走真装配)。裸目标名:ecs 层先于 engine 层配置,
    # CMake 对非命名空间目标名在生成期解析(flowforge 同款先例)。
    runtime_render_scene_test_support
    lux::engine::runtime::runtime_execution
    lux::engine::resource::asset_codecs
)
# ★ 变更驱动的两张安全网在**这个目标上常开**(`LUX_ECS_EXTRACTION_VERIFY`):
#     · 抽取 oracle —— 反应式排空之后再全扫一遍,还找得到活干 = 漏了信号源;
#     · 变换 oracle —— 被快闸门跳过的实体 pose 其实变了 = 有写入方没设 dirty。
#
#   为什么要钉在 CI 里而不是「需要时手动打开」:它上线以来抓到过 4 处,其中
#   两处是**引擎自己的 bug**(Physics2DWorld 的角色控制器写回漏设 dirty、
#   FlipbookAnimationSystem 直写 Image2DComponent 不发信号),而且这两处在
#   各自被引入时**全部测试都是绿的** —— 它们防的正是「不报错」那一类。
#   靠人记得跑,等于没有。
#
#   ⚠️ oracle 只往 stderr 打印,自身不返回失败码(刻意:它在实机构型下是
#   诊断而不是断言)。所以这里用 ctest 的 FAIL_REGULAR_EXPRESSION 把
#   「打印了」变成「测试红」。
target_compile_definitions(render_subsystem_probes PRIVATE LUX_ECS_EXTRACTION_VERIFY=1)
if(MSVC)
    # The permanent probe intentionally instantiates every extraction path in
    # one TU.  /Od emits enough COMDAT sections to exceed classic COFF limits.
    target_compile_options(
        render_subsystem_probes
        PRIVATE
        "$<$<CONFIG:Debug>:/bigobj>"
    )
endif()


# Permanent contract probe for the ECS/render RPC boundary. In particular,
# RenderRequest::then may run synchronously and create replies must remain
# observable after their entity intent is abandoned.
add_executable(render_request_lifecycle_probe render_request_lifecycle_probe.cpp)
target_link_libraries(render_request_lifecycle_probe
    PRIVATE
    lux::engine::ecs::render_core
)

# Permanent split-frame lifecycle probe. It drives dispatch and reply pumping
# separately so entity/system teardown can occur in the otherwise invisible
# window between server-side creation and client-side ownership publication.
add_executable(render_subsystem_lifecycle_probe
    render_subsystem_lifecycle_probe.cpp)
target_link_libraries(render_subsystem_lifecycle_probe
    PRIVATE
    lux::engine::ecs::render_2d
    lux::engine::ecs::render_3d
    lux::engine::ecs::render_pixel_integration
    lux::engine::ecs::render_tilemap_integration
    lux::engine::function::render_vulkan
    lux::engine::resource::asset_codecs
)

# Private composition contract shared by the Pixel and Tilemap render leaves.
# Keeping the test outside either public API proves the cache is not a new
# inheritance surface or a domain-visible grid manager.
add_executable(sparse_canvas_atlas_cache_test
    sparse_canvas_atlas_cache_test.cpp)
target_include_directories(sparse_canvas_atlas_cache_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../render/pinclude)
