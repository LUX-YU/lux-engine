#pragma once
#include <cstdint>
#include <lux/engine/function/render/client/core/EngineSetSlot.hpp>

// Transitional: the single source of truth for descriptor layouts is migrating to
// the description layer's LayoutContract (logical resource registry + frequency
// domain + canonical slots). This header's slot/binding enums remain the runtime
// layout's authority until that migration completes; new code should reconcile
// against LayoutContract entries first.
// See .internal/lux-engine-descriptor-layout-implementation-checklist.md.
#include <lux/engine/description/LayoutContract.hpp>

namespace lux::render
{
    // (EDescriptorSetSlot / kDescriptorSetCount / kAllSetsMask 已拆到
    //  core/EngineSetSlot.hpp —— 作者环只需要槽位枚举,不需要整张契约表。
    //  本头经上方 include 继续原样提供它们,包含者零感知。)

    // ---------------------------------------------------------------
    // Per-set binding enums (individual bindings within each set)
    // ---------------------------------------------------------------

    // SET 0
    enum class ESceneSetBindings : uint8_t
    {
        GLOBAL = 0, ///< SceneGlobalGpuData[] SoA SSBO (time, delta_time, frame_number) — indexed by scene_index push
                    ///< constant
        VIEW =
            1, ///< ViewGpuData[] SoA SSBO (view/proj matrices, cam_pos, viewport) — indexed by view_index push constant
        COUNT
    };

    // SET 1 — Instance
    // (原此处的 EGeneralSetBindings{INSTANCES=0} 已删:零引用,**而且是错的** ——
    //  它声称 set 1 只有 1 条 binding,实际 engine_sets::kInstance 有 2 条
    //  (b0 transform SoA、b1 property SoA)。一个没人用的名字只是噪音;
    //  一个没人用**又对不上**的名字是误导。名字的缺席由 B 表补上,
    //  见 gpu/pipeline/EngineSetShapes.hpp 的 engine_sets::kInstance。)

    // SET 2
    enum class ETextureSetBindings : uint8_t
    {
        TEXTURES = 0,      ///< sampler2D[] (bindless 2D textures)
        CUBE_TEXTURES = 1, ///< samplerCube[] (bindless cubemaps)
        COUNT
    };

    // SET 3
    enum class ELightSetBindings : uint8_t
    {
        LIGHT_SPOT = 0,
        LIGHT_DIRECTIONAL = 1,
        LIGHT_POINT = 2,
        LIGHT_AREA = 3,
        SHADOW_SLICES = 4,       ///< Shadow slice SSBO (per-slice VP + bias)
        SHADOW_ATLAS = 5,        ///< PCF atlas: sampler2DArrayShadow (D32_SFLOAT)
        SHADOW_CONFIG = 6,       ///< Shadow configuration UBO
        SHADOW_SPOT_MAP = 7,     ///< Spot light slot -> shadow slice index (-1 = none)
        SHADOW_POINT_MAP = 8,    ///< Point light slot -> base slice index (-1 = none)
        SHADOW_ATLAS_EVSM = 9,   ///< EVSM blurred moments: sampler2DArray (RGBA16F)
        SHADOW_EVSM_CONFIG = 10, ///< EVSM exponents + bleed reduction UBO
        SHADING_INPUTS = 11,     ///< Screen-space shading input array (AO...; see ShadingInputSlot.hpp)
        COUNT
    };

    // SET 4 — Material Family SSBO bindings
    // Binding index = static_cast<uint32_t>(ELightingTechnique::xxx)
    // (See MaterialFamily.hpp for ELightingTechnique)
    // 0=Unlit 1=LegacyLit 2=PBR 3=Stylized 4=Graph
    inline constexpr uint32_t kMaterialFamilyBindingCount = 5;

    // SET 5 / SET 6 — **占了槽位号,但完全没有人写、也没有人读**
    //
    // 原此处有 EParticleSetBindings{PARTICLES} 与
    // EComputeSetBindings{CULL_UBO, INSTANCE_DATA, DRAW_COMMANDS, VISIBLE_INDICES}。
    // 两组都是零引用。查证过它们描述的槽位本身的状态,不只是符号的引用数:
    //
    //   · 没有任何着色器声明 `set = 5` 或 `set = 6`
    //   · getParticleSetLayout() / getComputeSetLayout() 定义了但**从未被调用**
    //     (已随本次一并删除)
    //   · GPU 驱动剔除实际走的是特性自声明的 set(EPlannedSetKind::FeatureExplicit),
    //     不是这里预留的 6 号
    //
    // 也就是说 EComputeSetBindings 那 4 条 binding 描述的是一个**没发生的设计**。
    // 布局仍会被建出来(engine_sets 里的 kParticle/kCompute 还在),因为槽位号在
    // kEngineSetShapes 里是密集数组下标,动一个就是全体重排 —— 那是另一个话题。
    // 这里删掉的只是**对不存在之物的命名**。
    //
    // ⚠️ 别再加回来。要给某个 set 命名之前先确认它真的有人写。

    // SET 7 — Bindless vertex source array (R1.4 of render-refactor).
    // One SSBO binding with descriptorCount = kVertexPoolMaxCount, marked
    // PARTIALLY_BOUND so registered pools sit at their assigned indices and
    // unbound slots are valid (read by no shader). VARIABLE_DESCRIPTOR_COUNT
    // keeps Vulkan validation happy with the variable-size array.
    enum class EVertexPoolSetBindings : uint8_t
    {
        VERTEX_POOLS = 0, ///< SSBO[kVertexPoolMaxCount] of vertex pool buffers
        COUNT
    };

    /// Maximum number of vertex sources the bindless array can hold.
    /// 16 = 1 static segment + ~15 transient producers; bump when needed.
    inline constexpr uint32_t kVertexPoolMaxCount = 16;

    // ---------------------------------------------------------------
    // Compile-time mapping: binding enum → descriptor set index.
    // Values are derived from EDescriptorSetSlot to keep a single
    // source of truth.
    // ---------------------------------------------------------------
    // 特化只覆盖上面**还活着的** binding 枚举。三条随枚举一起删了
    //(Instance / Particle / Compute)—— 不是"没人用所以删",是被映射的那一端
    // 已经不存在了。
    template <typename T> struct get_binding_set;
    template <> struct get_binding_set<ESceneSetBindings>
    {
        static constexpr uint32_t value = static_cast<uint32_t>(EDescriptorSetSlot::Scene);
    };
    template <> struct get_binding_set<ETextureSetBindings>
    {
        static constexpr uint32_t value = static_cast<uint32_t>(EDescriptorSetSlot::Texture);
    };
    template <> struct get_binding_set<ELightSetBindings>
    {
        static constexpr uint32_t value = static_cast<uint32_t>(EDescriptorSetSlot::Light);
    };
    template <> struct get_binding_set<EVertexPoolSetBindings>
    {
        static constexpr uint32_t value = static_cast<uint32_t>(EDescriptorSetSlot::VertexPool);
    };
}
