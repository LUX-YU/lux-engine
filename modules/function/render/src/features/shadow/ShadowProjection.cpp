#include <lux/engine/render/features/shadow/ShadowProjection.hpp>

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

    RenderResult<void> projectShadows(
        const ShadowProjectionInput& config,
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
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
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
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
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
                            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                        }
                        projection_scale *= cosine / std::sqrt(1 - cosine * cosine);
                    }
                    if (selection.near_distance >= far_light)
                    {
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                    }
                    matrix = perspective(projection_scale, selection.near_distance, far_light) *
                             lightView(position, direction, up);
                    slice.depths = {selection.near_distance, static_cast<float>(far_light), 0, 0};
                }
                if (!matrix.allFinite())
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                storeView(matrix, views[first + face]);
            }
            first += count;
        }
        return {};
    }

    ShadowProjectionData::ShadowProjectionData(ShadowProjectionSettings settings) noexcept
        : settings_(settings), views_(settings.shape.view_count), slices_(settings.shape.view_count),
          raster_states_(settings.shape.view_count)
    {
    }

    RenderResult<bool> ShadowProjectionData::select(const ShadowCamera& camera, const ShadowLightSnapshot& lights) noexcept
    {
        if (lights.revision == 0) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        if (camera == camera_ && applied_light_revision_ == lights.revision) return false;
        if (revision_ == UINT64_MAX) return cxx::unexpected(RenderError{kDeviceCapacity});
        // A failure may overwrite the scratch data. Only successful projections own a cache key.
        applied_light_revision_ = 0;

        const auto counts = lights.counts;
        const auto light_count = std::uint64_t(counts[0]) + counts[1] + counts[2];
        if (light_count > UINT32_MAX) return cxx::unexpected(RenderError{kStorageCapacity});
        const std::array<std::uint32_t, 3> offsets{0, counts[0], counts[0] + counts[1]};
        casters_.resize(lights.casters.size());
        selected_lights_.resize(lights.casters.size());
        const auto near_distance = camera.position_near[3];
        const auto far_distance = std::min(camera.direction_far[3], settings_.distance);
        std::uint64_t layer_count = 0;
        for (std::size_t i = 0; i < lights.casters.size(); ++i)
        {
            const auto& light = lights.casters[i];
            const auto& settings = light.settings;
            const auto kind = light.value.index();
            const bool invalid_index = kind >= counts.size() || light.dense_index >= counts[kind];
            if (invalid_index) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            auto& caster = casters_[i];
            caster.light = light.id;
            caster.kind = static_cast<EDirectLightingBuffer>(kind);
            caster.normal_offset = settings.normal_bias;
            caster.comparison_offset = settings.bias;
            caster.caster_distance = settings_.distance;
            caster.depth_bias_constant = settings.depth_bias_constant;
            caster.depth_bias_slope = settings.depth_bias_slope;
            caster.cascade_far_distances.clear();
            if (caster.kind == EDirectLightingBuffer::DIRECTIONAL)
            {
                if (settings.cascade_count > settings.cascade_splits.size())
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                for (auto split : std::span(settings.cascade_splits).first(settings.cascade_count))
                {
                    caster.cascade_far_distances.push_back(near_distance + (far_distance - near_distance) * split);
                }
            }
            selected_lights_[i] = {light.value, light.dense_index, true};
            layer_count += caster.kind == EDirectLightingBuffer::DIRECTIONAL ? settings.cascade_count :
                caster.kind == EDirectLightingBuffer::POINT ? 6 : 1;
        }
        layer_count = std::max<std::uint64_t>(settings_.shape.view_count, layer_count);
        if (layer_count > settings_.maximum_layers) return cxx::unexpected(RenderError{kDeviceCapacity});

        views_.resize(layer_count);
        slices_.resize(layer_count);
        raster_states_.assign(layer_count, RasterState{});
        std::size_t first = 0;
        for (const auto& caster : casters_)
        {
            const auto count = caster.kind == EDirectLightingBuffer::DIRECTIONAL ? caster.cascade_far_distances.size() :
                caster.kind == EDirectLightingBuffer::POINT ? 6u : 1u;
            std::ranges::fill(
                std::span{raster_states_}.subspan(first, count),
                RasterState{RasterViewport{}, caster.depth_bias_constant, caster.depth_bias_slope}
            );
            first += count;
        }

        ranges_.resize(std::max<std::uint64_t>(1, light_count));
        const ShadowProjectionInput projection{
            {settings_.shape.resolution, static_cast<std::uint32_t>(layer_count)}, casters_, settings_.pcf_radius
        };
        auto projected = projectShadows(projection, camera, selected_lights_, views_, slices_, ranges_, offsets);
        if (!projected) return cxx::unexpected(projected.error());
        const ShadowMappingHeader header{
            {offsets[0], offsets[1], offsets[2], static_cast<std::uint32_t>(light_count)},
            camera.position_near,
            camera.direction_far
        };
        const auto ranges = std::as_bytes(std::span{ranges_});
        mappings_.resize(sizeof(header) + ranges.size());
        std::memcpy(mappings_.data(), &header, sizeof(header));
        std::memcpy(mappings_.data() + sizeof(header), ranges.data(), ranges.size());

        camera_ = camera;
        applied_light_revision_ = lights.revision;
        ++revision_;
        return true;
    }
} // namespace lux::render::detail
