#pragma once

#include <lux/engine/render/resources/texture/Data.hpp>

namespace lux::render
{
    struct TextureBinding final
    {
        TextureId texture;
        bool operator==(const TextureBinding&) const noexcept = default;
    };

} // namespace lux::render
