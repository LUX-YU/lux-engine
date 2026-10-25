#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/description/Image.hpp>
#include <lux/engine/render/capabilities/RenderCapability.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/core/ResourceId.hpp>
#include <span>
#include <vector>

namespace lux::render
{
    class ResourceRegistry;
    struct MeshTag;
    using MeshId = ResourceId<MeshTag>;

    // Asset layout, independent of the Shader's final descriptor placement and Pipeline state.
    struct MeshAttribute final
    {
        std::uint32_t location = 0, offset = 0;
        rdesc::ETextureFormat format = rdesc::ETextureFormat::UNDEFINED;
        bool operator==(const MeshAttribute&) const noexcept = default;
    };

    struct MeshSubmesh final
    {
        std::uint32_t first_index = 0, index_count = 0;
        std::int32_t vertex_offset = 0;
        bool operator==(const MeshSubmesh&) const noexcept = default;
    };

    struct MeshGpuBounds final
    {
        std::array<float, 4> center{}; // w = 1 for trusted bounds, 0 means conservatively visible.
        std::array<float, 4> half_extent{};
        bool operator==(const MeshGpuBounds&) const noexcept = default;
    };

    static_assert(sizeof(MeshGpuBounds) == 32);

    struct MeshDescription final
    {
        std::uint32_t vertex_stride = 0, vertex_count = 0;
        std::uint64_t index_offset = 0;
        std::uint32_t index_stride = 4, index_count = 0;
        std::vector<MeshAttribute> attributes;
        std::vector<MeshSubmesh> submeshes;
        MeshGpuBounds bounds;
        bool operator==(const MeshDescription&) const noexcept = default;
    };

    // Actual geometry record consumed when a Feature lowers a section to an indirect draw.
    // Instance count and selection belong to the Feature, not this asset table.
    struct MeshGpuSection final
    {
        std::uint32_t index_count = 0, first_index = 0;
        std::int32_t vertex_offset = 0;
    };

    static_assert(sizeof(MeshGpuSection) == 12);

    struct MeshWrite final
    {
        CapabilityInput destination;
        MeshDescription description;
        // Borrowed until Response (success, failure or cancellation). Failed submission takes no borrow.
        // Abandon/connection loss is not completion: the application must drain Server and Producers before freeing it.
        std::span<const std::byte> source;
    };

    struct CreateMesh final
    {
        static constexpr std::string_view name = "lux.render.CreateMesh";
        static constexpr bool frame_scoped = false;
        using Request = MeshWrite;

        struct Response final
        {
            RenderError error{};
            MeshId mesh{};
        };
    };

    struct ReleaseMesh final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name = "lux.render.ReleaseMesh";
        static constexpr bool frame_scoped = true;
        using Request = MeshId;
        using Response = AckResponse;
    };

    template <> struct ConfigValueTraits<MeshDescription> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t> retainedBytes(const MeshDescription&) noexcept;
    };

} // namespace lux::render
