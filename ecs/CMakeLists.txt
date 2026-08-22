message("-------- ECS Layer --------")

generate_visibility_header(
    ENABLE_MACRO_NAME   LUX_ECS_LIBRARY
    PUBLIC_MACRO_NAME   LUX_ECS_PUBLIC
    GENERATE_FILE_PATH  lux/engine/ecs/visibility.h
)
# ─────────────────────────────────────────────────────────────────────────────
#  ecs/ — the ECS layer (ADR: lux-engine-ecs-layer-architecture).
#
#  ECS is a LAYER, not a few modules inside function: everything that "knows an
#  entity / component" lives here — the kernel (World/ISystem/registry), the
#  shared component substrate (Transform…), and one folder per System
#  (RenderSystem / PhysicsSystem / AnimationSystem / …). It sits ABOVE function
#  (which stays strictly ECS-free: Vulkan render graph, sampling math, input,
#  ui, shadergen) and BELOW the apps (engine/). External libs (Box2D/Jolt/
#  miniaudio) are the "low level" for physics/audio — so those get no function
#  module; their coupling lives here behind a backend seam.
#
#  namespace: flat `lux::ecs`, class names carry the domain qualifier
#  (…2D… vs unsuffixed 3D). Include prefix: `lux/engine/ecs/…`.
#  Replaces the retired `packs/` tier (its composition model — the registry +
#  SceneServices — is inherited by the runtime contribution host).
# ─────────────────────────────────────────────────────────────────────────────

# Stable identities are configured first. The ECS kernel and the LXES format
# both depend on them, but neither depends on the other.
add_subdirectory(identity)
add_subdirectory(scene_format)

# Explicit dependency order: core (kernel + 共享组件) → dimensional render targets.
# Explicit rather than subdirectory_list so this ordering is guaranteed.
#
# ★ 这里此前先加一个 `components` 目标（Transform2D/3D + 派生 Transform +
#   PrimaryCameraTag）。它折进 `core` 了（批 6-D）：`core` 本来就有一个
#   `components/`（Hierarchy / Name），于是「共享组件」有**两个家**。实测那 6 个
#   消费者全部已经链 `core`，所以折叠零新依赖，且 include 路径
#   `lux/engine/ecs/components/…` 一个字都不用改。
#   `PrimaryCameraTag` 不是共享组件（实测只有 render 与宿主用它），归 `ecs/render`。
if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    # Offline EntityScene cooks serialize the concrete built-in schema set.
    # Configure those source targets and their ordinary reflection sidecars
    # honestly, while leaving scheduler behavior, 2D gameplay, Lua and render
    # extraction outside this product.
    add_subdirectory(core)
    add_subdirectory(serialization)
    add_subdirectory(entity_scene)
    add_subdirectory(spatial3d)
    add_subdirectory(terrain)
    add_subdirectory(physics3d)
    add_subdirectory(navigation)
    add_subdirectory(render)

    set(LUX_ECS_COMPONENTS
        identity
        scene_format
        core
        component_archive
        entity_scene
        spatial3d_components
        terrain_components
        physics3d
        navigation3d
        render_components
        render_components_3d
    )
else()
    add_subdirectory(core)
    add_subdirectory(serialization)
    add_subdirectory(entity_scene)
    add_subdirectory(transform)
    add_subdirectory(spatial2d)
    add_subdirectory(spatial3d)
    add_subdirectory(tilemap)
    add_subdirectory(pixel)
    add_subdirectory(terrain)
    add_subdirectory(script)    # per-entity script behaviors + ScriptEventRegistry
    add_subdirectory(physics)     # 中立层:定步累加器 + 碰撞探针缝。零第三方依赖
    add_subdirectory(physics2d)   # Box2D。只依赖中立 physics；可选跨域适配在 integration/
    add_subdirectory(physics3d)   # Jolt。今天零用户 —— 拆出来正是为了让它掉出 2D 构建
    add_subdirectory(navigation)  # NavigationRegion3D ECS owner fixture
    add_subdirectory(render)
    add_subdirectory(animation)   # dimensional animation targets follow render/
    add_subdirectory(integration) # 显式叶节点：可选 ECS 领域之间的组合，不被领域反向依赖

    set(LUX_ECS_COMPONENTS
        identity
        scene_format
        core component_archive entity_scene transform spatial2d_components spatial2d_streaming spatial3d_components spatial3d_streaming
        tilemap tilemap_streaming pixel pixel_streaming terrain_components
        scripting scripting_lua scripting_native
        physics physics2d physics3d
        navigation3d
        render_components render_components_2d render_components_3d
        render_core render_2d render_3d render_pixel_integration
        render_tilemap_integration
        animation_2d animation_3d
        physics2d_pixel_integration physics2d_script_integration presentation_2d
    )
endif()
# Product composition calls these ECS installers directly. No registry or
# package-shaped Runtime identity sits between ScheduleBuilder and Systems.

install_components(
    PROJECT_NAME    lux-engine-ecs
    VERSION         0.0.1
    NAMESPACE       lux::engine::ecs
    COMPONENTS      ${LUX_ECS_COMPONENTS}
)
