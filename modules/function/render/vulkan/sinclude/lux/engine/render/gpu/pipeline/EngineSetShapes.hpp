#pragma once
// =============================================================================
//  EngineSetShapes.hpp — shape declarations for the engine's shared
//  descriptor sets.
// -----------------------------------------------------------------------------
//  Why this table is needed: the full shape of an engine-level set
//  (Scene/Instance/Texture/Light/Material/Particle/Compute/VertexPool) is an
//  engine-level fact — it cannot be decided by "which pipelines happen to be
//  in a given render graph." Reflection aggregation at graph-compile time
//  only ever sees the binding subset that graph actually uses, while the set
//  instance is always allocated with the full shape (binding the full set
//  with a subset layout triggers VUID-vkCmdBindDescriptorSets-00358). More
//  direct evidence: the Scene set's binding 0 (SceneGlobalGpuData) is
//  currently not declared by any shader, so it doesn't even have a "shader
//  name" contract key to hang off of — it can only be declared by the
//  engine itself.
//
//  This table replaces the hand-written per-set VkDescriptorSetLayoutBinding
//  arrays that used to live in GeneralDescriptorSetLayout::init(): the shape
//  moves from *code* to *data*, and init degenerates into one generic build
//  loop. This is a different table, with a different purpose, from
//  LayoutContract (logical resource -> flags/frequency domain/ownership,
//  oriented at shader reconciliation):
//    LayoutContract  — what a shader-declared resource is called and what it means;
//    EngineSetShapes — what an engine-shared set looks like.
//
//  Design: .internal/lux-engine-descriptor-layout-architecture.md §4.1/§4.2
// =============================================================================

#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/function/render/client/core/ShadingInputSlot.hpp> // kShadingInputSlotCount (Light b11)
#include <lux/engine/description/LayoutContract.hpp>                   // rdesc::EBindFrequency
#include <lux/engine/description/ShaderInfo.hpp>                       // rdesc::ShaderInfo
#include <lux/engine/render/gpu/pipeline/SpirvPatcher.hpp>             // SpirvRelocation

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::render
{
    /// Where a binding's descriptorCount comes from. Most bindings are a
    /// fixed 1; a bindless array's capacity is derived from device limits,
    /// which can't be filled in when building the static table, so it's
    /// tagged with an enum and resolved at build time instead.
    enum class EBindingCountSource : uint8_t
    {
        Fixed,                ///< Use the literal value of the count field.
        Bindless2DTextures,   ///< Device-derived: bindless 2D texture table capacity.
        BindlessCubeTextures, ///< Device-derived: bindless cube texture table capacity.
        VertexPoolSlots,      ///< kVertexPoolMaxCount.
        MaterialFamilies,     ///< kMaterialFamilyBindingCount (one binding per family).
    };

    struct EngineSetBindingShape
    {
        uint32_t binding{0};
        VkDescriptorType type{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        VkShaderStageFlags stages{0};
        VkDescriptorBindingFlags binding_flags{0};
        EBindingCountSource count_source{EBindingCountSource::Fixed};
        uint32_t count{1};
    };

    struct EngineSetShape
    {
        EDescriptorSetSlot slot{};
        std::span<const EngineSetBindingShape> bindings;
        /// Whether this set needs the UPDATE_AFTER_BIND pool flag (true if
        /// any binding carries UAB).
        bool update_after_bind{false};
        /// For sets like MaterialFamilies that "expand into N identically-
        /// shaped bindings by a constant": bindings holds only the template
        /// entry 0, expanded by count_source at build time.
        bool expand_by_count_source{false};
        /// This set's update-frequency domain.
        ///
        /// Why this is an engine-level fact rather than derived from the
        /// contract: the contract registers frequency per *logical
        /// resource*, but what's needed here is the frequency of the
        /// *whole set* — the two don't always line up: the Scene set's
        /// binding 0 isn't declared by any shader, so there's no contract
        /// entry to derive it from at all. And the ≤4-set goal is built
        /// precisely on "there are only 4 frequency domains" (sets in the
        /// same domain merge into one, so any pipeline binds at most 4) —
        /// so every set's domain must be a fixed, known fact.
        rdesc::EBindFrequency frequency{rdesc::EBindFrequency::FEATURE};
    };

    namespace engine_sets
    {
        constexpr VkShaderStageFlags kVFC =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        constexpr VkShaderStageFlags kVF = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        constexpr VkShaderStageFlags kFC = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        constexpr VkShaderStageFlags kF = VK_SHADER_STAGE_FRAGMENT_BIT;
        constexpr VkShaderStageFlags kC = VK_SHADER_STAGE_COMPUTE_BIT;
        constexpr VkShaderStageFlags kCV = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT;

        constexpr VkDescriptorBindingFlags kUAB = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        constexpr VkDescriptorBindingFlags kUAB_PB = kUAB | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        constexpr VkDescriptorBindingFlags kUAB_PB_VAR = kUAB_PB | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;

        // SET 0 — Scene: global SoA + view SoA (binding 0 isn't declared by
        // any shader — only the engine side writes it — so it can never
        // appear in LayoutContract).
        inline constexpr std::array kScene{
            EngineSetBindingShape{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kVFC, kUAB},
            EngineSetBindingShape{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kVFC, kUAB},
        };

        // SET 1 — Instance: transform SoA + property SoA.
        inline constexpr std::array kInstance{
            EngineSetBindingShape{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kVF, kUAB},
            EngineSetBindingShape{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kVF, kUAB},
        };

        // SET 2 — Texture: bindless 2D + cube (capacity is device-derived).
        inline constexpr std::array kTexture{
            EngineSetBindingShape{
                0,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                kF,
                kUAB_PB,
                EBindingCountSource::Bindless2DTextures},
            EngineSetBindingShape{
                1,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                kF,
                kUAB_PB_VAR,
                EBindingCountSource::BindlessCubeTextures},
        };

        // SET 3 — Light + shadow + shading inputs: b0-3 are lights; b4-10 are
        // shadows (optional, PARTIALLY_BOUND); b11 is the screen-space shading
        // input array (ALWAYS fully written — LightResources fills every
        // element with a 1×1 white default at init, so no PARTIALLY_BOUND).
        inline constexpr std::array kLight{
            EngineSetBindingShape{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFC, kUAB},
            EngineSetBindingShape{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFC, kUAB},
            EngineSetBindingShape{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFC, kUAB},
            EngineSetBindingShape{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFC, kUAB},
            EngineSetBindingShape{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kVF, kUAB_PB},
            EngineSetBindingShape{5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kF, kUAB_PB},
            EngineSetBindingShape{6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kF, kUAB_PB},
            EngineSetBindingShape{7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kF, kUAB_PB},
            EngineSetBindingShape{8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kF, kUAB_PB},
            EngineSetBindingShape{9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kF, kUAB_PB},
            EngineSetBindingShape{10, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kF, kUAB_PB},
            EngineSetBindingShape{
                11,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                kF,
                kUAB,
                EBindingCountSource::Fixed,
                kShadingInputSlotCount},
        };

        // SET 4 — Material: one identically-shaped SSBO per family (count =
        // kMaterialFamilyBindingCount).
        inline constexpr std::array kMaterial{
            EngineSetBindingShape{
                0,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                kF,
                kUAB,
                EBindingCountSource::MaterialFamilies},
        };

        // SET 5 — Particle:**空**。
        //
        // 粒子的 5 个 C++ 文件已在 eb93bed 删除,3 个着色器无人加载(既不在
        // embed_builtin_shaders,全仓也没有任何运行时引用),现已一并移出构建。
        // 这条 binding 描述的资源不存在了。
        //
        // 槽位号 5 **保留** —— kEngineSetShapes 按槽位号做数组下标,腾掉一个就是
        // 全体重排(会牵动 kLayoutContractV0 的字面量、vertex_pool.glsl 的宏、
        // 所有含 luxVertexPools 的 .spv 字节)。清空 binding 列表则不然:
        // engineSetDomainOffset() 是 constexpr 且 13 个消费点全部实时计算,
        // 空域的 occupancy 为 0,后面的域自动前移。
        inline constexpr std::array<EngineSetBindingShape, 0> kParticle{};

        // SET 6 — Compute cull: 1 UBO + 3 SSBOs.
        inline constexpr std::array kCompute{
            EngineSetBindingShape{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kC, 0},
            EngineSetBindingShape{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kC, 0},
            EngineSetBindingShape{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kC, 0},
            EngineSetBindingShape{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kC, 0},
        };

        // SET 7 — VertexPool: bindless vertex-source array.
        inline constexpr std::array kVertexPool{
            EngineSetBindingShape{
                0,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                kVFC,
                kUAB_PB,
                EBindingCountSource::VertexPoolSlots},
        };
    } // namespace engine_sets

    /// Shape table for all engine sets (index is the set number, aligned
    /// with EDescriptorSetSlot).
    ///
    /// The last column is the frequency domain, which decides which group a
    /// set merges into under ≤4-set layering:
    ///   GLOBAL     once per frame (scene/view)
    ///   BINDLESS   large tables that almost never change (textures, vertex pool)
    ///   FEATURE    varies with the feature combination (instance/light/material/particle/cull)
    ///   PASS_LOCAL per pass (no engine set lives here — it's the domain of private sets)
    inline constexpr std::array<EngineSetShape, kDescriptorSetCount> kEngineSetShapes{{
        {EDescriptorSetSlot::Scene, engine_sets::kScene, true, false, rdesc::EBindFrequency::GLOBAL},
        {EDescriptorSetSlot::Instance, engine_sets::kInstance, true, false, rdesc::EBindFrequency::FEATURE},
        {EDescriptorSetSlot::Texture, engine_sets::kTexture, true, false, rdesc::EBindFrequency::BINDLESS},
        {EDescriptorSetSlot::Light, engine_sets::kLight, true, false, rdesc::EBindFrequency::FEATURE},
        {EDescriptorSetSlot::Material, engine_sets::kMaterial, true, true, rdesc::EBindFrequency::FEATURE},
        {EDescriptorSetSlot::Particle, engine_sets::kParticle, false, false, rdesc::EBindFrequency::FEATURE},
        {EDescriptorSetSlot::Compute, engine_sets::kCompute, false, false, rdesc::EBindFrequency::FEATURE},
        // The vertex pool belongs to FEATURE, not BINDLESS — the domain is a
        // *grouping key* chosen by "lifetime + budget," not an assertion
        // about update frequency. Reason: BINDLESS's other member (the
        // texture table) is a single *global* instance, while the vertex
        // pool is *per scene* (owned by StandardMeshStackFeature; 2D/headless
        // scenes don't pay for it). Merging the two into one set would force
        // that set down to a per-scene lifetime, so every scene's descriptor
        // pool would have to hold thousands of bindless texture descriptors
        // times frames-in-flight — pure waste.
        // Once placed in FEATURE: BINDLESS is left with only the global
        // texture table (reusing the existing global set, zero additions),
        // and the vertex pool shares its domain and lifetime with the rest
        // of the per-scene resources. Pipelines still bind only 4 sets
        // (verified in practice).
        {EDescriptorSetSlot::VertexPool, engine_sets::kVertexPool, true, false, rdesc::EBindFrequency::FEATURE},
    }};

    // ── Position constants for domain merging ─────────────────────────────
    //
    //  Must be computed from the whole shape table, never from "which sets
    //  a given graph happens to use": the domain set is a single
    //  scene-level instance, but different graphs use different engine sets
    //  — if we computed offsets per graph, a graph without Light would place
    //  Material at offset 2 instead of 13, making the two graphs' domain
    //  layouts incompatible even though there is only one instance. So the
    //  offset, like the shape, is an engine-level fact.

    /// How many bindings an engine set occupies within the domain.
    /// Not `bindings.size()`: Material is a template entry that expands to
    /// N entries by family count.
    [[nodiscard]] constexpr uint32_t engineSetOccupancy(uint32_t canonical_set) noexcept
    {
        if (canonical_set >= kEngineSetShapes.size())
            return 0;
        const EngineSetShape& shape = kEngineSetShapes[canonical_set];
        if (!shape.expand_by_count_source)
            return static_cast<uint32_t>(shape.bindings.size());
        if (shape.bindings.empty())
            return 0;
        switch (shape.bindings[0].count_source)
        {
        case EBindingCountSource::MaterialFamilies:
            return kMaterialFamilyBindingCount;
        default:
            return static_cast<uint32_t>(shape.bindings.size());
        }
    }

    /// Whether this set has any VARIABLE_DESCRIPTOR_COUNT binding.
    [[nodiscard]] constexpr bool engineSetHasVariableCount(uint32_t canonical_set) noexcept
    {
        if (canonical_set >= kEngineSetShapes.size())
            return false;
        for (const auto& b : kEngineSetShapes[canonical_set].bindings)
            if (b.binding_flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT)
                return true;
        return false;
    }

    /// Which binding entry in the shape table a given canonical position
    /// maps to; returns nullptr if it doesn't exist.
    ///
    /// For template-style sets (Material's per-family expansion), only the
    /// template entry 0 is recorded: a binding number is considered to
    /// exist as long as it falls within the expanded range (< occupancy),
    /// and its shape comes from the template entry.
    ///
    /// Used for: (1) compile-time cross-validation between the contract and
    /// the shape table; (2) when routing claims an engine set, checking
    /// entry by entry that a reflected binding really lives in this set
    /// (number exists + type matches).
    [[nodiscard]] constexpr const EngineSetBindingShape*
    engineShapeBindingFor(uint32_t canonical_set, uint32_t binding) noexcept
    {
        if (canonical_set >= kEngineSetShapes.size())
            return nullptr;
        const EngineSetShape& shape = kEngineSetShapes[canonical_set];
        if (shape.expand_by_count_source)
            return (!shape.bindings.empty() && binding < engineSetOccupancy(canonical_set)) ? &shape.bindings[0]
                                                                                            : nullptr;
        for (const auto& b : shape.bindings)
            if (b.binding == binding)
                return &b;
        return nullptr;
    }

    /// Looks up an *engine-owned* resource in the contract by name;
    /// non-engine resources, unregistered names, or out-of-range entries
    /// all return nullptr.
    ///
    /// This is the single predicate shared by both the SPIR-V relocation
    /// and the metadata relocation — relocationsFor and
    /// relocateShaderInfoByName must give exactly the same answer to
    /// "which resources need to move," and writing the predicate in two
    /// places would eventually diverge, so it lives here instead.
    [[nodiscard]] constexpr const rdesc::LogicalResourceDesc* engineOwnedResource(std::string_view name) noexcept
    {
        const auto* e = rdesc::findLogicalResource(name);
        return (e && e->engine_set && e->canonical_set < kEngineSetShapes.size()) ? e : nullptr;
    }

    /// rdesc reflection type -> Vulkan descriptor type (constexpr, usable
    /// for compile-time validation).
    [[nodiscard]] constexpr VkDescriptorType toVkDescriptorType(rdesc::EDescriptorType t) noexcept
    {
        switch (t)
        {
        case rdesc::EDescriptorType::SAMPLER:
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        case rdesc::EDescriptorType::COMBINED_IMAGE_SAMPLER:
            return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case rdesc::EDescriptorType::SAMPLED_IMAGE:
            return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case rdesc::EDescriptorType::STORAGE_IMAGE:
            return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case rdesc::EDescriptorType::UNIFORM_TEXEL_BUFFER:
            return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        case rdesc::EDescriptorType::STORAGE_TEXEL_BUFFER:
            return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        case rdesc::EDescriptorType::UNIFORM_BUFFER:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case rdesc::EDescriptorType::STORAGE_BUFFER:
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case rdesc::EDescriptorType::INPUT_ATTACHMENT:
            return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        default:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        }
    }

    /// An engine set's starting binding offset within its domain set
    /// (computed from the whole table, independent of any particular
    /// graph).
    ///
    /// Ordering is not simply ascending canonical order: Vulkan requires
    /// VARIABLE_DESCRIPTOR_COUNT to land on the binding with the **highest
    /// binding number** in the whole set (VUID-...-03004). Sorting by
    /// ascending canonical order would push Texture's cube binding (which
    /// carries VAR) into the middle — the layout could never be created.
    /// So any set carrying VAR is always placed last within its domain.
    ///
    /// A domain can have at most one set carrying VAR, or there's no
    /// solution; currently only Texture carries VAR in kEngineSetShapes.
    [[nodiscard]] constexpr uint32_t engineSetDomainOffset(uint32_t canonical_set) noexcept
    {
        if (canonical_set >= kEngineSetShapes.size())
            return 0;
        const auto domain = kEngineSetShapes[canonical_set].frequency;
        const bool self_var = engineSetHasVariableCount(canonical_set);

        uint32_t offset = 0;
        for (uint32_t s = 0; s < kEngineSetShapes.size(); ++s)
        {
            if (s == canonical_set || kEngineSetShapes[s].frequency != domain)
                continue;
            const bool s_var = engineSetHasVariableCount(s);
            // Whether s sorts before canonical_set: non-VAR sets as a group
            // sort before VAR sets; within the same category, ascending
            // canonical order.
            const bool before = (self_var == s_var) ? (s < canonical_set) : !s_var;
            if (before)
                offset += engineSetOccupancy(s);
        }
        return offset;
    }

    /// How many descriptors of each type a domain set contains —
    /// descriptor pools are sized off this number, not padded by
    /// guesswork.
    ///
    /// Only covers domains allocated *per scene* (GLOBAL / FEATURE): their
    /// descriptorCount values are all compile-time constants (Material
    /// expands by family, the vertex pool uses kVertexPoolMaxCount). The
    /// BINDLESS domain's capacity is device-derived, but it only contains
    /// the global texture table and isn't allocated from a scene pool, so
    /// it isn't needed here.
    struct DomainDescriptorCounts
    {
        uint32_t storage_buffer{0};
        uint32_t combined_image_sampler{0};
        uint32_t uniform_buffer{0};
    };

    [[nodiscard]] constexpr DomainDescriptorCounts domainDescriptorCounts(rdesc::EBindFrequency domain) noexcept
    {
        DomainDescriptorCounts c{};
        for (uint32_t s = 0; s < kEngineSetShapes.size(); ++s)
        {
            const auto& shape = kEngineSetShapes[s];
            if (shape.frequency != domain)
                continue;
            for (const auto& b : shape.bindings)
            {
                uint32_t n = b.count;
                uint32_t repeat = 1;
                switch (b.count_source)
                {
                case EBindingCountSource::VertexPoolSlots:
                    n = kVertexPoolMaxCount;
                    break;
                case EBindingCountSource::MaterialFamilies:
                    n = 1;
                    repeat = kMaterialFamilyBindingCount;
                    break;
                // Bindless capacity is device-derived — see the comment
                // above; this case is never hit here.
                case EBindingCountSource::Bindless2DTextures:
                case EBindingCountSource::BindlessCubeTextures:
                    n = 0;
                    break;
                case EBindingCountSource::Fixed:
                default:
                    break;
                }
                const uint32_t total = n * repeat;
                switch (b.type)
                {
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                    c.storage_buffer += total;
                    break;
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    c.combined_image_sampler += total;
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                    c.uniform_buffer += total;
                    break;
                default:
                    break;
                }
            }
        }
        return c;
    }

    /// Which set slot a domain occupies in the merged pipeline layout.
    ///
    /// This is the final ≤4-set slot assignment: the four domains each get
    /// one slot, ordered by "changes least first" (Vulkan's recommendation
    /// is to put low-frequency sets first, so rebinding a high-frequency
    /// set doesn't disturb the ones before it). PASS_LOCAL comes last — it
    /// changes every pass.
    [[nodiscard]] constexpr uint32_t domainSetSlot(rdesc::EBindFrequency domain) noexcept
    {
        switch (domain)
        {
        case rdesc::EBindFrequency::GLOBAL:
            return 0; // once per frame
        case rdesc::EBindFrequency::BINDLESS:
            return 1; // almost never changes
        case rdesc::EBindFrequency::FEATURE:
            return 2; // varies with feature combination
        case rdesc::EBindFrequency::PASS_LOCAL:
            return 3; // every pass
        }
        return 3;
    }

    /// 这份反射里是否还有「合并位置与当前位置不同」的引擎资源。
    ///
    /// 用途只有一个:检查跨 stage 的合并状态是否一致。不能直接比较 merged 标志 ——
    /// 恒等搬运的补丁字节与源相同,去重会命中源槽位、标志因此没置上,但位置本来就
    /// 已经正确,那不是真冲突。
    ///
    /// (它曾经还有第二个用途:在切换入口返回原句柄时区分「恒等成功」与「失败回退」。
    ///  切换不再有静默回退之后,失败会直接报错,那个用途随之消失。)
    [[nodiscard]] inline bool hasNonIdentityRelocation(const rdesc::ShaderInfo& info)
    {
        for (const auto& s : info.sets)
            for (const auto& b : s.bindings)
                if (const auto* e = engineOwnedResource(b.name))
                {
                    const uint32_t to_set = domainSetSlot(kEngineSetShapes[e->canonical_set].frequency);
                    const uint32_t to_binding = engineSetDomainOffset(e->canonical_set) + e->canonical_binding;
                    if (to_set != s.set || to_binding != b.binding)
                        return true;
                }
        return false;
    }

    /// The relocation table for private/explicit sets (sets with no
    /// contract hit): {set number before patching -> after patching}.
    ///
    /// The real allocation strategy, deliberately kept minimal: only two
    /// kinds of sets get moved: (1) slot number > 3 (mesh's visible set5 —
    /// after merging, the pipeline layout would be pushed out to 6 slots by
    /// it); (2) sets that collide with a domain slot this shader uses
    /// (caster's visible set2 collides with FEATURE domain slot 2). The
    /// target slot is the first free one starting from 3 upward (not
    /// conflicting with a used domain slot, nor with any other
    /// reserved/already-assigned private set). All other non-engine sets
    /// stay put — lighting's gbuffer set1 borrows a free BINDLESS slot, and
    /// cluster's explicit set3 is already ≤4 and non-colliding; moving them
    /// would only break the explicitly-declared slot-number convention
    /// (explicit_set_layouts matches by literal slot number).
    ///
    /// Warning: if an explicit set later comes to collide with a domain
    /// slot, this function will move it while the feature's
    /// explicit_set_layouts still writes the old slot number — the
    /// domain-mode validation at registration time will error out
    /// immediately; the fix in that case is to change the feature's
    /// explicit slot number, not to special-case it here.
    [[nodiscard]] inline std::vector<std::pair<uint32_t, uint32_t>> privateSetRemapFor(const rdesc::ShaderInfo& info)
    {
        // Which domain slots this shader's engine resources will occupy.
        bool domain_used[4] = {false, false, false, false};
        for (const auto& s : info.sets)
            for (const auto& b : s.bindings)
                if (const auto* e = engineOwnedResource(b.name))
                    domain_used[domainSetSlot(kEngineSetShapes[e->canonical_set].frequency)] = true;

        // Collect non-engine sets (the whole set has no contract hit; mixed
        // sets are rejected by the half-migrated-set gate before reaching
        // here).
        std::vector<uint32_t> private_sets;
        for (const auto& s : info.sets)
        {
            bool any_engine = false;
            for (const auto& b : s.bindings)
                if (engineOwnedResource(b.name))
                {
                    any_engine = true;
                    break;
                }
            if (!any_engine)
                private_sets.push_back(s.set);
        }
        std::sort(private_sets.begin(), private_sets.end());

        // Slot occupancy table: domain slots + private sets kept in place.
        bool occupied[16] = {};
        for (int d = 0; d < 4; ++d)
            if (domain_used[d])
                occupied[d] = true;

        std::vector<uint32_t> to_move;
        for (uint32_t s : private_sets)
        {
            const bool clash = s < 4 && occupied[s];
            if (s <= 3 && !clash)
                occupied[s] = true; // keep in place
            else
                to_move.push_back(s); // slot number > 3 or collides with a domain slot -> move
        }

        std::vector<std::pair<uint32_t, uint32_t>> remap;
        for (uint32_t s : to_move)
        {
            uint32_t target = 3;
            while (target < 16 && occupied[target])
                ++target;
            if (target >= 16)
                break; // impossible: all 16 slots exhausted
            occupied[target] = true;
            remap.push_back({s, target});
        }
        return remap;
    }

    /// Computes the relocation table for a reflection, keyed by *resource
    /// name*.
    ///
    /// Why it can't be keyed by the shader's set number: a set number is
    /// the shader's *local* numbering, not the same as the canonical number
    /// — DeferredLighting's fragment shader declares Light at set2
    /// (canonical is 3), and cluster at set3. Relocating by set number
    /// would move Light's binding as if it were Texture, straight into the
    /// BINDLESS domain, colliding with the real Texture — and a collision
    /// like that produces **no Vulkan error at runtime whatsoever**; two
    /// resources simply end up reading the same descriptor. This was
    /// caught by spirv_patcher_test against a real shader, not derived on
    /// paper.
    ///
    /// So the key has to be the resource name: name -> contract -> canonical
    /// position -> final position within the domain. Names not present in
    /// the contract (single-pipeline private sets) are left untouched.
    [[nodiscard]] inline std::vector<SpirvRelocation> relocationsFor(const rdesc::ShaderInfo& info)
    {
        std::vector<SpirvRelocation> out;
        for (const auto& s : info.sets)
            for (const auto& b : s.bindings)
            {
                const auto* e = engineOwnedResource(b.name);
                if (!e)
                    continue; // private/unregistered -> not handled by engine relocation (see the private relocation
                              // below)
                out.push_back(SpirvRelocation{
                    s.set,
                    b.binding,
                    domainSetSlot(kEngineSetShapes[e->canonical_set].frequency),
                    engineSetDomainOffset(e->canonical_set) + e->canonical_binding}
                );
            }

        // Relocation of private sets (the real allocation strategy): the
        // whole set shifts, binding numbers stay the same. Shares
        // privateSetRemapFor with relocateShaderInfoByName, so the two can
        // never diverge.
        for (const auto& [from, to] : privateSetRemapFor(info))
            for (const auto& s : info.sets)
                if (s.set == from)
                    for (const auto& b : s.bindings)
                        out.push_back(SpirvRelocation{from, b.binding, to, b.binding});
        return out;
    }

    /// Relocates a piece of reflection metadata to its merged position,
    /// using the same name-driven rules. Must be kept exactly consistent
    /// with relocationsFor — any divergence between the two means the
    /// SPIR-V and the metadata disagree.
    [[nodiscard]] inline rdesc::ShaderInfo relocateShaderInfoByName(const rdesc::ShaderInfo& src)
    {
        rdesc::ShaderInfo out = src;
        out.sets.clear();

        const auto emit = [&out](uint32_t set, rdesc::EDescriptorBindingInfo b) {
            b.set = set;
            for (auto& s : out.sets)
                if (s.set == set)
                {
                    s.bindings.push_back(std::move(b));
                    return;
                }
            rdesc::DescriptorSetLayoutInfo ns{};
            ns.set = set;
            ns.bindings.push_back(std::move(b));
            out.sets.push_back(std::move(ns));
        };

        const auto remap = privateSetRemapFor(src);
        const auto remapPrivate = [&remap](uint32_t set) {
            for (const auto& [from, to] : remap)
                if (from == set)
                    return to;
            return set;
        };

        for (const auto& s : src.sets)
            for (const auto& b : s.bindings)
            {
                const auto* e = engineOwnedResource(b.name);
                rdesc::EDescriptorBindingInfo nb = b;
                if (e)
                {
                    nb.binding = engineSetDomainOffset(e->canonical_set) + e->canonical_binding;
                    emit(domainSetSlot(kEngineSetShapes[e->canonical_set].frequency), std::move(nb));
                }
                else
                {
                    // Private/unregistered: shifted per the private
                    // relocation table (usually stays in place).
                    emit(remapPrivate(s.set), std::move(nb));
                }
            }

        // Explicit flag consumed by pipeline-layout routing dispatch (see
        // the comment on this field in ShaderInfo: Instance's domain offset
        // happens to be 0, so merged-or-not is structurally
        // indistinguishable and can't be guessed).
        out.merged_domain_layout = true;
        // Translation table for bare-slot bindings (literal slot numbers
        // like bindTransientDS(5, ...) get translated to the new slot
        // through this).
        out.private_set_remap.assign(remap.begin(), remap.end());
        return out;
    }

    // Note: there used to be a relocateShaderInfo that relocated by the
    // shader's local set number; it has been removed — a set number is the
    // shader's local numbering, and relocating by it is exactly the
    // collision bug pattern caught by spirv_patcher_test (see the file-top
    // comment and relocationsFor's comment above). Relocation is now always
    // name-driven.

    /// How many bindings a domain has in total after merging (computed from
    /// the whole table).
    [[nodiscard]] constexpr uint32_t domainBindingCount(rdesc::EBindFrequency domain) noexcept
    {
        uint32_t total = 0;
        for (uint32_t s = 0; s < kEngineSetShapes.size(); ++s)
            if (kEngineSetShapes[s].frequency == domain)
                total += engineSetOccupancy(s);
        return total;
    }

    // ── Compile-time cross-validation between the contract and the shape table ──
    //
    //  Both tables share the same underlying facts (canonical position,
    //  type, stage, binding flags), yet each has its own purpose and its
    //  own place to maintain it — drift between them can only be caught by
    //  a human paying attention. And the consequences of drift are all
    //  silent: a wrong binding number in the contract lands, via
    //  engineSetDomainOffset + canonical_binding, inside a neighboring
    //  set's domain range (a variant of the "overlapping domain binding
    //  ranges" problem); a stage/flags mismatch makes the Plan's record
    //  diverge from the real layout. Both tables are constexpr, so every
    //  engine_set contract entry is checked field-by-field against the
    //  shape table right here — drift becomes a compile error.
    //
    //  Each validation function returns the index, within
    //  kLayoutContractV0, of the first entry that fails (-1 = everything
    //  passed) — when a static_assert fails, evaluating the function name
    //  in any constexpr context pinpoints which entry.
    namespace contract_shape_check
    {
        [[nodiscard]] constexpr VkShaderStageFlags stagesToVk(uint8_t s) noexcept
        {
            VkShaderStageFlags f = 0;
            if (s & static_cast<uint8_t>(rdesc::EStageBits::VERTEX))
                f |= VK_SHADER_STAGE_VERTEX_BIT;
            if (s & static_cast<uint8_t>(rdesc::EStageBits::FRAGMENT))
                f |= VK_SHADER_STAGE_FRAGMENT_BIT;
            if (s & static_cast<uint8_t>(rdesc::EStageBits::COMPUTE))
                f |= VK_SHADER_STAGE_COMPUTE_BIT;
            return f;
        }

        [[nodiscard]] constexpr VkDescriptorBindingFlags bindingFlagsToVk(uint8_t f) noexcept
        {
            VkDescriptorBindingFlags out = 0;
            if (f & static_cast<uint8_t>(rdesc::EBindingFlags::UPDATE_AFTER_BIND))
                out |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
            if (f & static_cast<uint8_t>(rdesc::EBindingFlags::PARTIALLY_BOUND))
                out |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
            if (f & static_cast<uint8_t>(rdesc::EBindingFlags::VARIABLE_COUNT))
                out |= VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
            return out;
        }

        /// A canonical position must really exist in the shape table (the
        /// set is in range and the binding has an entry).
        [[nodiscard]] constexpr int firstPositionNotInShape() noexcept
        {
            for (std::size_t i = 0; i < rdesc::kLayoutContractV0.size(); ++i)
            {
                const auto& e = rdesc::kLayoutContractV0[i];
                if (!e.engine_set)
                    continue;
                if (!engineShapeBindingFor(e.canonical_set, e.canonical_binding))
                    return static_cast<int>(i);
            }
            return -1;
        }

        /// The contract's type must match the Vulkan type at that position
        /// in the shape table.
        [[nodiscard]] constexpr int firstTypeMismatch() noexcept
        {
            for (std::size_t i = 0; i < rdesc::kLayoutContractV0.size(); ++i)
            {
                const auto& e = rdesc::kLayoutContractV0[i];
                if (!e.engine_set)
                    continue;
                const auto* b = engineShapeBindingFor(e.canonical_set, e.canonical_binding);
                if (b && b->type != toVkDescriptorType(e.type))
                    return static_cast<int>(i);
            }
            return -1;
        }

        /// Stages must match bit-for-bit: the shape table is the ground
        /// truth for the layout; if the contract is narrower it distorts
        /// what the Plan records (applyContract overwrites using the
        /// contract), and if it's wider the contract is lying. Equality is
        /// required — wanting them to differ means coming here to change
        /// this assertion first, which makes it a deliberate decision.
        [[nodiscard]] constexpr int firstStageMismatch() noexcept
        {
            for (std::size_t i = 0; i < rdesc::kLayoutContractV0.size(); ++i)
            {
                const auto& e = rdesc::kLayoutContractV0[i];
                if (!e.engine_set)
                    continue;
                const auto* b = engineShapeBindingFor(e.canonical_set, e.canonical_binding);
                if (b && b->stages != stagesToVk(e.stages))
                    return static_cast<int>(i);
            }
            return -1;
        }

        /// Same for binding flags (UAB / PARTIALLY_BOUND / VARIABLE_COUNT):
        /// must match bit-for-bit.
        [[nodiscard]] constexpr int firstBindingFlagsMismatch() noexcept
        {
            for (std::size_t i = 0; i < rdesc::kLayoutContractV0.size(); ++i)
            {
                const auto& e = rdesc::kLayoutContractV0[i];
                if (!e.engine_set)
                    continue;
                const auto* b = engineShapeBindingFor(e.canonical_set, e.canonical_binding);
                if (b && b->binding_flags != bindingFlagsToVk(e.binding_flags))
                    return static_cast<int>(i);
            }
            return -1;
        }
    } // namespace contract_shape_check

    static_assert(
        contract_shape_check::firstPositionNotInShape() == -1,
        "LayoutContract 与 EngineSetShapes 漂移:某条 engine_set 契约条目的 canonical 位置"
        "在形状表里不存在(会静默落进邻居 set 的域内区间)。用 firstPositionNotInShape() 定位。");
    static_assert(
        contract_shape_check::firstTypeMismatch() == -1,
        "LayoutContract 与 EngineSetShapes 漂移:描述符类型不一致。用 firstTypeMismatch() 定位。");
    static_assert(
        contract_shape_check::firstStageMismatch() == -1,
        "LayoutContract 与 EngineSetShapes 漂移:stage 可见性不一致(Plan 记录会失真)。"
        "用 firstStageMismatch() 定位。");
    static_assert(
        contract_shape_check::firstBindingFlagsMismatch() == -1,
        "LayoutContract 与 EngineSetShapes 漂移:binding flags 不一致。"
        "用 firstBindingFlagsMismatch() 定位。");

    // ── binding 枚举表(A)与形状表(B)的编译期对账 ────────────────────────
    //
    //  上面那组守的是 **契约(C) ↔ 形状(B)**。但描述符布局其实有**三张**表:
    //
    //    A  DescriptorSetLayoutContract.hpp 的 per-set binding 枚举
    //       —— C++ 侧写描述符时念的名字(ELightSetBindings::SHADOW_ATLAS 等)
    //    B  本文件的 engine_sets  —— 建 layout 用的完整形状
    //    C  rdesc::kLayoutContractV0 —— 着色器对账用的逻辑资源表
    //
    //  B↔C 有上面 4 条。**A↔B 此前一条都没有,而且已经漂了**:
    //  EGeneralSetBindings 声称 set 1 只有 1 条 binding,实际 kInstance 有 2 条
    //  (b0 transform SoA、b1 property SoA)。它零引用,所以漂了一年也没人知道 ——
    //  已在 B1 删除。**没有校验的表,不会因为没人用就停止骗人。**
    //
    //  漂移的后果是静默的:A 的名字被用来写描述符(vkUpdateDescriptorSets 的
    //  dstBinding),B 的形状被用来建 layout。两者对不上时,写进去的描述符落在
    //  一个 layout 里不存在或类型不符的位置上 —— PARTIALLY_BOUND 让空 binding
    //  完全合法,于是着色器读到全零,零 validation 错误。
    //
    //  (kMaterialFamilyBindingCount 不在此列:engineSetOccupancy 直接读它,
    //   Material set 的 A 与 B 本来就是同一个数,没有第二份可漂。)
    namespace binding_enum_check
    {
        struct NamedBindingEnum
        {
            uint32_t slot;
            uint32_t count; ///< 枚举的 COUNT 哨兵
            const char* name;
        };

        inline constexpr std::array kNamedBindingEnums{
            NamedBindingEnum{
                static_cast<uint32_t>(EDescriptorSetSlot::Scene),
                static_cast<uint32_t>(ESceneSetBindings::COUNT),
                "ESceneSetBindings"},
            NamedBindingEnum{
                static_cast<uint32_t>(EDescriptorSetSlot::Texture),
                static_cast<uint32_t>(ETextureSetBindings::COUNT),
                "ETextureSetBindings"},
            NamedBindingEnum{
                static_cast<uint32_t>(EDescriptorSetSlot::Light),
                static_cast<uint32_t>(ELightSetBindings::COUNT),
                "ELightSetBindings"},
            NamedBindingEnum{
                static_cast<uint32_t>(EDescriptorSetSlot::VertexPool),
                static_cast<uint32_t>(EVertexPoolSetBindings::COUNT),
                "EVertexPoolSetBindings"},
        };

        /// 枚举的 COUNT 必须等于形状表里该 set 的实际 binding 数。
        /// 这条直接对准已经发生过的那次漂移。
        [[nodiscard]] constexpr int firstEnumCountMismatch() noexcept
        {
            for (std::size_t i = 0; i < kNamedBindingEnums.size(); ++i)
                if (kNamedBindingEnums[i].count != engineSetOccupancy(kNamedBindingEnums[i].slot))
                    return static_cast<int>(i);
            return -1;
        }

        /// 0..COUNT-1 每个编号都必须在形状表里真实存在。
        /// 比数量相等更强:形状表的 binding 编号不保证从 0 起连续(将来某个 set
        /// 若挖了洞,只比数量就查不出来)。
        [[nodiscard]] constexpr int firstEnumBindingNotInShape() noexcept
        {
            for (std::size_t i = 0; i < kNamedBindingEnums.size(); ++i)
                for (uint32_t b = 0; b < kNamedBindingEnums[i].count; ++b)
                    if (!engineShapeBindingFor(kNamedBindingEnums[i].slot, b))
                        return static_cast<int>(i);
            return -1;
        }
    } // namespace binding_enum_check

    static_assert(
        binding_enum_check::firstEnumCountMismatch() == -1,
        "DescriptorSetLayoutContract 的 binding 枚举与 EngineSetShapes 漂移:COUNT 与形状表的"
        "binding 数不一致。用 firstEnumCountMismatch() 取下标,再查 kNamedBindingEnums[i].name。");
    static_assert(
        binding_enum_check::firstEnumBindingNotInShape() == -1,
        "DescriptorSetLayoutContract 的 binding 枚举与 EngineSetShapes 漂移:某个枚举值在形状表里"
        "没有对应 binding。用 firstEnumBindingNotInShape() 定位。");

} // namespace lux::render
