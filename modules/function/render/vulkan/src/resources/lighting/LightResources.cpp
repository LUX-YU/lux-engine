#include <lux/engine/render/resources/lighting/LightResources.hpp>
#include <vk_mem_alloc.h>
#include <lux/engine/render/gpu/descriptor/SceneDescriptorArena.hpp>
#include <lux/engine/render/gpu/descriptor/DomainWriteTarget.hpp>

#include <type_traits>
#include <algorithm>
#include <cmath>

namespace lux::render
{
    bool LightResources::canRebaseSceneOrigin(
        const std::int64_t origin_delta[3]) const noexcept
    {
        const auto can_rebase = [origin_delta](const auto& ssbo)
        {
            using Value = std::remove_cvref_t<decltype(ssbo.hostValue(0u))>;
            if constexpr (std::is_same_v<Value, PointLightGPU> ||
                          std::is_same_v<Value, SpotLightGPU>)
            {
                for (std::uint32_t index = 0u;
                     index < ssbo.count(); ++index)
                {
                    if (ssbo.isSlotAlive(index) &&
                        !canRebaseRenderPageDelta(
                            ssbo.hostValue(index).position_page,
                            origin_delta))
                    {
                        return false;
                    }
                }
            }
            return true;
        };
        return std::apply(
            [&](const auto&... ssbo)
            {
                return (can_rebase(ssbo) && ...);
            },
            ssbos_);
    }

    void LightResources::rebaseSceneOrigin(
        const std::int64_t origin_delta[3]) noexcept
    {
        const auto rebase = [origin_delta](auto& ssbo)
        {
            using Value = std::remove_cvref_t<decltype(ssbo.hostValue(0u))>;
            if constexpr (std::is_same_v<Value, PointLightGPU> ||
                          std::is_same_v<Value, SpotLightGPU>)
            {
                for (std::uint32_t index = 0u;
                     index < ssbo.count(); ++index)
                {
                    if (!ssbo.isSlotAlive(index))
                        continue;
                    auto value = ssbo.hostValue(index);
                    rebaseRenderPageDelta(
                        value.position_page, origin_delta);
                    (void)ssbo.modifyAt(index, value);
                }
            }
        };
        std::apply([&](auto&... ssbo) { (rebase(ssbo), ...); }, ssbos_);
    }

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
        if (!h.isValid() || h.index >= handle_generations_.size())
            return;
        if (!handle_alive_[h.index] || handle_generations_[h.index] != h.gen)
            return;

        handle_alive_[h.index] = 0u;
        ++handle_generations_[h.index];
        free_handle_indices_.push_back(h.index);
    }

    std::optional<float> LightResources::intensity(
        LightHandle handle) const noexcept
    {
        const auto* record = binding_map_.find(handle);
        if (!record)
            return std::nullopt;
        const auto index = record->local_slot.index;
        switch (record->binding)
        {
        case ELightSetBindings::LIGHT_DIRECTIONAL:
            return std::get<SlicedSSBO<DirectionalLightGPU>>(ssbos_)
                .hostValue(index).intensity;
        case ELightSetBindings::LIGHT_POINT:
            return std::get<SlicedSSBO<PointLightGPU>>(ssbos_)
                .hostValue(index).intensity;
        case ELightSetBindings::LIGHT_SPOT:
            return std::get<SlicedSSBO<SpotLightGPU>>(ssbos_)
                .hostValue(index).intensity;
        case ELightSetBindings::LIGHT_AREA:
            return std::get<SlicedSSBO<AreaLightGPU>>(ssbos_)
                .hostValue(index).intensity;
        default:
            return std::nullopt;
        }
    }

    bool LightResources::setIntensity(
        LightHandle handle,
        float value) noexcept
    {
        const auto* record = binding_map_.find(handle);
        if (!record)
            return false;
        const auto modify = [index = record->local_slot.index, value](auto& ssbo)
        {
            auto light = ssbo.hostValue(index);
            light.intensity = value;
            return ssbo.modifyAt(index, light);
        };
        switch (record->binding)
        {
        case ELightSetBindings::LIGHT_DIRECTIONAL:
            return modify(std::get<SlicedSSBO<DirectionalLightGPU>>(ssbos_));
        case ELightSetBindings::LIGHT_POINT:
            return modify(std::get<SlicedSSBO<PointLightGPU>>(ssbos_));
        case ELightSetBindings::LIGHT_SPOT:
            return modify(std::get<SlicedSSBO<SpotLightGPU>>(ssbos_));
        case ELightSetBindings::LIGHT_AREA:
            return modify(std::get<SlicedSSBO<AreaLightGPU>>(ssbos_));
        default:
            return false;
        }
    }

    void LightResources::cancelIntensityTransition(LightHandle handle) noexcept
    {
        std::erase_if(
            intensity_transitions_,
            [handle](const IntensityTransition& transition)
            {
                return transition.handle == handle;
            });
    }

    bool LightResources::beginFadeIn(
        LightHandle handle,
        float scene_time,
        float duration_seconds)
    {
        const auto* record = binding_map_.find(handle);
        if (!record ||
            (record->binding != ELightSetBindings::LIGHT_POINT &&
             record->binding != ELightSetBindings::LIGHT_SPOT) ||
            !std::isfinite(scene_time) ||
            !std::isfinite(duration_seconds) ||
            duration_seconds <= 0.0f)
        {
            return false;
        }
        const auto target = intensity(handle);
        if (!target)
            return false;
        cancelIntensityTransition(handle);
        if (!setIntensity(handle, 0.0f))
            return false;
        intensity_transitions_.push_back(IntensityTransition{
            handle,
            0.0f,
            *target,
            scene_time,
            duration_seconds,
            false});
        return true;
    }

    bool LightResources::beginFadeOut(
        LightHandle handle,
        float scene_time,
        float duration_seconds)
    {
        const auto* record = binding_map_.find(handle);
        if (!record ||
            (record->binding != ELightSetBindings::LIGHT_POINT &&
             record->binding != ELightSetBindings::LIGHT_SPOT) ||
            !std::isfinite(scene_time) ||
            !std::isfinite(duration_seconds) ||
            duration_seconds <= 0.0f)
        {
            return false;
        }
        const auto current = intensity(handle);
        if (!current)
            return false;
        cancelIntensityTransition(handle);
        intensity_transitions_.push_back(IntensityTransition{
            handle,
            *current,
            0.0f,
            scene_time,
            duration_seconds,
            true});
        return true;
    }

    void LightResources::advanceIntensityTransitions(float scene_time)
    {
        std::vector<LightHandle> completed_removals;
        std::size_t write = 0u;
        for (auto transition : intensity_transitions_)
        {
            if (!binding_map_.contains(transition.handle))
                continue;
            const float elapsed = std::max(0.0f, scene_time - transition.start_time);
            const float factor = std::clamp(
                elapsed / transition.duration,
                0.0f,
                1.0f);
            const float value = transition.start_intensity +
                (transition.target_intensity - transition.start_intensity) * factor;
            if (!setIntensity(transition.handle, value))
                continue;
            if (factor >= 1.0f)
            {
                if (transition.remove_on_completion)
                    completed_removals.push_back(transition.handle);
                continue;
            }
            intensity_transitions_[write++] = transition;
        }
        intensity_transitions_.resize(write);
        for (const auto handle : completed_removals)
            remove(handle);
    }

    // ===== Alignment helpers (moved from header — no longer templated on ECS types) =====
    static aligned16vec3 to_aligned3(const Eigen::Vector3f& v) {
        aligned16vec3 o; o.x = v.x(); o.y = v.y(); o.z = v.z(); return o;
    }
    static aligned8vec2 to_aligned2(const Eigen::Vector2f& v) {
        aligned8vec2 o; o.x = v.x(); o.y = v.y(); return o;
    }

    template <typename TLightGPU>
    static void fillSpatialPosition(
        TLightGPU& gpu,
        const RenderLargePosition3D& position) noexcept
    {
        for (std::size_t axis = 0; axis != 3u; ++axis)
        {
            gpu.position_page[axis] = position.page_delta[axis];
        }
        gpu.position_page[3] = 0;
        gpu.position_local.x = position.local[0];
        gpu.position_local.y = position.local[1];
        gpu.position_local.z = position.local[2];
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

        // 不再分配 per-set 实例 —— 描述符只写场景域集,绑定也从域集取
        //(useEngineSet)。写目标即下面这组域集句柄。
        if (!domain_.set(info.domain_sets, info.domain_binding_offset))
            return false;

        // Write initial tight descriptors for every per-frame slice.
        // Slice i must point at its own data so GLSL lights.length() stays bounded.
        for (uint32_t i = 0; i < frames_in_flight_; ++i)
        {
            refreshDescriptorOnSet<ELightSetBindings::LIGHT_DIRECTIONAL>(i, i);
            refreshDescriptorOnSet<ELightSetBindings::LIGHT_POINT>(i, i);
            refreshDescriptorOnSet<ELightSetBindings::LIGHT_SPOT>(i, i);
            refreshDescriptorOnSet<ELightSetBindings::LIGHT_AREA>(i, i);
        }

        // b11 has no PARTIALLY_BOUND — every element of every per-frame set
        // (and the domain twin) must hold a valid image before the first bind.
        device_    = info.device;
        descriptor_svc_ = info.descriptor_svc;
        allocator_ = info.allocator;
        if (createDefaultShadingInputImage())
        {
            // 不再 fill(default_input_view_):数组语义是"**提供者**给的 view",
            // NULL 表示该槽没有提供者,writeShadingInputDescriptors 会自己回落到
            // 默认纹理。fill 会把 init 之前 provideShadingInput 记下的 view 覆盖掉
            // —— 那正是 1.4 想修的静默丢弃换了个地方发生。
            for (uint32_t i = 0; i < frames_in_flight_; ++i)
                writeShadingInputDescriptors(i);
        }

        initialized_ = true;
        return true;
    }

    bool LightResources::createDefaultShadingInputImage()
    {
        if (device_ == VK_NULL_HANDLE || allocator_ == VK_NULL_HANDLE)
            return false;

        VkImageCreateInfo img_ci{};
        img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_ci.imageType = VK_IMAGE_TYPE_2D;
        img_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        img_ci.extent = {1, 1, 1};
        img_ci.mipLevels = 1;
        img_ci.arrayLayers = 1;
        img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        img_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        if (vmaCreateImage(allocator_, &img_ci, &alloc_ci,
                           &default_input_image_, &default_input_alloc_, nullptr) != VK_SUCCESS)
            return false;

        VkImageViewCreateInfo view_ci{};
        view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image = default_input_image_;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_ci.subresourceRange.levelCount = 1;
        view_ci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &view_ci, nullptr, &default_input_view_) != VK_SUCCESS)
        {
            destroyShadingInputResources();
            return false;
        }

        // One shared sampler for the whole array: shading inputs are
        // screen-space textures sampled at the pixel's own uv, so bilinear +
        // clamp covers every slot (a 1×1 default is filter-invariant anyway).
        // Exactly SamplerDesc::linearClamp(); owned by the DescriptorService
        // cache, so destroyShadingInputResources() must not destroy it.
        shading_input_sampler_ = descriptor_svc_->sampler(SamplerDesc::linearClamp());
        if (shading_input_sampler_ == VK_NULL_HANDLE)
        {
            destroyShadingInputResources();
            return false;
        }
        return true;
    }

    void LightResources::destroyShadingInputResources() noexcept
    {
        // No vkDestroySampler: shading_input_sampler_ is borrowed from the
        // DescriptorService cache, which keeps it alive until device teardown.
        vkDestroyImageView(device_, default_input_view_, nullptr);
        if (default_input_image_ != VK_NULL_HANDLE)
            vmaDestroyImage(allocator_, default_input_image_, default_input_alloc_);
        shading_input_sampler_ = VK_NULL_HANDLE;
        default_input_view_    = VK_NULL_HANDLE;
        default_input_image_   = VK_NULL_HANDLE;
        default_input_alloc_   = VK_NULL_HANDLE;
        shading_input_views_.fill(VK_NULL_HANDLE);
        default_input_cleared_ = false;
    }

    void LightResources::writeShadingInputDescriptors(uint32_t set_index) const
    {
        if (default_input_view_ == VK_NULL_HANDLE)
            return;

        std::array<VkDescriptorImageInfo, kShadingInputSlotCount> infos{};
        for (uint32_t s = 0; s < kShadingInputSlotCount; ++s)
        {
            infos[s].sampler     = shading_input_sampler_;
            infos[s].imageView   = shading_input_views_[s] != VK_NULL_HANDLE
                                       ? shading_input_views_[s] : default_input_view_;
            infos[s].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        // 阶段 C:域集是唯一写目标(legacy per-set 半边已删)。
        VkDescriptorSet ds = domainSetFor(set_index);
        if (ds == VK_NULL_HANDLE)
            return;

        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = ds;
        w.dstBinding = domain_.binding(static_cast<uint32_t>(ELightSetBindings::SHADING_INPUTS));
        w.descriptorCount = kShadingInputSlotCount;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = infos.data();
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }

    void LightResources::provideShadingInput(EShadingInputSlot slot, VkImageView view)
    {
        const auto index = static_cast<uint32_t>(slot);
        if (index >= kShadingInputSlotCount)
            return;   // 调用方传错槽位 —— 拒绝是对的

        // 先记下来,**再**看默认纹理建好没有。
        //
        // 此前这两件事挤在同一个条件里,于是默认纹理没就绪时提供者交进来的
        // view 被直接丢弃且无任何记录:SSAO 装了、pass 在跑、AO 纹理也产出了,
        // 但 b11 永远是默认白 —— 表现为"SSAO 没效果",无从查起。
        // 槽位表是纯 CPU 状态,任何时候记都安全;写描述符才需要默认纹理就绪
        //(数组里没被提供的元素要拿它填)。createDefaultShadingInputImage()
        // 成功后 init 会重写一遍全部元素,那时这里记下的 view 就带上了。
        shading_input_views_[index] = view;

        if (default_input_view_ == VK_NULL_HANDLE)
            return;
        for (uint32_t i = 0; i < frames_in_flight_; ++i)
            writeShadingInputDescriptors(i);
    }

    void LightResources::postTransfer(VkCommandBuffer cmd)
    {
        if (default_input_cleared_ || default_input_image_ == VK_NULL_HANDLE)
            return;

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = default_input_image_;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;

        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);

        // Neutral value for every slot: 1.0 in all channels (AO = unoccluded).
        VkClearColorValue white{};
        white.float32[0] = white.float32[1] = white.float32[2] = white.float32[3] = 1.0f;
        VkImageSubresourceRange range = barrier.subresourceRange;
        vkCmdClearColorImage(cmd, default_input_image_,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);

        default_input_cleared_ = true;
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
                for (uint32_t i = 0; i < kShadowCascadeSlots; ++i) gpu.cascade_splits[i] = d.cascade_splits[i];

                auto local_slot = std::get<SlicedSSBO<DirectionalLightGPU>>(ssbos_).add(gpu);
                LightHandle h = allocateGlobalHandle();
                binding_map_.insert(h, SlotRecord{
                    .binding = ELightSetBindings::LIGHT_DIRECTIONAL,
                    .local_slot = local_slot,
                });
                ++live_counts_[static_cast<std::size_t>(ELightSetBindings::LIGHT_DIRECTIONAL)];
                return h;
            }
            else if constexpr (std::is_same_v<T, PointLightDesc>) {
                PointLightGPU gpu{};
                fillCommonFromDesc(gpu, d.color, d.intensity, d.flags,
                                    d.shadow_map_size, d.shadow_bias, d.shadow_normal_bias);
                fillSpatialPosition(gpu, d.spatial_position);
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
                ++live_counts_[static_cast<std::size_t>(ELightSetBindings::LIGHT_POINT)];
                return h;
            }
            else if constexpr (std::is_same_v<T, SpotLightDesc>) {
                SpotLightGPU gpu{};
                fillCommonFromDesc(gpu, d.color, d.intensity, d.flags,
                                    d.shadow_map_size, d.shadow_bias, d.shadow_normal_bias);
                fillSpatialPosition(gpu, d.spatial_position);
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
                ++live_counts_[static_cast<std::size_t>(ELightSetBindings::LIGHT_SPOT)];
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
                ++live_counts_[static_cast<std::size_t>(ELightSetBindings::LIGHT_AREA)];
                return h;
            }
        }, desc);
    }

    void LightResources::remove(LightHandle handle)
    {
        auto* rec = binding_map_.find(handle);
        if (!rec) return;

        const auto binding = rec->binding;
        cancelIntensityTransition(handle);

        switch (binding) {
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
        auto& count = live_counts_[static_cast<std::size_t>(binding)];
        if (count > 0u)
            --count;
        releaseGlobalHandle(handle);
    }

    RenderError LightResources::modify(LightHandle handle, const LightDescriptor& desc)
    {
        auto* rec = binding_map_.find(handle);
        if (!rec)
            return renderError<err::resource::NotFound>();

        const ELightSetBindings binding = rec->binding;
        const SlotHandle local_slot = rec->local_slot;

        return std::visit([&](const auto& d) -> RenderError {
            using T = std::decay_t<decltype(d)>;

            if constexpr (std::is_same_v<T, DirectionalLightDesc>) {
                if (binding != ELightSetBindings::LIGHT_DIRECTIONAL)
                    return renderError<err::resource::UnsupportedType>();
                DirectionalLightGPU gpu{};
                fillCommonFromDesc(gpu, d.color, d.intensity, d.flags,
                                    d.shadow_map_size, d.shadow_bias, d.shadow_normal_bias);
                gpu.direction     = to_aligned3(d.direction.normalized());
                gpu.cascade_count = d.cascade_count;
                for (uint32_t i = 0; i < kShadowCascadeSlots; ++i) gpu.cascade_splits[i] = d.cascade_splits[i];
                auto& ssbo = std::get<SlicedSSBO<DirectionalLightGPU>>(ssbos_);
                return ssbo.modify(local_slot, gpu) ? RenderError{} : renderError<err::resource::ModifyFailed>();
            }
            else if constexpr (std::is_same_v<T, PointLightDesc>) {
                if (binding != ELightSetBindings::LIGHT_POINT)
                    return renderError<err::resource::UnsupportedType>();
                PointLightGPU gpu{};
                fillCommonFromDesc(gpu, d.color, d.intensity, d.flags,
                                    d.shadow_map_size, d.shadow_bias, d.shadow_normal_bias);
                fillSpatialPosition(gpu, d.spatial_position);
                gpu.range                 = d.range;
                gpu.attenuation_constant  = d.attenuation_constant;
                gpu.attenuation_linear    = d.attenuation_linear;
                gpu.attenuation_quadratic = d.attenuation_quadratic;
                auto& ssbo = std::get<SlicedSSBO<PointLightGPU>>(ssbos_);
                return ssbo.modify(local_slot, gpu) ? RenderError{} : renderError<err::resource::ModifyFailed>();
            }
            else if constexpr (std::is_same_v<T, SpotLightDesc>) {
                if (binding != ELightSetBindings::LIGHT_SPOT)
                    return renderError<err::resource::UnsupportedType>();
                SpotLightGPU gpu{};
                fillCommonFromDesc(gpu, d.color, d.intensity, d.flags,
                                    d.shadow_map_size, d.shadow_bias, d.shadow_normal_bias);
                fillSpatialPosition(gpu, d.spatial_position);
                gpu.direction             = to_aligned3(d.direction.normalized());
                gpu.range                 = d.range;
                gpu.attenuation_constant  = d.attenuation_constant;
                gpu.attenuation_linear    = d.attenuation_linear;
                gpu.attenuation_quadratic = d.attenuation_quadratic;
                gpu.inner_cone_angle      = d.inner_cone_angle;
                gpu.outer_cone_angle      = d.outer_cone_angle;
                auto& ssbo = std::get<SlicedSSBO<SpotLightGPU>>(ssbos_);
                return ssbo.modify(local_slot, gpu) ? RenderError{} : renderError<err::resource::ModifyFailed>();
            }
            else if constexpr (std::is_same_v<T, AreaLightDesc>) {
                if (binding != ELightSetBindings::LIGHT_AREA)
                    return renderError<err::resource::UnsupportedType>();
                AreaLightGPU gpu{};
                fillCommonFromDesc(gpu, d.color, d.intensity, d.flags,
                                    d.shadow_map_size, d.shadow_bias, d.shadow_normal_bias);
                gpu.size = to_aligned2(d.size);
                auto& ssbo = std::get<SlicedSSBO<AreaLightGPU>>(ssbos_);
                return ssbo.modify(local_slot, gpu) ? RenderError{} : renderError<err::resource::ModifyFailed>();
            }
        }, desc);
    }

}
