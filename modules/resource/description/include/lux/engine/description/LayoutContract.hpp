#pragma once
// =============================================================================
//  LayoutContract.hpp — the descriptor layout contract
// -----------------------------------------------------------------------------
//  The SINGLE SOURCE OF TRUTH for descriptor set/binding layout. Defines two
//  things:
//    (a) Logical resources (LogicalResourceDesc) — every resource shared
//        across shader and C++: its name (the reconciliation key), kind,
//        update-frequency domain, and canonical slot;
//    (b) Frequency domains (EBindFrequency) — frozen domains (GLOBAL/
//        BINDLESS, where the canonical slot IS the final slot) versus free
//        domains (FEATURE/PASS_LOCAL, reassigned by LayoutPlan at graph
//        compile time).
//
//  Consumers (dependencies all point downward, which is exactly why this
//  table lives in the description layer):
//    render         — layout construction / binding recipes / LayoutPlan's
//                     allocation seed;
//    shadergen      — lux_shader_emitter injects canonical slots into .lglsl;
//    GLSL interface headers — the .lglslh declarations for frozen-domain
//                     resources are generated from this table.
//
//  [Canonical-slot evolution rule] The data here is currently v0 — a mirror
//  of the current layout (a precondition for byte-identical SPIR-V during the
//  migration). When the free domains later move to a <=4-set tiered layout
//  (see design doc §4.2), only this table's DATA changes — zero changes to
//  shader source, since .lglsl never embeds literal slot numbers. The name is
//  a permanent reconciliation key: renaming a shader declaration must update
//  this table (and vice versa); entries are only ever added, never removed in
//  spirit (retired resources are archived as comments, not deleted).
//
//  Design: .internal/lux-engine-descriptor-layout-architecture.md §4.1/§4.2
//  Implementation checklist: .internal/lux-engine-descriptor-layout-implementation-checklist.md
// =============================================================================

#include "ShaderInfo.hpp"   // EDescriptorType — shares its type family with reflection
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace lux::rdesc
{
    /// The update-frequency domain. Determines whether a resource lives in a
    /// frozen slot or a free slot, and its binding-locality tier.
    enum class EBindFrequency : uint8_t
    {
        GLOBAL,      ///< Scene/frame-global (frozen: the canonical slot IS the final slot)
        BINDLESS,    ///< Bind-once unbounded array (frozen; SPIR-V patch remapping never touches it)
        FEATURE,     ///< Feature-private resource (free: allocated per graph, at compile time)
        PASS_LOCAL,  ///< Single-pass transient (free; the compute set0 convention belongs to this domain)
    };

    /// Lightweight stage-visibility bitmask (the contract only needs coarse
    /// granularity; reflection is the source of truth for exact stages).
    enum class EStageBits : uint8_t
    {
        VERTEX   = 1u << 0,
        FRAGMENT = 1u << 1,
        COMPUTE  = 1u << 2,
    };

    /// Descriptor-binding behavior flags — the part of the layout truth that
    /// SPIR-V reflection CANNOT recover (update semantics and array
    /// shrinkage). The contract declares these explicitly, and layout
    /// construction folds them into VkDescriptorSetLayoutBinding /
    /// VkDescriptorBindingFlags.
    enum class EBindingFlags : uint8_t
    {
        NONE               = 0,
        UPDATE_AFTER_BIND  = 1u << 0,
        PARTIALLY_BOUND    = 1u << 1,
        VARIABLE_COUNT     = 1u << 2,   ///< Trailing binding; actual count is derived from the device
    };
    [[nodiscard]] constexpr uint8_t operator|(EBindingFlags a, EBindingFlags b) noexcept
    {
        return static_cast<uint8_t>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
    }
    [[nodiscard]] constexpr uint8_t operator|(uint8_t a, EBindingFlags b) noexcept
    {
        return static_cast<uint8_t>(a | static_cast<uint8_t>(b));
    }
    [[nodiscard]] constexpr uint8_t operator|(EStageBits a, EStageBits b) noexcept
    {
        return static_cast<uint8_t>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
    }
    [[nodiscard]] constexpr uint8_t operator|(uint8_t a, EStageBits b) noexcept
    {
        return static_cast<uint8_t>(a | static_cast<uint8_t>(b));
    }

    /// One contract entry for a logical resource. `name` is the reconciliation
    /// key shared between the .lglsl declaration and the corresponding C++
    /// parameter-struct member; `count == 0` means the shader side is a
    /// runtime array (its upper bound is managed separately on the C++ side).
    struct LogicalResourceDesc
    {
        const char*     name;
        EBindFrequency  frequency;
        EDescriptorType type;
        uint32_t        count;             ///< 0 = runtime array
        uint32_t        canonical_set;
        uint32_t        canonical_binding;
        uint8_t         stages;            ///< EStageBits bitmask
        uint8_t         binding_flags{0};  ///< EBindingFlags bitmask (not recoverable from reflection; declared by the contract)

        /// Whether this resource lives in an ENGINE-SHARED descriptor set
        /// (the set instance is allocated by an engine-side resource object:
        /// SceneResources / InstanceResources / LightResources /
        /// MaterialResources / TextureResources / VertexPoolRegistry / …).
        ///
        /// true  → when building the pipeline layout, this set is taken
        ///         directly from the engine's shared layout table.
        ///         **This is mandatory**: reflection only sees the subset of
        ///         bindings this particular pipeline actually uses, but the
        ///         set that gets bound was allocated with the FULL shape —
        ///         using the subset layout would trigger
        ///         VUID-vkCmdBindDescriptorSets-00358 (binding-count
        ///         mismatch).
        /// false → a pipeline-private set (e.g. a transient descriptor set),
        ///         whose shape is exactly what reflection sees; DescriptorService
        ///         builds it from reflection plus the flags above.
        bool            engine_set{false};
    };

    // -------------------------------------------------------------------------
    //  v0 contract data — a mirror of the current layout.
    //
    //  Registration rule: the name must match the shader declaration exactly
    //  (it's the reconciliation key), so v0 only records frozen-domain entries
    //  whose names are already settled. FEATURE-domain entries are registered
    //  one at a time as each shader migrates into .lglsl (the table grows in
    //  lockstep with the shaders, so it always stays reconcilable).
    // -------------------------------------------------------------------------
    inline constexpr auto kLayoutContractV0 = std::to_array<LogicalResourceDesc>({
        // ── BINDLESS domain (currently: set2 texture table / set7 vertex
        //    pool; later merged into a single frozen set)
        //    Declared in: lighting_common.glsl / gbuffer_*.frag etc. (uTex),
        //    skybox_cubemap.frag (uCubeTex), vertex_pool.glsl (luxVertexPools).
        //    Stage is FRAGMENT only: no compute shader references uTex
        //    (lighting_common is only included by fragment-stage shaders),
        //    and the engine's shared Texture layout (EngineSetShapes) is also
        //    built as FRAGMENT-only — an earlier version of this entry
        //    declared FRAGMENT|COMPUTE, an over-broad declaration caught and
        //    fixed by the compile-time cross-check between the contract and
        //    the shape table (at the end of EngineSetShapes.hpp).
        { "uTex",           EBindFrequency::BINDLESS, EDescriptorType::COMBINED_IMAGE_SAMPLER,
          /*count*/ 0, /*set*/ 2, /*binding*/ 0, static_cast<uint8_t>(EStageBits::FRAGMENT),
          EBindingFlags::PARTIALLY_BOUND | EBindingFlags::UPDATE_AFTER_BIND,
          /*engine_set*/ true },
        { "uCubeTex",       EBindFrequency::BINDLESS, EDescriptorType::COMBINED_IMAGE_SAMPLER,
          /*count*/ 0, /*set*/ 2, /*binding*/ 1, static_cast<uint8_t>(EStageBits::FRAGMENT),
          EBindingFlags::PARTIALLY_BOUND | EBindingFlags::UPDATE_AFTER_BIND | EBindingFlags::VARIABLE_COUNT,
          /*engine_set*/ true },
        { "luxVertexPools", EBindFrequency::BINDLESS, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 0, /*set*/ 7, /*binding*/ 0,
          EStageBits::VERTEX | EStageBits::FRAGMENT | EStageBits::COMPUTE,
          EBindingFlags::PARTIALLY_BOUND | EBindingFlags::UPDATE_AFTER_BIND,
          /*engine_set*/ true },

        // ── GLOBAL domain (set0)
        { "uSceneGlobals",  EBindFrequency::GLOBAL,   EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 0, /*binding*/ 0,
          EStageBits::VERTEX | EStageBits::FRAGMENT | EStageBits::COMPUTE,
          static_cast<uint8_t>(EBindingFlags::UPDATE_AFTER_BIND),
          /*engine_set*/ true },
        { "uViews",         EBindFrequency::GLOBAL,   EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 0, /*binding*/ 1,
          EStageBits::VERTEX | EStageBits::FRAGMENT | EStageBits::COMPUTE,
          static_cast<uint8_t>(EBindingFlags::UPDATE_AFTER_BIND),
          /*engine_set*/ true },

        // ── FEATURE domain (registered one at a time as each shader migrates
        //    into .lglsl; canonical = its current slot)
        //    (R2-1:uHDRColor(tonemap 私有采样输入,set1 b0)已退出契约 ——
        //     engine_set=false 的管线私有条目只为喂发射器而存在;发射器现在对
        //     契约外资源按"set 1 + 文件内声明序"自动分配,给出相同位置,
        //     .spv 逐字节不变。同批退出:uGroup / uBlurSrc / uBlur / uMask。)
        //    lighting_common.lglslh: the four light SSBOs (currently set3
        //    b0-3; they'll move together later when the tiering switches
        //    over, with zero shader changes)
        { "uSpotLights",        EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 3, /*binding*/ 0, EStageBits::FRAGMENT | EStageBits::COMPUTE,
          static_cast<uint8_t>(EBindingFlags::UPDATE_AFTER_BIND),
          /*engine_set*/ true },
        //    shadow_pcf.glsl / shadow_evsm.glsl: the Light set's shadow
        //    section, b4-b10. Caught in one shot by the contract-completeness
        //    reconciliation test (the [contract] case in spirv_patcher_test):
        //    all 6 shadow-sampling fragment variants (deferred_lighting /
        //    fr_pbr / fr_stylized, each x PCF/EVSM) declare these 7 entries —
        //    without registering them here, switching those pipelines over
        //    gets rejected by the partial-migration guard. These fields
        //    mirror EngineSetShapes' kLight b4-b10 exactly (checked by the
        //    compile-time cross-validation).
        //    WARNING: uShadowSlices is "one name, multiple homes": the
        //    MeshShadow family's compact caster set (declared explicitly by
        //    that feature) puts it at its OWN set's b0 — that alternate slot
        //    is registered in the reconciliation test's kContractExemptions,
        //    not in this contract.
        { "uShadowSlices",      EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 3, /*binding*/ 4, EStageBits::VERTEX | EStageBits::FRAGMENT,
          EBindingFlags::UPDATE_AFTER_BIND | EBindingFlags::PARTIALLY_BOUND,
          /*engine_set*/ true },
        { "uShadowAtlas",       EBindFrequency::FEATURE, EDescriptorType::COMBINED_IMAGE_SAMPLER,
          /*count*/ 1, /*set*/ 3, /*binding*/ 5, static_cast<uint8_t>(EStageBits::FRAGMENT),
          EBindingFlags::UPDATE_AFTER_BIND | EBindingFlags::PARTIALLY_BOUND,
          /*engine_set*/ true },
        { "uShadowConfig",      EBindFrequency::FEATURE, EDescriptorType::UNIFORM_BUFFER,
          /*count*/ 1, /*set*/ 3, /*binding*/ 6, static_cast<uint8_t>(EStageBits::FRAGMENT),
          EBindingFlags::UPDATE_AFTER_BIND | EBindingFlags::PARTIALLY_BOUND,
          /*engine_set*/ true },
        { "uSpotShadowMap",     EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 3, /*binding*/ 7, static_cast<uint8_t>(EStageBits::FRAGMENT),
          EBindingFlags::UPDATE_AFTER_BIND | EBindingFlags::PARTIALLY_BOUND,
          /*engine_set*/ true },
        { "uPointShadowMap",    EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 3, /*binding*/ 8, static_cast<uint8_t>(EStageBits::FRAGMENT),
          EBindingFlags::UPDATE_AFTER_BIND | EBindingFlags::PARTIALLY_BOUND,
          /*engine_set*/ true },
        { "uShadowAtlasEVSM",   EBindFrequency::FEATURE, EDescriptorType::COMBINED_IMAGE_SAMPLER,
          /*count*/ 1, /*set*/ 3, /*binding*/ 9, static_cast<uint8_t>(EStageBits::FRAGMENT),
          EBindingFlags::UPDATE_AFTER_BIND | EBindingFlags::PARTIALLY_BOUND,
          /*engine_set*/ true },
        { "uEvsmConfig",        EBindFrequency::FEATURE, EDescriptorType::UNIFORM_BUFFER,
          /*count*/ 1, /*set*/ 3, /*binding*/ 10, static_cast<uint8_t>(EStageBits::FRAGMENT),
          EBindingFlags::UPDATE_AFTER_BIND | EBindingFlags::PARTIALLY_BOUND,
          /*engine_set*/ true },
        //    shading_inputs.glsl:屏幕空间着色输入数组(b11,v0 只有 AO 一个
        //    槽)。与 b4-b10 的影子段不同,这条**没有 PARTIALLY_BOUND** ——
        //    LightResources 在 init 期就用 1×1 白纹理写满全部元素,消费者
        //    无条件采样(默认值 1.0 = 无遮蔽),提供者(如 SSAO)只是覆写。
        //    count 是 C++ 侧 EShadingInputSlot::COUNT 的镜像(形状表 B 用
        //    枚举,这里是字面量 —— 本表不依赖 render 头)。
        { "uShadingInputs",     EBindFrequency::FEATURE, EDescriptorType::COMBINED_IMAGE_SAMPLER,
          /*count*/ 1, /*set*/ 3, /*binding*/ 11, static_cast<uint8_t>(EStageBits::FRAGMENT),
          static_cast<uint8_t>(EBindingFlags::UPDATE_AFTER_BIND),
          /*engine_set*/ true },
        { "uDirectionalLights", EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 3, /*binding*/ 1, EStageBits::FRAGMENT | EStageBits::COMPUTE,
          static_cast<uint8_t>(EBindingFlags::UPDATE_AFTER_BIND),
          /*engine_set*/ true },
        { "uPointLights",       EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 3, /*binding*/ 2, EStageBits::FRAGMENT | EStageBits::COMPUTE,
          static_cast<uint8_t>(EBindingFlags::UPDATE_AFTER_BIND),
          /*engine_set*/ true },
        { "uAreaLights",        EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 3, /*binding*/ 3, EStageBits::FRAGMENT | EStageBits::COMPUTE,
          static_cast<uint8_t>(EBindingFlags::UPDATE_AFTER_BIND),
          /*engine_set*/ true },
        //    (原此处的 "uParticles"(set 5 binding 0)已删。粒子的 C++ 早在
        //     eb93bed 就删了,3 个着色器无人加载,现已移出构建 —— 声明它的
        //     particle.vert.lglsl 不复存在,而本表的条目必须对应一个真实的
        //     着色器资源。EngineSetShapes 的 engine_sets::kParticle 已同步清空,
        //     槽位号 5 保留。)
        //    canvas2d (image/pixel_field/tile).vert.lglsl: the GPU-driven
        //    instance streams for the three kinds, plus the shared-shape
        //    order stream (FEATURE slot is set1 for all of them — it's a
        //    per-pipeline slot, so entries from different pipeline families
        //    are allowed to share it)
        { "uImages",            EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 1, /*binding*/ 0, static_cast<uint8_t>(EStageBits::VERTEX),
          static_cast<uint8_t>(EBindingFlags::UPDATE_AFTER_BIND) },
        { "uFields",            EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 1, /*binding*/ 0, static_cast<uint8_t>(EStageBits::VERTEX),
          static_cast<uint8_t>(EBindingFlags::UPDATE_AFTER_BIND) },
        { "uTiles",             EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 1, /*binding*/ 0, static_cast<uint8_t>(EStageBits::VERTEX),
          static_cast<uint8_t>(EBindingFlags::UPDATE_AFTER_BIND) },
        { "uOrder",             EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 1, /*binding*/ 1, static_cast<uint8_t>(EStageBits::VERTEX),
          static_cast<uint8_t>(EBindingFlags::UPDATE_AFTER_BIND) },
        //    (R2-1:uGroup / uBlurSrc / uBlur / uMask(canvas2d 合成与 highlight
        //     模糊/合成的管线私有采样输入,set1 b0 / b0 / b0+b1)已随 uHDRColor
        //     退出契约 —— 自动分配按声明序给出与原钉位逐字节相同的结果。
        //     历史备注保留:uBlurSrc 原名 uTex,迁移期改名以避开 BINDLESS
        //     uTex[] 的对账键 —— 改名的理由今天依然成立:局部名与契约名同名时,
        //     发射器会按契约注入而不是按局部分配。)
        //    forward/gbuffer VS's three entries (shared by forward_mesh_vp /
        //    gbuffer_vp / highlight_mask): the two instance streams plus the
        //    GPU-culled visibility table. uVisibleInstances' canonical=5
        //    takes the forward family's convention; the shadow family's
        //    drifted set2 variant is left unmigrated for now and will be
        //    reconciled together when the tiering switches over later
        { "uTransforms",        EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 1, /*binding*/ 0, EStageBits::VERTEX | EStageBits::FRAGMENT,
          static_cast<uint8_t>(EBindingFlags::UPDATE_AFTER_BIND),
          /*engine_set*/ true },
        { "uProperties",        EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 1, /*binding*/ 1, EStageBits::VERTEX | EStageBits::FRAGMENT,
          static_cast<uint8_t>(EBindingFlags::UPDATE_AFTER_BIND),
          /*engine_set*/ true },
        { "uVisibleInstances",  EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 5, /*binding*/ 0, static_cast<uint8_t>(EStageBits::VERTEX) },
        //    Material-family SSBO (binding = technique id; forward and
        //    gbuffer fragment shaders share the same name/slot)
        { "uUnlit",             EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 4, /*binding*/ 0, static_cast<uint8_t>(EStageBits::FRAGMENT),
          static_cast<uint8_t>(EBindingFlags::UPDATE_AFTER_BIND),
          /*engine_set*/ true },
        { "uPbr",               EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 4, /*binding*/ 2, static_cast<uint8_t>(EStageBits::FRAGMENT),
          static_cast<uint8_t>(EBindingFlags::UPDATE_AFTER_BIND),
          /*engine_set*/ true },
        { "uStylized",          EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 4, /*binding*/ 3, static_cast<uint8_t>(EStageBits::FRAGMENT),
          static_cast<uint8_t>(EBindingFlags::UPDATE_AFTER_BIND),
          /*engine_set*/ true },
        //    The Graph family's (node-graph material) generic blob. Declared
        //    not in .lglsl but in engine/toolchain/shader/glsl's ShaderEmitter —
        //    material-graph shaders are generated at runtime, but this entry
        //    still needs to be registered: otherwise reflection routing won't
        //    recognize it as belonging to the engine's Material set and will
        //    build it a private layout instead, making the graph-material
        //    variant's pipeline layout incompatible with the built-in
        //    family's variant within the same pass (VUID-...-08600).
        { "uMats",              EBindFrequency::FEATURE, EDescriptorType::STORAGE_BUFFER,
          /*count*/ 1, /*set*/ 4, /*binding*/ 4, static_cast<uint8_t>(EStageBits::FRAGMENT),
          static_cast<uint8_t>(EBindingFlags::UPDATE_AFTER_BIND),
          /*engine_set*/ true },
    });

    [[nodiscard]] constexpr std::span<const LogicalResourceDesc> layoutContract() noexcept
    {
        return kLayoutContractV0;
    }

    /// Look up an entry by name (the reconciliation key); returns nullptr if
    /// unregistered. A linear scan — the contract has only a few dozen
    /// entries, and every caller (emitter / layout construction) runs at
    /// compile time or on a low-frequency path.
    [[nodiscard]] constexpr const LogicalResourceDesc* findLogicalResource(std::string_view name) noexcept
    {
        for (const auto& e : kLayoutContractV0)
            if (name == e.name)
                return &e;
        return nullptr;
    }

    /// 「不在契约里」的索引哨兵。
    inline constexpr std::uint32_t kInvalidLogicalResourceIndex =
        (std::numeric_limits<std::uint32_t>::max)();

    /// 表内下标。诊断与错误上报传这个索引而不是名字字符串:契约是引擎级常量,两端读
    /// 的是同一份表,所以名字在消费侧解析即可,不必跨线程/跨进程搬运字符串。
    [[nodiscard]] constexpr std::uint32_t logicalResourceIndex(std::string_view name) noexcept
    {
        for (std::size_t i = 0; i < kLayoutContractV0.size(); ++i)
            if (name == kLayoutContractV0[i].name)
                return static_cast<std::uint32_t>(i);
        return kInvalidLogicalResourceIndex;
    }

    /// logicalResourceIndex 的逆向解析;越界返回 nullptr。
    [[nodiscard]] constexpr const LogicalResourceDesc* logicalResourceAt(std::uint32_t index) noexcept
    {
        if (index >= kLayoutContractV0.size())
            return nullptr;
        return &kLayoutContractV0[index];
    }

} // namespace lux::rdesc
