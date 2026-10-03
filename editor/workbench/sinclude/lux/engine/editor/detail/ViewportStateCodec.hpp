#pragma once

#include <lux/engine/editor/views/CameraNavigation.hpp>
#include <lux/engine/render/RendererConfig.hpp>
#include <lux/engine/serialization/BinaryReader.hpp>
#include <cmath>

namespace lux::editor::views::detail
{
    // Private shared codec for the two actual viewport owners. No binding, author content or asset locator.
    inline void writeViewportState(
        serialization::BinaryWriter& writer,
        const ViewportCameraState& pose,
        render::PixelExtent extent
    )
    {
        for (const auto value : pose.transform.translation)
            (void)writer.writeFloat(value);
        for (const auto value : pose.transform.rotation.coeffs())
            (void)writer.writeFloat(value);
        for (const auto value : pose.transform.scale)
            (void)writer.writeFloat(value);
        (void)writer.writeUnsigned(static_cast<std::uint8_t>(pose.camera.projection.index()));
        std::visit(
            [&](const auto& projection)
            {
                using T = std::decay_t<decltype(projection)>;
                if constexpr (std::same_as<T, lux::scene::PerspectiveProjection>)
                    (void)writer.writeFloat(projection.vertical_fov);
                else
                    (void)writer.writeFloat(projection.vertical_extent);
                (void)writer.writeFloat(projection.near_plane);
                (void)writer.writeFloat(projection.far_plane);
            },
            pose.camera.projection
        );
        (void)writer.writeUnsigned(static_cast<std::uint8_t>(pose.camera.primary));
        (void)writer.writeUnsigned(extent.width);
        (void)writer.writeUnsigned(extent.height);
    }

    inline bool readViewportState(
        serialization::BinaryReader& reader,
        ViewportCameraState& pose,
        render::PixelExtent& extent
    )
    {
        const auto scalar = [&reader](double& value)
        {
            auto decoded = reader.readFloat<double>();
            if (!decoded || !std::isfinite(*decoded))
                return false;
            value = *decoded;
            return true;
        };
        for (auto& value : pose.transform.translation)
            if (!scalar(value))
                return false;
        for (auto& value : pose.transform.rotation.coeffs())
            if (!scalar(value))
                return false;
        for (auto& value : pose.transform.scale)
            if (!scalar(value))
                return false;
        const auto kind = reader.readUnsigned<std::uint8_t>();
        double shape{}, near_plane{}, far_plane{};
        if (!kind || *kind > 1 || !scalar(shape) || !scalar(near_plane) || !scalar(far_plane))
            return false;
        if (*kind == 0)
            pose.camera.projection = lux::scene::PerspectiveProjection{shape, near_plane, far_plane};
        else
            pose.camera.projection = lux::scene::OrthographicProjection{shape, near_plane, far_plane};
        const auto primary = reader.readUnsigned<std::uint8_t>();
        const auto width = reader.readUnsigned<std::uint32_t>(), height = reader.readUnsigned<std::uint32_t>();
        const bool invalid_extent = !width || !height || !*width || !*height || *width > 65536 || *height > 65536;
        const bool invalid_transform =
            std::abs(pose.transform.rotation.norm() - 1.0) > 1e-6 || (pose.transform.scale.array().abs() < 1e-12).any();
        if (!primary || *primary > 1 || invalid_extent || invalid_transform)
            return false;
        pose.camera.primary = *primary != 0;
        extent = {*width, *height};
        return lux::scene::cameraProjection(pose.camera, double(*width) / *height).has_value();
    }
} // namespace lux::editor::views::detail
