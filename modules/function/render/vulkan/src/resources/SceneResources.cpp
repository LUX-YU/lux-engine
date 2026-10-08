#include <limits>
#include <lux/engine/render/resources/SceneResources.hpp>

namespace lux::render
{
    struct SceneResources::Backing
    {
        DomainWriteTarget domain;
        SceneGlobalBuffer scene;
        ViewBuffer view;
        std::vector<VkDescriptorSet> sets;
    };

    SceneResources::CreateResult SceneResources::create(const CreateInfo& info) noexcept
    {
        const bool is_invalid_frames = info.slices == 0 || info.slices > kMaxFramesInFlight;
        const bool is_missing_backing = !info.arena || !info.set_layout || !info.device_context.logicalDevice();
        const bool is_empty_capacity = info.initial_scene_capacity == 0 || info.initial_view_capacity == 0;
        const bool is_incomplete_target =
            info.domain_sets.size() != info.slices ||
            std::ranges::any_of(info.domain_sets, [](VkDescriptorSet set) { return set == VK_NULL_HANDLE; });
        const auto maximum_binding = std::max(info.binding_scene_global, info.binding_view_data);
        const bool is_invalid_binding =
            info.binding_scene_global == info.binding_view_data ||
            info.domain_binding_offset > std::numeric_limits<uint32_t>::max() - maximum_binding;
        const bool is_invalid_configuration =
            is_invalid_frames || is_missing_backing || is_empty_capacity || is_incomplete_target || is_invalid_binding;
        if (is_invalid_configuration)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        DomainWriteTarget domain;
        if (auto accepted = domain.set(info.domain_sets, info.domain_binding_offset); !accepted)
        {
            return lux::cxx::unexpected(accepted.error());
        }
        GpuBufferCreateInfo config{};
        config.device_context = &info.device_context;
        config.deferred_queue = &info.deferred_queue;
        config.slices = info.slices;
        config.buffer_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        config.allow_shader_write = false;
        config.initial_capacity = info.initial_scene_capacity;
        auto scene = SceneGlobalBuffer::create(config);
        if (!scene)
        {
            return lux::cxx::unexpected(scene.error());
        }
        config.initial_capacity = info.initial_view_capacity;
        auto view = ViewBuffer::create(config);
        if (!view)
        {
            return lux::cxx::unexpected(view.error());
        }
        // The original arena retains successful prefix allocations until its safe point.
        // No descriptors are written and no semantic resource is published on rejection.
        std::vector<VkDescriptorSet> sets;
        sets.reserve(info.slices);
        for (uint32_t slice = 0; slice < info.slices; ++slice)
        {
            auto allocated = info.arena->allocate(info.set_layout);
            if (!allocated)
            {
                return lux::cxx::unexpected(allocated.error());
            }
            sets.push_back(*allocated);
        }
        Backing backing{std::move(domain), std::move(*scene), std::move(*view), std::move(sets)};
        return std::unique_ptr<SceneResources>(new SceneResources(info, std::move(backing)));
    }

    SceneResources::SceneResources(const CreateInfo& info, Backing&& backing) noexcept
        : domain_(std::move(backing.domain)), binding_scene_global_(info.binding_scene_global),
          binding_view_data_(info.binding_view_data), scene_buf_(std::move(backing.scene)),
          view_buf_(std::move(backing.view)), descriptor_sets_(std::move(backing.sets))
    {
        for (uint32_t slice = 0; slice < descriptor_sets_.size(); ++slice)
        {
            writeDescriptorForSet(slice);
        }
    }

    void SceneResources::writeDescriptorForSet(uint32_t slice) noexcept
    {
        scene_buf_.writeDescriptorSlice(descriptor_sets_[slice], binding_scene_global_, slice);
        view_buf_.writeDescriptorSlice(descriptor_sets_[slice], binding_view_data_, slice);
        scene_buf_.writeDescriptorSlice(domain_.setFor(slice), domain_.binding(binding_scene_global_), slice);
        view_buf_.writeDescriptorSlice(domain_.setFor(slice), domain_.binding(binding_view_data_), slice);
    }

} // namespace lux::render
