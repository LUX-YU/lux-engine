#include <lux/engine/render/resources/lighting/LightResources.hpp>
#include <lux/engine/render/resources/descriptor/SceneDescriptorArena.hpp>

namespace lux::render
{
    LightHandle LightResources::allocateGlobalHandle()
    {
        if (!free_handle_indices_.empty())
        {
            const uint32_t index = free_handle_indices_.back();
            free_handle_indices_.pop_back();
            handle_alive_[index] = 1u;
            return LightHandle{index, handle_generations_[index]};
        }

        const uint32_t index = static_cast<uint32_t>(handle_generations_.size());
        handle_generations_.push_back(1u);
        handle_alive_.push_back(1u);
        return LightHandle{index, 1u};
    }

    void LightResources::releaseGlobalHandle(LightHandle h) noexcept
    {
        if (!h.valid() || h.index >= handle_generations_.size())
            return;
        if (!handle_alive_[h.index] || handle_generations_[h.index] != h.gen)
            return;

        handle_alive_[h.index] = 0u;
        ++handle_generations_[h.index];
        free_handle_indices_.push_back(h.index);
    }

    // ===== Alignment helpers (moved from header — no longer templated on ECS types) =====
    static aligned16vec3 to_aligned3(const Eigen::Vector3f& v) {
        aligned16vec3 o; o.x = v.x(); o.y = v.y(); o.z = v.z(); return o;
    }
    static aligned16vec2 to_aligned2(const Eigen::Vector2f& v) {
        aligned16vec2 o; o.x = v.x(); o.y = v.y(); return o;
    }

    // ===== Common GPU field fill from descriptor base fields =====
    template<typename TLightGPU>
    static void fillCommonFromDesc(TLightGPU& gpu, const Eigen::Vector3f& color,
                                    float intensity, uint32_t flags,
                                    uint32_t shadow_map_size, float shadow_bias,
                                    float shadow_normal_bias)
    {
        gpu.color              = to_aligned3(color);
        gpu.intensity          = intensity;
        gpu.flags              = flags;
        gpu.shadow_map_size    = shadow_map_size;
        gpu.shadow_bias        = shadow_bias;
        gpu.shadow_normal_bias = shadow_normal_bias;
    }
    bool LightResources::init(const InitInfo& info)
    {
        frames_in_flight_ = info.ssbo_config.slices;

        // Initialize SlicedSSBO for each light type
        std::get<SlicedSSBO<DirectionalLightGPU>>(ssbos_).init(info.ssbo_config);
        std::get<SlicedSSBO<PointLightGPU>>(ssbos_).init(info.ssbo_config);
        std::get<SlicedSSBO<SpotLightGPU>>(ssbos_).init(info.ssbo_config);
        std::get<SlicedSSBO<AreaLightGPU>>(ssbos_).init(info.ssbo_config);

        // Create per-frame descriptor sets from the scene's growable descriptor
        // arena. One set at a time: a batched vkAllocateDescriptorSets cannot
        // straddle a pool boundary if the arena grows mid-batch. allocate()
        // throws only if a fresh pool can't hold a single set — preserve the
        // historical "return false → scene renders unlit" contract.
        descriptor_sets_.resize(frames_in_flight_, VK_NULL_HANDLE);
        if (!info.arena)
            return false;
        try {
            for (uint32_t i = 0; i < frames_in_flight_; ++i)
                descriptor_sets_[i] = info.arena->allocate(info.set_layout);
        } catch (...) {
            return false;
        }

        // Write initial tight descriptors for every per-frame set.
        // Each set i must point at slice i so GLSL lights.length() stays bounded.
        for (uint32_t i = 0; i < frames_in_flight_; ++i)
        {
            refreshDescriptorOnSet<ELightSetBindings::LIGHT_DIRECTIONAL>(i, i);
            refreshDescriptorOnSet<ELightSetBindings::LIGHT_POINT>(i, i);
            refreshDescriptorOnSet<ELightSetBindings::LIGHT_SPOT>(i, i);
            refreshDescriptorOnSet<ELightSetBindings::LIGHT_AREA>(i, i);
        }

        initialized_ = true;
        return true;
    }

    Expected<LightHandle>
    LightResources::submit(const LightDescriptor& desc)
    {
        return submitDescriptor(desc);
    }

    Expected<LightHandle>
    LightResources::submitDescriptor(const LightDescriptor& desc)
    {
        ds_revision_.bump();

        return std::visit([this](const auto& d) -> Expected<LightHandle> {
            using T = std::decay_t<decltype(d)>;

            if constexpr (std::is_same_v<T, DirectionalLightDesc>) {
                DirectionalLightGPU gpu{};
                fillCommonFromDesc(gpu, d.color, d.intensity, d.flags,
                                    d.shadow_map_size, d.shadow_bias, d.shadow_normal_bias);
                gpu.direction     = to_aligned3(d.direction.normalized());
                gpu.cascade_count = d.cascade_count;
                for (uint32_t i = 0; i < 8; ++i) gpu.cascade_splits[i] = d.cascade_splits[i];

                auto local_slot = std::get<SlicedSSBO<DirectionalLightGPU>>(ssbos_).add(gpu);
                LightHandle h = allocateGlobalHandle();
                binding_map_.insert(h, SlotRecord{
                    .binding = ELightSetBindings::LIGHT_DIRECTIONAL,
                    .local_slot = local_slot,
                });
                return h;
            }
            else if constexpr (std::is_same_v<T, PointLightDesc>) {
                PointLightGPU gpu{};
                fillCommonFromDesc(gpu, d.color, d.intensity, d.flags,
                                    d.shadow_map_size, d.shadow_bias, d.shadow_normal_bias);
                gpu.position              = to_aligned3(d.position);
                gpu.range                 = d.range;
                gpu.attenuation_constant  = d.attenuation_constant;
                gpu.attenuation_linear    = d.attenuation_linear;
                gpu.attenuation_quadratic = d.attenuation_quadratic;

                auto local_slot = std::get<SlicedSSBO<PointLightGPU>>(ssbos_).add(gpu);
                LightHandle h = allocateGlobalHandle();
                binding_map_.insert(h, SlotRecord{
                    .binding = ELightSetBindings::LIGHT_POINT,
                    .local_slot = local_slot,
                });
                return h;
            }
            else if constexpr (std::is_same_v<T, SpotLightDesc>) {
                SpotLightGPU gpu{};
                fillCommonFromDesc(gpu, d.color, d.intensity, d.flags,
                                    d.shadow_map_size, d.shadow_bias, d.shadow_normal_bias);
                gpu.position              = to_aligned3(d.position);
                gpu.direction             = to_aligned3(d.direction.normalized());
                gpu.range                 = d.range;
                gpu.attenuation_constant  = d.attenuation_constant;
                gpu.attenuation_linear    = d.attenuation_linear;
                gpu.attenuation_quadratic = d.attenuation_quadratic;
                gpu.inner_cone_angle      = d.inner_cone_angle;
                gpu.outer_cone_angle      = d.outer_cone_angle;

                auto local_slot = std::get<SlicedSSBO<SpotLightGPU>>(ssbos_).add(gpu);
                LightHandle h = allocateGlobalHandle();
                binding_map_.insert(h, SlotRecord{
                    .binding = ELightSetBindings::LIGHT_SPOT,
                    .local_slot = local_slot,
                });
                return h;
            }
            else if constexpr (std::is_same_v<T, AreaLightDesc>) {
                AreaLightGPU gpu{};
                fillCommonFromDesc(gpu, d.color, d.intensity, d.flags,
                                    d.shadow_map_size, d.shadow_bias, d.shadow_normal_bias);
                gpu.size = to_aligned2(d.size);

                auto local_slot = std::get<SlicedSSBO<AreaLightGPU>>(ssbos_).add(gpu);
                LightHandle h = allocateGlobalHandle();
                binding_map_.insert(h, SlotRecord{
                    .binding = ELightSetBindings::LIGHT_AREA,
                    .local_slot = local_slot,
                });
                return h;
            }
        }, desc);
    }

    void LightResources::remove(LightHandle handle)
    {
        auto* rec = binding_map_.find(handle);
        if (!rec) return;

        switch (rec->binding) {
        case ELightSetBindings::LIGHT_DIRECTIONAL:
            std::get<SlicedSSBO<DirectionalLightGPU>>(ssbos_).remove(rec->local_slot); break;
        case ELightSetBindings::LIGHT_POINT:
            std::get<SlicedSSBO<PointLightGPU>>(ssbos_).remove(rec->local_slot); break;
        case ELightSetBindings::LIGHT_SPOT:
            std::get<SlicedSSBO<SpotLightGPU>>(ssbos_).remove(rec->local_slot); break;
        case ELightSetBindings::LIGHT_AREA:
            std::get<SlicedSSBO<AreaLightGPU>>(ssbos_).remove(rec->local_slot); break;
        default:
            break;
        }

        binding_map_.erase(handle);
        releaseGlobalHandle(handle);
    }

    std::error_code LightResources::modify(LightHandle handle, const LightDescriptor& desc)
    {
        auto* rec = binding_map_.find(handle);
        if (!rec)
            return make_error_code(ERenderError::NotFound);

        const ELightSetBindings binding = rec->binding;
        const SlotHandle local_slot = rec->local_slot;

        return std::visit([&](const auto& d) -> std::error_code {
            using T = std::decay_t<decltype(d)>;

            if constexpr (std::is_same_v<T, DirectionalLightDesc>) {
                if (binding != ELightSetBindings::LIGHT_DIRECTIONAL)
                    return make_error_code(ERenderError::UnsupportedType);
                DirectionalLightGPU gpu{};
                fillCommonFromDesc(gpu, d.color, d.intensity, d.flags,
                                    d.shadow_map_size, d.shadow_bias, d.shadow_normal_bias);
                gpu.direction     = to_aligned3(d.direction.normalized());
                gpu.cascade_count = d.cascade_count;
                for (uint32_t i = 0; i < 8; ++i) gpu.cascade_splits[i] = d.cascade_splits[i];
                auto& ssbo = std::get<SlicedSSBO<DirectionalLightGPU>>(ssbos_);
                return ssbo.modify(local_slot, gpu) ? std::error_code{} : make_error_code(ERenderError::ModifyFailed);
            }
            else if constexpr (std::is_same_v<T, PointLightDesc>) {
                if (binding != ELightSetBindings::LIGHT_POINT)
                    return make_error_code(ERenderError::UnsupportedType);
                PointLightGPU gpu{};
                fillCommonFromDesc(gpu, d.color, d.intensity, d.flags,
                                    d.shadow_map_size, d.shadow_bias, d.shadow_normal_bias);
                gpu.position              = to_aligned3(d.position);
                gpu.range                 = d.range;
                gpu.attenuation_constant  = d.attenuation_constant;
                gpu.attenuation_linear    = d.attenuation_linear;
                gpu.attenuation_quadratic = d.attenuation_quadratic;
                auto& ssbo = std::get<SlicedSSBO<PointLightGPU>>(ssbos_);
                return ssbo.modify(local_slot, gpu) ? std::error_code{} : make_error_code(ERenderError::ModifyFailed);
            }
            else if constexpr (std::is_same_v<T, SpotLightDesc>) {
                if (binding != ELightSetBindings::LIGHT_SPOT)
                    return make_error_code(ERenderError::UnsupportedType);
                SpotLightGPU gpu{};
                fillCommonFromDesc(gpu, d.color, d.intensity, d.flags,
                                    d.shadow_map_size, d.shadow_bias, d.shadow_normal_bias);
                gpu.position              = to_aligned3(d.position);
                gpu.direction             = to_aligned3(d.direction.normalized());
                gpu.range                 = d.range;
                gpu.attenuation_constant  = d.attenuation_constant;
                gpu.attenuation_linear    = d.attenuation_linear;
                gpu.attenuation_quadratic = d.attenuation_quadratic;
                gpu.inner_cone_angle      = d.inner_cone_angle;
                gpu.outer_cone_angle      = d.outer_cone_angle;
                auto& ssbo = std::get<SlicedSSBO<SpotLightGPU>>(ssbos_);
                return ssbo.modify(local_slot, gpu) ? std::error_code{} : make_error_code(ERenderError::ModifyFailed);
            }
            else if constexpr (std::is_same_v<T, AreaLightDesc>) {
                if (binding != ELightSetBindings::LIGHT_AREA)
                    return make_error_code(ERenderError::UnsupportedType);
                AreaLightGPU gpu{};
                fillCommonFromDesc(gpu, d.color, d.intensity, d.flags,
                                    d.shadow_map_size, d.shadow_bias, d.shadow_normal_bias);
                gpu.size = to_aligned2(d.size);
                auto& ssbo = std::get<SlicedSSBO<AreaLightGPU>>(ssbos_);
                return ssbo.modify(local_slot, gpu) ? std::error_code{} : make_error_code(ERenderError::ModifyFailed);
            }
        }, desc);
    }

    std::error_code LightResources::updateLive(LightHandle handle,
                                               const LightDescriptor& desc)
    {
        return modify(handle, desc);
    }
}
