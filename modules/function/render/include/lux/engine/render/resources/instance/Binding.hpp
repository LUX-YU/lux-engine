#pragma once
#include <lux/engine/render/resources/instance/Data.hpp>

namespace lux::render
{
    enum class EMeshInstanceBinding : std::uint8_t
    {
        TRANSFORMS,
        PROPERTIES,
        ALIVE
    };

    struct MeshInstanceBinding final
    {
        EMeshInstanceBinding role = EMeshInstanceBinding::TRANSFORMS;
        bool operator==(const MeshInstanceBinding&) const noexcept = default;
    };

} // namespace lux::render
