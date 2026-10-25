#pragma once

#include <lux/engine/render/capabilities/RenderCapability.hpp>
#include <lux/engine/render/core/ResourceId.hpp>

#include <lux/engine/render/visibility.h>

#include <algorithm>
#include <array>
#include <lux/engine/description/MaterialProgramSet.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/inputs/SampledTextureInputs.hpp>

namespace lux::render
{
    struct MaterialTag;
    using MaterialId = ResourceId<MaterialTag>;
    // Client-supplied finite values in the Cooked parameter order; Render checks the actual layout and count.
    using MaterialParameters = std::vector<std::array<float, 4>>;

    struct MaterialTextureGpuRef final
    {
        std::uint32_t representation_index = 0;
        std::uint32_t resource_index = 0;
        std::uint32_t sampler_index = 0;
        std::uint32_t flags = 0;
    };

    static_assert(sizeof(MaterialTextureGpuRef) == 16);

    struct MaterialWrite final
    {
        CapabilityInput destination;
        rdesc::MaterialProgramSet program;
        MaterialParameters parameters{};
        // Client keeps referenced textures valid and explicitly updates these fields before releasing them.
        std::vector<TextureId> textures;
    };

    struct CreateMaterial final
    {
        static constexpr std::string_view name = "lux.render.CreateMaterial";
        static constexpr bool frame_scoped = true;
        using Request = MaterialWrite;

        struct Response final
        {
            RenderError error{};
            MaterialId material{};
        };
    };

    struct MaterialUpdate final
    {
        MaterialId material;
        MaterialParameters parameters;
        std::vector<TextureId> textures;
    };

    struct UpdateMaterial final
    {
        static constexpr std::string_view name = "lux.render.UpdateMaterial";
        static constexpr bool frame_scoped = true;
        // Related writes and releases are ordered by the Client, including across Client connections.
        // Program and table slot remain fixed. A different program requires a new Material identity.
        using Request = MaterialUpdate;
        using Response = AckResponse;
    };

    struct ReleaseMaterial final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name = "lux.render.ReleaseMaterial";
        static constexpr bool frame_scoped = true;
        using Request = MaterialId;
        using Response = AckResponse;
    };

    template <> struct ConfigValueTraits<rdesc::MaterialProgramSet> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t>
        retainedBytes(const rdesc::MaterialProgramSet&) noexcept;
    };

} // namespace lux::render
