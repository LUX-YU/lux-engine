#pragma once
#include <lux/engine/render/resource/instance/Data.hpp>
#include <lux/engine/render/resource/ResourceProjection.hpp>

namespace lux::render
{
    enum class EMeshVisibilityExtent : std::uint32_t { VISIBLE_MEMBERS, INDEXED_COMMANDS };

    enum class EMeshInstanceBinding : std::uint8_t
    {
        PROPERTIES,
        ALIVE,
        // Named graph import for a paged session. Both fields are reached from the same root.
        PAGED_ROOT,
        CANDIDATES,
        CHUNKS,
        CAMERA_GROUPS,
        CAMERA_DISPATCH
    };

    [[nodiscard]] constexpr std::uint64_t meshInstanceProjectionBytes(EMeshInstanceBinding role) noexcept
    {
        switch (role)
        {
        case EMeshInstanceBinding::PROPERTIES:
            return sizeof(MeshInstanceGpuProperty);
        case EMeshInstanceBinding::ALIVE:
            return 2 * sizeof(std::uint32_t);
        case EMeshInstanceBinding::CANDIDATES: return sizeof(std::uint32_t);
        case EMeshInstanceBinding::CHUNKS: return 2 * sizeof(std::uint32_t);
        case EMeshInstanceBinding::CAMERA_GROUPS: return sizeof(MeshDrawGroupRecord);
        case EMeshInstanceBinding::CAMERA_DISPATCH: return 6 * sizeof(std::uint32_t);
        case EMeshInstanceBinding::PAGED_ROOT:
            return kMeshInstanceTableAxis * sizeof(std::uint64_t);
        }
        return 0;
    }

    [[nodiscard]] inline ResourceProjection meshInstanceProjection(EMeshInstanceBinding role) noexcept
    {
        const auto bytes = meshInstanceProjectionBytes(role);
        if (role == EMeshInstanceBinding::CAMERA_DISPATCH)
            return {GraphBufferDesc{bytes}, {GraphBufferRange{0, bytes}}, EGraphUse::INDIRECT, EGraphUse::INDIRECT};
        const bool shader_read = role >= EMeshInstanceBinding::CANDIDATES;
        const auto boundary = shader_read ? EGraphUse::SHADER_BUFFER_READ : EGraphUse::STORAGE_READ;
        return {GraphBufferDesc{bytes}, {GraphBufferRange{0, bytes}}, boundary, EGraphUse::STORAGE_READ};
    }

    struct MeshInstanceBinding final
    {
        EMeshInstanceBinding role = EMeshInstanceBinding::PROPERTIES;
        bool operator==(const MeshInstanceBinding&) const noexcept = default;
    };

} // namespace lux::render
