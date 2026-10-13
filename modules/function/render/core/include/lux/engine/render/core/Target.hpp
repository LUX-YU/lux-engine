#pragma once

#include <cstdint>
#include <lux/engine/description/Image.hpp>
#include <lux/engine/render/core/Error.hpp>

namespace lux::render
{
    enum class ETargetKind : std::uint8_t
    {
        OFFSCREEN,
        SURFACE
    };

    // A final color-output specification, not a Native resource or runtime owner.
    // Device limits, supported formats/sample counts and WSI eligibility are later checks.
    struct TargetDesc
    {
        std::uint32_t width{};
        std::uint32_t height{};
        rdesc::ETextureFormat format{rdesc::ETextureFormat::UNDEFINED};
        std::uint32_t samples{1};
        ETargetKind kind{ETargetKind::OFFSCREEN};

        [[nodiscard]] RenderResult<void> validate() const noexcept;
        bool operator==(const TargetDesc&) const noexcept = default;
    };

    // A future Target owner's uint64_t target_epoch changes when its specification
    // changes in place. It is independent of TargetId::gen and is not stored here:
    // two identical descriptions need not belong to the same object or revision.
}
