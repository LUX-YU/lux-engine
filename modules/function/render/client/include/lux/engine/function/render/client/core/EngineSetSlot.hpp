#pragma once
// =============================================================================
//  EngineSetSlot.hpp — 引擎共享描述符 set 的槽位枚举(仅此而已)
// -----------------------------------------------------------------------------
//  从 DescriptorSetLayoutContract.hpp 拆出。为什么值得单独一个头:
//
//  树外作者的**作者环**(RenderFeature → RenderFeature → RenderContextView)只
//  需要这个枚举 —— `RenderContextView::engineSetLayout(EDescriptorSetSlot)` 是
//  实证在用的外部 API(lux-robotics 的 GaussianSplatFeature 以它绑引擎 Scene 集)。
//  而完整的契约头会把 per-set binding 枚举与 description 模块的
//  LayoutContract(逻辑资源表)一并拖进来 —— 那些是**图纸环**的内容,声明一个
//  特性的人不需要。拆出后,作者环的传递闭包不再含 `description/`。
//
//  契约头 include 本头,对既有包含者(含树外)完全透明。
// =============================================================================
#include <cstdint>

namespace lux::render
{
    // ---------------------------------------------------------------
    // Central descriptor set slot assignment.
    // To add a new descriptor set, insert an entry before COUNT and
    // define the corresponding binding enum + get_binding_set<> in
    // DescriptorSetLayoutContract.hpp.
    // ---------------------------------------------------------------
    enum class EDescriptorSetSlot : uint32_t
    {
        Scene = 0,
        Instance = 1,
        Texture = 2,
        Light = 3,
        Material = 4,
        Particle = 5,   ///< GPU particle SSBO (compute simulation + vertex billboard)
        Compute = 6,    ///< GPU-driven compute cull descriptor set
        VertexPool = 7, ///< Bindless vertex source array (R1.4 of render-refactor;
                        ///<   reads in VERTEX_BIT + COMPUTE_BIT; one descriptorCount
                        ///<   slot per registered IVertexSource)
        COUNT           // <-- add new sets before this
    };

    /// Total number of descriptor set slots (derived from EDescriptorSetSlot::COUNT).
    inline constexpr uint32_t kDescriptorSetCount = static_cast<uint32_t>(EDescriptorSetSlot::COUNT);

    /// Bitmask covering all descriptor set slots (e.g. 0x1F for 5 sets).
    inline constexpr uint32_t kAllSetsMask = (1u << kDescriptorSetCount) - 1u;
}
