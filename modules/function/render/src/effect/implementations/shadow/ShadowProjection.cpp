#include <lux/engine/render/effect/implementations/shadow/ShadowProjection.hpp>

#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <lux/engine/render/graph/GraphError.hpp>

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

        std::uint32_t shadowLayers(const ShadowLightRecord& light) noexcept
        {
            if (std::holds_alternative<DirectionalLightGpu>(light.value)) return light.settings.cascade_count;
            return std::holds_alternative<PointLightGpu>(light.value) ? 6u : 1u;
        }

        Matrix directional(
            const Vector& direction, const std::array<Vector, 8>& corners,
            std::array<double, 2> depth_interval, const ShadowProjectionSettings& settings
        ) noexcept
        {
            const double resolution = settings.shape.resolution;
            const double guard_texels = settings.pcf_radius + 1;
            const Matrix view = lightView(Vector::Zero(), direction, upSeed(direction));
            Vector minimum = Vector::Constant(std::numeric_limits<double>::max());
            Vector maximum = -minimum;
            for (unsigned i = 0; i < 4; ++i)
            {
                const Vector ray = corners[i + 4] - corners[i];
                for (double t : depth_interval)
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
            minimum.z() -= settings.distance;
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

    RenderResult<bool> ShadowProjectionCache::project(
        const ViewGpuParameters& camera, const ShadowLightSnapshot& lights, bool lights_changed
    ) noexcept
    {
        const double near_z = camera.position_near[3], far_z = camera.direction_far[3];
        Matrix inverse;
        for (unsigned i = 0; i < 16; ++i)
        {
            inverse.data()[i] = camera.inverse_view_projection[i];
        }
        if (inverse.determinant() == 0)
        {
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        }
        std::array<Vector, 8> corners;
        for (unsigned i = 0; i < corners.size(); ++i)
        {
            const Eigen::Vector4d point = inverse * Eigen::Vector4d(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i >> 2, 1);
            if (!point.allFinite() || std::abs(point.w()) < 1e-12)
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            corners[i] = point.head<3>() / point.w();
        }
        const double guard = settings_.pcf_radius + 1;
        const double scale = (settings_.shape.resolution - 2 * guard) / settings_.shape.resolution;
        std::uint32_t first = 0;
        const float cascade_span = std::min(camera.direction_far[3], settings_.distance) - camera.position_near[3];
        bool projected = false;
        const std::array offsets{0u, lights.counts[0], lights.counts[0] + lights.counts[1]};
        for (const auto& light : lights.casters)
        {
            const auto& selection = light.settings;
            const auto kind = static_cast<std::uint32_t>(light.value.index());
            const auto count = shadowLayers(light);
            if (!lights_changed && kind != 0)
            {
                // Local-light faces stay in the same layers and their matrices are camera-independent.
                first += count;
                continue;
            }
            projected |= count != 0;
            if (lights_changed)
            {
                const LightShadowRange range{first, count, kind};
                const auto row = offsets[kind] + light.dense_index;
                std::memcpy(mappings_.data() + sizeof(ShadowMappingHeader) + row * sizeof(range), &range, sizeof(range));
                std::ranges::fill(
                    std::span{raster_states_}.subspan(first, count),
                    RasterState{RasterViewport{}, selection.depth_bias_constant, selection.depth_bias_slope}
                );
            }
            for (std::uint32_t face = 0; face < count; ++face)
            {
                auto& slice = slices_[first + face];
                slice.identity = {first + face, 1, kind != 0, 0};
                const float inset = static_cast<float>(guard / settings_.shape.resolution);
                slice.inner_uv = {inset, inset, 1 - inset, 1 - inset};
                slice.sampling =
                    {selection.normal_bias, selection.bias, static_cast<float>(settings_.pcf_radius), 0};
                Matrix matrix;
                if (kind == 0)
                {
                    const auto depth = [&](std::uint32_t index) noexcept
                    {
                        return camera.position_near[3] + cascade_span * selection.cascade_splits[index];
                    };
                    const double begin = face ? depth(face - 1) : near_z;
                    const double end = depth(face);
                    if (begin < near_z || end > far_z || end <= begin)
                    {
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                    }
                    matrix = directional(
                        xyz(std::get<DirectionalLightGpu>(light.value).direction),
                        corners,
                        {(begin - near_z) / (far_z - near_z), (end - near_z) / (far_z - near_z)},
                        settings_
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
                            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                        }
                        projection_scale *= cosine / std::sqrt(1 - cosine * cosine);
                    }
                    constexpr float near_light = 0.05F;
                    if (near_light >= far_light)
                    {
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                    }
                    matrix = perspective(projection_scale, near_light, far_light) *
                             lightView(position, direction, up);
                    slice.depths = {near_light, static_cast<float>(far_light), 0, 0};
                }
                if (!matrix.allFinite())
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                storeView(matrix, views_[first + face]);
            }
            first += count;
        }
        return projected;
    }

    RenderResult<void> ShadowProjectionCache::select(
        const ViewGpuParameters& camera, ResourceViewValue<ShadowLightSnapshot> selection
    ) noexcept
    {
        if (!selection.revision) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        const auto& lights = selection.value.get();
        const bool lights_changed = applied_light_revision_ != selection.revision;
        const bool header_changed = position_near_ != camera.position_near || direction_far_ != camera.direction_far;
        const bool planes_changed = position_near_[3] != camera.position_near[3] ||
            direction_far_[3] != camera.direction_far[3];
        const bool frustum_changed = planes_changed || inverse_view_projection_ != camera.inverse_view_projection;
        if (!lights_changed && !header_changed && !frustum_changed) return {};
        const bool exhausted = view_revision_ == UINT64_MAX || slice_revision_ == UINT64_MAX ||
            mapping_revision_ == UINT64_MAX;
        if (exhausted) return cxx::unexpected(RenderError{kDeviceCapacity});
        const double near_z = camera.position_near[3], far_z = camera.direction_far[3];
        const bool invalid_camera =
            !std::isfinite(near_z) || !std::isfinite(far_z) || near_z <= 0 || far_z <= near_z ||
            !std::ranges::all_of(camera.inverse_view_projection, [](float v) { return std::isfinite(v); }) ||
            !std::ranges::all_of(camera.position_near, [](float v) { return std::isfinite(v); }) ||
            !std::ranges::all_of(camera.direction_far, [](float v) { return std::isfinite(v); }) ||
            std::abs(xyz(camera.direction_far).squaredNorm() - 1) > 0.001;
        if (invalid_camera)
        {
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        }

        // Projection can fail after writing a cascade. A retry rebuilds every output before publishing revisions.
        applied_light_revision_ = 0;
        const auto counts = lights.counts;
        const auto light_count = std::uint64_t{counts[0]} + counts[1] + counts[2];
        if (lights_changed)
        {
            if (light_count > UINT32_MAX) return cxx::unexpected(RenderError{kStorageCapacity});
            std::uint64_t layers = 0;
            for (const auto& light : lights.casters)
            {
                const auto kind = light.value.index();
                const bool invalid_index = kind >= counts.size() || light.dense_index >= counts[kind];
                const bool invalid_cascades = std::holds_alternative<DirectionalLightGpu>(light.value) &&
                    light.settings.cascade_count > light.settings.cascade_splits.size();
                if (invalid_index || invalid_cascades) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                layers += shadowLayers(light);
            }
            layers = std::max<std::uint64_t>(settings_.shape.view_count, layers);
            if (layers > settings_.maximum_layers) return cxx::unexpected(RenderError{kDeviceCapacity});
            views_.assign(layers, MeshVisibilityView{});
            slices_.assign(layers, ShadowSliceGpu{});
            raster_states_.assign(layers, RasterState{});
            const auto ranges = std::max<std::uint64_t>(1, light_count);
            mappings_.assign(sizeof(ShadowMappingHeader) + ranges * sizeof(LightShadowRange), std::byte{});
        }

        if (lights_changed || frustum_changed)
        {
            auto projected = project(camera, lights, lights_changed);
            if (!projected) return cxx::unexpected(projected.error());
            view_revision_ += lights_changed || *projected;
            slice_revision_ += lights_changed || (*projected && planes_changed);
        }
        if (lights_changed || header_changed)
        {
            const ShadowMappingHeader header{
                {0, counts[0], counts[0] + counts[1], static_cast<std::uint32_t>(light_count)},
                camera.position_near, camera.direction_far
            };
            std::memcpy(mappings_.data(), &header, sizeof(header));
            ++mapping_revision_;
        }
        inverse_view_projection_ = camera.inverse_view_projection;
        position_near_ = camera.position_near;
        direction_far_ = camera.direction_far;
        applied_light_revision_ = selection.revision;
        return {};
    }
} // namespace lux::render::detail
