#include <lux/engine/render/features/ShadowProjectionBinding.hpp>

#include <bit>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/features/FeatureAssembly.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/storage/DirectLightingStorage.hpp>

namespace lux::render::detail
{
    ShadowProjectionBinding::ShadowProjectionBinding(
        DeviceSession& device,
        FrameExecution& frames,
        ResourceRegistry& resources,
        StorageId light_storage,
        const Description& description
    ) noexcept
        : device_(device), frames_(frames), resources_(resources), lights_(light_storage), description_(description),
          selected_lights_(description.config.casters.size()), views_(description.config.shape.view_count),
          slices_(description.config.shape.view_count), raster_states_(description.config.shape.view_count)
    {
        for (std::size_t i = 0; i < updates_.size(); ++i)
        {
            updates_[i].resource = description.outputs[i];
        }
        view_layout_ = {static_cast<std::uint32_t>(views_.size() * sizeof(MeshVisibilityView)), 16};
        for (std::uint32_t i = 0; i < views_.size(); ++i)
        {
            view_layout_.fields.push_back(
                {"view" + std::to_string(i), rdesc::EDataScalar::FLOAT, i * sizeof(MeshVisibilityView), 4, 4, 0, 16}
            );
        }
        updates_[0].input.parameters = std::as_bytes(std::span(views_));
        updates_[0].input.parameter_layout = &view_layout_;
        std::size_t view = 0;
        for (const auto& caster : description.config.casters)
        {
            const auto count = caster.kind == EDirectLightingBuffer::DIRECTIONAL ? caster.cascade_far_distances.size()
                               : caster.kind == EDirectLightingBuffer::POINT     ? 6u
                                                                                 : 1u;
            for (std::size_t i = 0; i < count; ++i)
            {
                raster_states_[view++] = {RasterViewport{}, caster.depth_bias_constant, caster.depth_bias_slope};
            }
        }
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
        std::span<const StorageId> storages,
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
        for (const auto& selection : description.config.casters)
        {
            VDirectLight light;
            std::uint32_t index = 0;
            if (!(*storage)->read(selection.light, light, index) ||
                light.index() != static_cast<std::size_t>(selection.kind))
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
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

    RenderResult<FeatureBinding::Preparation> ShadowProjectionBinding::prepareFrame(
        const FeatureFrameInput& input,
        ResourceUploader& uploads
    ) noexcept
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
        if (camera != camera_ || light_revision_ != (*light_storage)->revision())
        {
            // A rejected input may have changed scratch values. It cannot leave the previous cache key valid.
            light_revision_ = 0;
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
            for (std::size_t i = 0; i < selected_lights_.size(); ++i)
            {
                auto& light = selected_lights_[i];
                const auto& selected = config.casters[i];
                light.active = (*light_storage)->read(selected.light, light.value, light.dense_index);
                if (light.active && light.value.index() != static_cast<std::size_t>(selected.kind))
                {
                    return cxx::unexpected(RenderError{kFeatureContract});
                }
            }
            if (auto projected = projectShadows(config, camera, selected_lights_, views_, slices_, ranges, offsets);
                !projected)
            {
                return cxx::unexpected(projected.error());
            }
            camera_ = camera;
            light_revision_ = (*light_storage)->revision();
            ++revision_;
            for (auto& slice : gpu_)
            {
                slice.uploaded = 0;
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
                    nri::BufferUsageBits::SHADER_RESOURCE_STORAGE
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
        updates_[0].input.parameter_layout = &view_layout_;
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
        slice.pending = slice.uploaded;
        slice.pending_revision = 0;
        cxx::scope_exit acknowledge(
            [&]() noexcept
            {
                frames_.onPreparationSubmitted(
                    &slice,
                    [](void* value, std::uint32_t, RenderResult<void> accepted) noexcept
                    {
                        auto& slice = *static_cast<Slice*>(value);
                        if (accepted)
                        {
                            slice.uploaded = slice.pending;
                            if (slice.pending_revision)
                            {
                                slice.revision = slice.pending_revision;
                                slice.uploaded = 0;
                            }
                        }
                        slice.pending_revision = 0;
                    }
                );
            }
        );
        std::size_t base = 0;
        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            while (slice.pending < base + sources[i].size())
            {
                const auto offset = slice.pending - base;
                auto copied = uploads.stageBuffer({slice.buffers[i], offset, sources[i].subspan(offset)});
                if (!copied)
                {
                    return cxx::unexpected(copied.error());
                }
                if (!*copied)
                {
                    return prepared(FeatureBinding::EPreparation::WAITING);
                }
                slice.pending += *copied;
            }
            base += sources[i].size();
        }
        slice.pending_revision = revision_;
        return prepared(FeatureBinding::EPreparation::READY);
    }
} // namespace lux::render::detail
