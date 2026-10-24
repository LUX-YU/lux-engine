#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/core/Target.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace lux::scene
{
    // Scene-local, never reused. Route with the Scene instance and the CameraMan entity generation.
    struct CameraTargetId final
    {
        std::uint64_t value{};

        [[nodiscard]] bool valid() const noexcept
        {
            return value != 0;
        }

        bool operator==(const CameraTargetId&) const noexcept = default;
    };

    // Runtime output intent. description is the actual CreateTarget/ResizeTarget reply, not a requested size.
    // The caller keeps target alive until RenderSystem confirms this relation has retired.
    struct CameraTarget final
    {
        CameraTargetId id;
        render::TargetId target;
        render::TargetDesc description;
        render::RasterViewport viewport;
        float exposure{};
        std::array<float, 4> background{0, 0, 0, 1};
        std::uint64_t revision{};
        bool active = true;
        std::uint64_t epoch = 1;
    };

    // Intent only; no Render identities, callbacks or persistence codec.
    struct CameraTargetsComponent final
    {
        std::vector<CameraTarget> targets;
    };
} // namespace lux::scene
