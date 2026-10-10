#include <lux/engine/render/gpu/descriptor/SceneDescriptorArena.hpp>
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DInstanceArena.hpp>

namespace lux::render
{
    Canvas2DInstanceArena::CreateResult Canvas2DInstanceArena::create(const CreateInfo& info) noexcept
    {
        const bool is_empty_capacity = info.initial_capacity == 0 || info.max_capacity == 0;
        const bool is_invalid_extent =
            info.initial_capacity > info.max_capacity || info.max_capacity > kCanvas2DSlotMask + 1u;
        const bool is_invalid_configuration = is_empty_capacity || is_invalid_extent;
        if (is_invalid_configuration)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }

        constexpr VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        for (std::uint32_t binding = 0; binding < bindings.size(); ++binding)
        {
            bindings[binding] = {binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr};
        }
        const std::array<VkDescriptorBindingFlags, 2> flags{
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
        };
        const DescriptorLayoutDesc layout_info{
            .bindings = bindings,
            .binding_flags = flags,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
            .debug_name = "Canvas2DInstanceArena"
        };
        auto layout = info.descriptors.registerLayout(layout_info);
        if (!layout)
        {
            return lux::cxx::unexpected(layout.error());
        }
        auto images = TKindStore<Image2DGpuData>::prepare(info.device, info.retirement, info.initial_capacity);
        if (!images)
        {
            return lux::cxx::unexpected(images.error());
        }
        auto fields = TKindStore<PixelField2DGpuData>::prepare(info.device, info.retirement, 64);
        if (!fields)
        {
            return lux::cxx::unexpected(fields.error());
        }
        auto tiles = TKindStore<Tile2DGpuData>::prepare(info.device, info.retirement, 64);
        if (!tiles)
        {
            return lux::cxx::unexpected(tiles.error());
        }
        const auto native_layout = info.descriptors.layout(*layout);
        const std::array layouts{native_layout, native_layout, native_layout};
        auto sets = info.arena.allocateBatch(layouts);
        if (!sets)
        {
            return lux::cxx::unexpected(sets.error());
        }
        return std::unique_ptr<Canvas2DInstanceArena>(new Canvas2DInstanceArena(
            info,
            std::move(*images),
            std::move(*fields),
            std::move(*tiles),
            std::span<const VkDescriptorSet, 3>{*sets}
        ));
    }

    Canvas2DInstanceArena::Canvas2DInstanceArena(
        const CreateInfo& info,
        TKindStore<Image2DGpuData>::Backing&& images,
        TKindStore<PixelField2DGpuData>::Backing&& fields,
        TKindStore<Tile2DGpuData>::Backing&& tiles,
        std::span<const VkDescriptorSet, 3> sets
    ) noexcept
        : textures_(info.textures),
          images_(info.device, info.retirement, std::move(images), sets[0], info.max_capacity),
          fields_(info.device, info.retirement, std::move(fields), sets[1], 4096),
          tiles_(info.device, info.retirement, std::move(tiles), sets[2], 4096),
          group_count_(1u + std::min(info.offscreen_groups, kMaxCanvas2DGroups))
    {
    }
} // namespace lux::render
