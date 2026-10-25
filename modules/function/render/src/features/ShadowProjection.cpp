#include <lux/engine/render/features/ShadowProjection.hpp>

#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <limits>
#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>

namespace lux::render::detail
{
    namespace
    {
        using Matrix = Eigen::Matrix<double, 4, 4, Eigen::ColMajor>;
        using Vector = Eigen::Vector3d;

        Vector xyz(const std::array<float, 4>& value) noexcept
        {
            return {value[0], value[1], value[2]};
        }

        Matrix lightView(const Vector& position, const Vector& forward, const Vector& seed) noexcept
        {
            // Legacy makePerspectiveLightSlice basis. +Z points away from the light.
            const Vector right = forward.cross(seed).normalized();
            const Vector up = right.cross(forward).normalized();
            Matrix view = Matrix::Identity();
            view.block<1, 3>(0, 0) = right.transpose();
            view.block<1, 3>(1, 0) = up.transpose();
            view.block<1, 3>(2, 0) = forward.transpose();
            view(0, 3) = -right.dot(position);
            view(1, 3) = -up.dot(position);
            view(2, 3) = -forward.dot(position);
            return view;
        }

        Vector upSeed(const Vector& direction) noexcept
        {
            return std::abs(direction.y()) < 0.95 ? Vector::UnitY() : Vector::UnitZ();
        }

        Matrix perspective(double scale, double near_z, double far_z) noexcept
        {
            Matrix projection = Matrix::Zero();
            projection(0, 0) = projection(1, 1) = scale;
            projection(2, 2) = far_z / (far_z - near_z);
            projection(2, 3) = -far_z * near_z / (far_z - near_z);
            projection(3, 2) = 1;
            // Unlike Legacy's negative viewport convention, NRI's selected positive-height viewport
            // and sampling both map clip.xy through 0.5*x+0.5. No additional Y inversion is needed.
            return projection;
        }

        void storeView(const Matrix& matrix, MeshVisibilityView& result) noexcept
        {
            for (std::size_t i = 0; i < result.world_to_clip.size(); ++i)
            {
                result.world_to_clip[i] = static_cast<float>(matrix.data()[i]);
            }
            const std::array<Eigen::Vector4d, 6> planes{
                matrix.row(3) + matrix.row(0),
                matrix.row(3) - matrix.row(0),
                matrix.row(3) + matrix.row(1),
                matrix.row(3) - matrix.row(1),
                matrix.row(2),
                matrix.row(3) - matrix.row(2)
            };
            for (std::size_t i = 0; i < planes.size(); ++i)
            {
                const auto plane = planes[i] / planes[i].head<3>().norm();
                for (unsigned j = 0; j < 4; ++j)
                {
                    result.planes[i][j] = static_cast<float>(plane[j]);
                }
            }
            result.flags[0] = 1;
        }

        Matrix directional(
            const Vector& direction,
            const std::array<Vector, 8>& corners,
            double near_fraction,
            double far_fraction,
            double caster_distance,
            double guard_texels,
            std::uint32_t resolution
        ) noexcept
        {
            const Matrix view = lightView(Vector::Zero(), direction, upSeed(direction));
            Vector minimum = Vector::Constant(std::numeric_limits<double>::max());
            Vector maximum = -minimum;
            for (unsigned i = 0; i < 4; ++i)
            {
                const Vector ray = corners[i + 4] - corners[i];
                for (double t : {near_fraction, far_fraction})
                {
                    const Vector point = (view * (corners[i] + t * ray).homogeneous()).head<3>();
                    minimum = minimum.cwiseMin(point);
                    maximum = maximum.cwiseMax(point);
                }
            }
            // Legacy segment fit, texel snapping and guard band; explicit caller-supplied caster distance
            // replaces its hidden near pullback and split-unit heuristics.
            Vector extent = maximum - minimum;
            for (unsigned axis = 0; axis < 2; ++axis)
            {
                const double padded = extent[axis] * resolution / (resolution - 2 * guard_texels - 2);
                const double texel = padded / resolution;
                const double center = std::round((maximum[axis] + minimum[axis]) * 0.5 / texel) * texel;
                minimum[axis] = center - padded * 0.5;
                maximum[axis] = center + padded * 0.5;
            }
            minimum.z() -= caster_distance;
            extent = maximum - minimum;
            Matrix projection = Matrix::Identity();
            projection(0, 0) = 2 / extent.x();
            projection(1, 1) = 2 / extent.y();
            projection(2, 2) = 1 / extent.z();
            projection(0, 3) = -(maximum.x() + minimum.x()) / extent.x();
            projection(1, 3) = -(maximum.y() + minimum.y()) / extent.y();
            projection(2, 3) = -minimum.z() / extent.z();
            return projection * view;
        }
    } // namespace

    RenderResult<void> validateShadowConfiguration(const ShadowMapFeatureConfig& config) noexcept
    {
        const bool invalid_shape = !config.shape.view_count || config.pcf_radius > 4 ||
                                   config.shape.resolution <= 2 * (config.pcf_radius + 2) || config.lights.empty() ||
                                   config.camera_offset > UINT32_MAX - sizeof(ShadowCamera);
        if (invalid_shape)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        std::uint64_t count = 0;
        for (std::size_t i = 0; i < config.casters.size(); ++i)
        {
            const auto& light = config.casters[i];
            const bool invalid_identity = light.light.isNull() || light.kind >= EDirectLightingBuffer::COUNTS;
            const bool invalid_distances = !std::isfinite(light.near_distance) || light.near_distance <= 0 ||
                                           !std::isfinite(light.caster_distance) || light.caster_distance <= 0;
            const bool invalid_bias = !std::isfinite(light.normal_offset) || light.normal_offset < 0 ||
                                      !std::isfinite(light.comparison_offset) || light.comparison_offset < 0 ||
                                      !std::isfinite(light.depth_bias_constant) ||
                                      !std::isfinite(light.depth_bias_slope);
            const bool duplicate = std::ranges::any_of(
                std::span(config.casters).first(i),
                [&](const auto& previous) { return previous.light == light.light; }
            );
            if (invalid_identity || invalid_distances || invalid_bias || duplicate)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            if (light.kind == EDirectLightingBuffer::DIRECTIONAL)
            {
                float previous = 0;
                for (float distance : light.cascade_far_distances)
                {
                    if (!std::isfinite(distance) || distance <= previous)
                    {
                        return cxx::unexpected(RenderError{kFeatureContract});
                    }
                    previous = distance;
                }
                if (light.cascade_far_distances.empty())
                {
                    return cxx::unexpected(RenderError{kFeatureContract});
                }
                count += light.cascade_far_distances.size();
            }
            else
            {
                count += light.kind == EDirectLightingBuffer::POINT ? 6 : 1;
            }
        }
        return count <= config.shape.view_count ? RenderResult<void>{} : cxx::unexpected(RenderError{kFeatureContract});
    }

    RenderResult<void> projectShadows(
        const ShadowMapFeatureConfig& config,
        const ShadowCamera& camera,
        std::span<const ShadowLightInput> lights,
        std::span<MeshVisibilityView> views,
        std::span<ShadowSliceGpu> slices,
        std::span<LightShadowRange> mappings,
        const std::array<std::uint32_t, 3>& offsets
    ) noexcept
    {
        const double near_z = camera.position_near[3], far_z = camera.direction_far[3];
        const bool invalid_camera =
            !std::isfinite(near_z) || !std::isfinite(far_z) || near_z <= 0 || far_z <= near_z ||
            !std::ranges::all_of(camera.inverse_view_projection, [](float v) { return std::isfinite(v); }) ||
            !std::ranges::all_of(camera.position_near, [](float v) { return std::isfinite(v); }) ||
            !std::ranges::all_of(camera.direction_far, [](float v) { return std::isfinite(v); }) ||
            std::abs(xyz(camera.direction_far).squaredNorm() - 1) > 0.001;
        if (invalid_camera)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        std::ranges::fill(views, MeshVisibilityView{});
        std::ranges::fill(slices, ShadowSliceGpu{});
        std::ranges::fill(mappings, LightShadowRange{});
        Matrix inverse;
        for (unsigned i = 0; i < 16; ++i)
        {
            inverse.data()[i] = camera.inverse_view_projection[i];
        }
        if (inverse.determinant() == 0)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        std::array<Vector, 8> corners;
        for (unsigned i = 0; i < corners.size(); ++i)
        {
            const Eigen::Vector4d point = inverse * Eigen::Vector4d(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i >> 2, 1);
            if (!point.allFinite() || std::abs(point.w()) < 1e-12)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            corners[i] = point.head<3>() / point.w();
        }
        const double guard = config.pcf_radius + 1;
        const double scale = (config.shape.resolution - 2 * guard) / config.shape.resolution;
        std::uint32_t first = 0;
        for (std::size_t i = 0; i < config.casters.size(); ++i)
        {
            const auto& selection = config.casters[i];
            const auto& light = lights[i];
            const auto kind = static_cast<std::uint32_t>(selection.kind);
            const auto count = kind == 0   ? static_cast<std::uint32_t>(selection.cascade_far_distances.size())
                               : kind == 1 ? 6u
                                           : 1u;
            if (!light.active)
            {
                first += count;
                continue;
            }
            mappings[offsets[kind] + light.dense_index] = {first, count, kind};
            for (std::uint32_t face = 0; face < count; ++face)
            {
                auto& slice = slices[first + face];
                slice.identity = {first + face, 1, kind != 0, 0};
                const float inset = static_cast<float>(guard / config.shape.resolution);
                slice.inner_uv = {inset, inset, 1 - inset, 1 - inset};
                slice.sampling =
                    {selection.normal_offset, selection.comparison_offset, static_cast<float>(config.pcf_radius), 0};
                Matrix matrix;
                if (kind == 0)
                {
                    const double begin = face ? selection.cascade_far_distances[face - 1] : near_z;
                    const double end = selection.cascade_far_distances[face];
                    if (begin < near_z || end > far_z || end <= begin)
                    {
                        return cxx::unexpected(RenderError{kFeatureContract});
                    }
                    matrix = directional(
                        xyz(std::get<DirectionalLightGpu>(light.value).direction),
                        corners,
                        (begin - near_z) / (far_z - near_z),
                        (end - near_z) / (far_z - near_z),
                        selection.caster_distance,
                        guard,
                        config.shape.resolution
                    );
                    slice.depths = {static_cast<float>(begin), static_cast<float>(end), 0, 0};
                }
                else
                {
                    Vector position, direction, up;
                    double far_light, projection_scale = scale;
                    if (kind == 1)
                    {
                        const auto& point = std::get<PointLightGpu>(light.value);
                        position = xyz(point.position_range);
                        far_light = point.position_range[3];
                        const std::array<Vector, 6> directions{
                            Vector::UnitX(),
                            -Vector::UnitX(),
                            Vector::UnitY(),
                            -Vector::UnitY(),
                            Vector::UnitZ(),
                            -Vector::UnitZ()
                        };
                        const std::array<Vector, 6> ups{
                            Vector::UnitY(),
                            Vector::UnitY(),
                            Vector::UnitZ(),
                            -Vector::UnitZ(),
                            Vector::UnitY(),
                            Vector::UnitY()
                        };
                        direction = directions[face];
                        up = ups[face];
                    }
                    else
                    {
                        const auto& spot = std::get<SpotLightGpu>(light.value);
                        position = xyz(spot.position_range);
                        far_light = spot.position_range[3];
                        direction = xyz(spot.direction_outer);
                        up = upSeed(direction);
                        const double cosine = spot.direction_outer[3];
                        if (cosine <= 0 || cosine >= 1)
                        {
                            return cxx::unexpected(RenderError{kFeatureContract});
                        }
                        projection_scale *= cosine / std::sqrt(1 - cosine * cosine);
                    }
                    if (selection.near_distance >= far_light)
                    {
                        return cxx::unexpected(RenderError{kFeatureContract});
                    }
                    matrix = perspective(projection_scale, selection.near_distance, far_light) *
                             lightView(position, direction, up);
                    slice.depths = {selection.near_distance, static_cast<float>(far_light), 0, 0};
                }
                if (!matrix.allFinite())
                {
                    return cxx::unexpected(RenderError{kFeatureContract});
                }
                storeView(matrix, views[first + face]);
            }
            first += count;
        }
        return {};
    }
} // namespace lux::render::detail
