#pragma once
#include <lux/engine/render/resource/instance/Data.hpp>

namespace lux::render
{
    enum class EMeshInstanceBinding : std::uint8_t
    {
        PROPERTIES,
        ALIVE,
        // Named graph import for a paged session. Both fields are reached from the same root.
        PAGED_ROOT
    };

    [[nodiscard]] constexpr std::uint64_t meshInstanceProjectionBytes(EMeshInstanceBinding role) noexcept
    {
        switch (role)
        {
        case EMeshInstanceBinding::PROPERTIES:
            return sizeof(MeshInstanceGpuProperty);
        case EMeshInstanceBinding::ALIVE:
            return 2 * sizeof(std::uint32_t);
        case EMeshInstanceBinding::PAGED_ROOT:
            return kMeshInstanceTableAxis * sizeof(std::uint64_t);
        }
        return 0;
    }

    struct MeshInstanceBinding final
    {
        EMeshInstanceBinding role = EMeshInstanceBinding::PROPERTIES;
        bool operator==(const MeshInstanceBinding&) const noexcept = default;
    };

} // namespace lux::render
