#include <lux/engine/render/extensions/upload/Preparation.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/features/shadow/ShadowProjectionBinding.hpp>

#include <bit>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>
#include <lux/engine/render/resource/lighting/DirectLightingStorage.hpp>
#include <lux/engine/render/resource/view/CameraParameters.hpp>

namespace lux::render::detail
{
    ShadowProjectionBinding::ShadowProjectionBinding(
        DeviceSession& device, RenderGraphInstance& instance, ResourceRegistry& resources,
        ResourceInstanceId light_storage, const Description& description
    ) noexcept
        : device_(device), instance_(instance), resources_(resources), lights_(light_storage),
          description_(description), views_(description.config.shape.view_count),
          slices_(description.config.shape.view_count), raster_states_(description.config.shape.view_count)
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

    RenderResult<std::unique_ptr<InstanceData>> ShadowProjectionBinding::create(
        DeviceSession& device,
        ResourceRegistry& resources,
        InstanceAccess& access,
        std::span<const ResourceInstanceId> storages,
        const void* value
    ) noexcept
    {
        auto& instance = static_cast<RenderGraphInstance&>(access);
        const auto& description = *static_cast<const Description*>(value);
        const auto light_storage = storages[description.lights.index];
        auto storage = resources.borrow<DirectLightingStorage>(light_storage);
        if (!storage)
        {
            return cxx::unexpected(storage.error());
        }
        auto binding = new ShadowProjectionBinding(device, instance, resources, light_storage, description);
        for (std::uint32_t slot = 0; slot < instance.frameCount(); ++slot)
            for (const auto& update : binding->updates_)
                instance.setResource(update.resource.index, slot, update.input);
        return std::unique_ptr<InstanceData>(binding);
    }

    RenderResult<bool>
    ShadowProjectionBinding::prepare(
        const InstanceInputs& input,
        ResourceUploader& uploads,
        detail::Preparation& preparation
    ) noexcept
    {
        const auto& config = description_.config;
        if (input.parameters.size() != sizeof(CameraParameters))
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        CameraParameters view;
        std::memcpy(&view, input.parameters.data(), sizeof(view));
        const ShadowCamera camera{view.inverse_view_projection, view.position_near, view.direction_far};
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
        const auto prepared = [&]() noexcept
        {
            if (std::exchange(slice.binding_changed, false))
                for (const auto& update : updates_)
                {
                    instance_.setResource(update.resource.index, input.slot, update.input);
                }
            return true;
        };
        if (slice.revision == revision_)
        {
            return prepared();
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
                preparation.onSubmitted(
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
        return prepared();
    }
} // namespace lux::render::detail
