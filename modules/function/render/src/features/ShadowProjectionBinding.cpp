#include <lux/engine/render/features/ShadowProjectionBinding.hpp>

#include <bit>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>
#include <lux/engine/render/resource/lighting/DirectLightingStorage.hpp>

namespace lux::render::detail
{
    ShadowProjectionBinding::ShadowProjectionBinding(
        DeviceSession& device,
        FrameExecution& frames,
        ResourceRegistry& resources,
        ResourceInstanceId light_storage,
        const Description& description
    ) noexcept
        : device_(device), frames_(frames), resources_(resources), lights_(light_storage), description_(description),
          views_(description.config.shape.view_count), slices_(description.config.shape.view_count),
          raster_states_(description.config.shape.view_count)
    {
        for (std::size_t i = 0; i < updates_.size(); ++i)
        {
            updates_[i].resource = description.outputs[i];
        }
        updates_[0].input.parameters = std::as_bytes(std::span(views_));
        updates_[3].input.parameters = std::as_bytes(std::span(raster_states_));
    }

    ShadowProjectionBinding::~ShadowProjectionBinding() noexcept
    {
        for (auto& frame : gpu_)
        {
            for (auto*& buffer : frame.buffers)
            {
                releaseAfterGpu(device_, buffer);
            }
        }
    }

    RenderResult<FeatureBinding> ShadowProjectionBinding::create(
        DeviceSession& device,
        FrameExecution& frames,
        ResourceRegistry& resources,
        RenderGraphInstance&,
        CompiledPrograms&,
        std::span<const ResourceInstanceId> storages,
        const void* value
    ) noexcept
    {
        const auto& description = *static_cast<const Description*>(value);
        const auto light_storage = storages[description.lights.index];
        auto storage = resources.borrow<DirectLightingStorage>(light_storage);
        if (!storage)
        {
            return cxx::unexpected(storage.error());
        }
        auto binding = new ShadowProjectionBinding(device, frames, resources, light_storage, description);
        FeatureBinding result{
            {binding, [](void* pointer) noexcept { delete static_cast<ShadowProjectionBinding*>(pointer); }},
            [](void* pointer, const FeatureFrameInput& input, ResourceUploader& uploads) noexcept
            { return static_cast<ShadowProjectionBinding*>(pointer)->prepareFrame(input, uploads); }
        };
        result.initial_resources = binding->updates_;
        return result;
    }

    RenderResult<FeatureBinding::Preparation>
    ShadowProjectionBinding::prepareFrame(const FeatureFrameInput& input, ResourceUploader& uploads) noexcept
    {
        const auto& config = description_.config;
        const bool invalid_parameters = config.camera_offset > input.parameters.size() ||
                                        sizeof(ShadowCamera) > input.parameters.size() - config.camera_offset;
        if (invalid_parameters)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        ShadowCamera camera;
        std::memcpy(&camera, input.parameters.data() + config.camera_offset, sizeof(camera));
        auto light_storage = resources_.borrow<DirectLightingStorage>(lights_);
        if (!light_storage)
        {
            return cxx::unexpected(light_storage.error());
        }
        if (camera != camera_ || applied_shadow_revision_ != (*light_storage)->shadowRevision())
        {
            // A rejected input may have changed scratch values. It cannot leave the previous cache key valid.
            applied_shadow_revision_ = 0;
            const auto counts = (*light_storage)->counts();
            const std::array<std::uint32_t, 3> offsets{0, counts[0], counts[0] + counts[1]};
            const auto count = std::size_t(counts[0]) + counts[1] + counts[2];
            mappings_.resize(sizeof(ShadowMappingHeader) + std::max<std::size_t>(1, count) * sizeof(LightShadowRange));
            const ShadowMappingHeader header{
                {offsets[0], offsets[1], offsets[2], static_cast<std::uint32_t>(count)},
                camera.position_near,
                camera.direction_far
            };
            std::memcpy(mappings_.data(), &header, sizeof(header));
            auto ranges = std::span(
                reinterpret_cast<LightShadowRange*>(mappings_.data() + sizeof(header)),
                std::max<std::size_t>(1, count)
            );
            casters_.clear();
            const auto near_distance = camera.position_near[3];
            const auto far_distance = std::min(camera.direction_far[3], config.distance);
            std::uint64_t view_count = 0;
            (*light_storage)
                ->forEachShadow(
                    [&](LightId id, std::uint32_t kind, const LightShadowSettings& settings)
                    {
                        ShadowLightSelection selected;
                        selected.light = id;
                        selected.kind = static_cast<EDirectLightingBuffer>(kind);
                        selected.normal_offset = settings.normal_bias;
                        selected.comparison_offset = settings.bias;
                        selected.caster_distance = config.distance;
                        selected.depth_bias_constant = settings.depth_bias_constant;
                        selected.depth_bias_slope = settings.depth_bias_slope;
                        selected.cascade_far_distances.clear();
                        if (kind == 0)
                        {
                            for (auto split : std::span(settings.cascade_splits).first(settings.cascade_count))
                            {
                                selected.cascade_far_distances.push_back(
                                    near_distance + (far_distance - near_distance) * split
                                );
                            }
                        }
                        view_count += kind == 0 ? settings.cascade_count : kind == 1 ? 6 : 1;
                        casters_.push_back(std::move(selected));
                    }
                );
            if (view_count > device_.description().dimensions.textureLayerMaxNum)
            {
                return cxx::unexpected(RenderError{kDeviceCapacity});
            }
            const auto layers = std::max<std::uint32_t>(config.shape.view_count, static_cast<std::uint32_t>(view_count));
            views_.resize(layers);
            slices_.resize(layers);
            raster_states_.assign(layers, RasterState{});
            std::size_t raster_view = 0;
            for (const auto& caster : casters_)
            {
                const auto count = caster.kind == EDirectLightingBuffer::DIRECTIONAL
                                       ? caster.cascade_far_distances.size()
                                   : caster.kind == EDirectLightingBuffer::POINT ? 6u
                                                                                 : 1u;
                for (std::size_t view = 0; view < count; ++view)
                {
                    raster_states_[raster_view++] =
                        {RasterViewport{}, caster.depth_bias_constant, caster.depth_bias_slope};
                }
            }
            selected_lights_.resize(casters_.size());
            for (std::size_t i = 0; i < casters_.size(); ++i)
            {
                auto& light = selected_lights_[i];
                light.active = (*light_storage)->read(casters_[i].light, light.value, light.dense_index);
            }
            const ShadowProjectionInput projection{{config.shape.resolution, layers}, casters_, config.pcf_radius};
            if (auto projected = projectShadows(projection, camera, selected_lights_, views_, slices_, ranges, offsets);
                !projected)
            {
                return cxx::unexpected(projected.error());
            }
            updates_[3].input.parameters = std::as_bytes(std::span(raster_states_));
            camera_ = camera;
            applied_shadow_revision_ = (*light_storage)->shadowRevision();
            ++revision_;
            for (auto& slice : gpu_)
            {
                slice.binding_changed = true;
            }
        }
        auto& slice = gpu_[input.slot];
        const std::array<std::span<const std::byte>, 3> sources{
            std::as_bytes(std::span(views_)),
            std::as_bytes(std::span(slices_)),
            mappings_
        };
        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            auto*& buffer = slice.buffers[i];
            if (!buffer || device_.nriCore().GetBufferDesc(*buffer).size < sources[i].size())
            {
                nri::Buffer* candidate = nullptr;
                const nri::BufferDesc description{
                    std::bit_ceil(sources[i].size()),
                    4,
                    (nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE)
                };
                const auto status =
                    device_.nriCore().CreatePlacedBuffer(device_.device(), NriDeviceHeap, description, candidate);
                if (status != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(status));
                }
                releaseAfterGpu(device_, buffer);
                buffer = candidate;
                slice.binding_changed = true;
            }
            updates_[i].input = {buffer, 0, sources[i].size(), sources[i]};
        }
        updates_[0].input.parameter_revision = revision_;
        updates_[3].input.parameter_revision = revision_;
        const auto prepared = [&](FeatureBinding::EPreparation state) noexcept
        {
            const auto updates = std::exchange(slice.binding_changed, false) ? std::span<const ResourceUpdate>(updates_)
                                                                             : std::span<const ResourceUpdate>{};
            return FeatureBinding::Preparation{state, updates};
        };
        if (slice.revision == revision_)
        {
            return prepared(FeatureBinding::EPreparation::READY);
        }
        slice.pending_revision = 0;
        bool staged_any = false;
        cxx::scope_exit acknowledge(
            [&]() noexcept
            {
                if (!staged_any)
                {
                    return;
                }
                uploads.onPreparationSubmitted(
                    &slice,
                    [](void* value, std::uint32_t, RenderResult<void> accepted) noexcept
                    {
                        auto& slice = *static_cast<Slice*>(value);
                        if (accepted)
                        {
                            if (slice.pending_revision)
                            {
                                slice.revision = slice.pending_revision;
                            }
                        }
                        slice.pending_revision = 0;
                    }
                );
            }
        );
        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            if (sources[i].empty())
            {
                continue;
            }
            auto copied = uploads.stageBuffer({slice.buffers[i], 0, sources[i]});
            if (!copied)
            {
                return cxx::unexpected(copied.error());
            }
            staged_any = true;
        }
        slice.pending_revision = revision_;
        return prepared(FeatureBinding::EPreparation::READY);
    }
} // namespace lux::render::detail
