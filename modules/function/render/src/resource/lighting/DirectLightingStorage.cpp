#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <lux/engine/render/runtime/commands/LightingCommands.hpp>
#include <lux/engine/render/resource/lighting/DirectLightingStorage.hpp>

#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <bit>
#include <cmath>
#include <cstring>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/graph/GraphResource.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>

namespace lux::render::detail
{
    namespace
    {
        void createLights(
            DirectLightingStorage& owner, const LightingCommands::CreateLights::Request& input,
            LightingCommands::CreateLights::Response& reply
        ) noexcept
        {
            reply.sequence = input.data.sequence;
            auto result = owner.create(input.data);
            reply.error = result ? RenderError{} : result.error();
            if (result) reply.lights = std::move(*result);
        }

        void removeLight(
            DirectLightingStorage& owner, const LightingCommands::ReleaseLight::Request& input,
            LightingCommands::ReleaseLight::Response& reply
        ) noexcept
        {
            reply.sequence = input.sequence;
            auto result = owner.remove(input.light);
            reply.error = result ? RenderError{} : result.error();
        }

        RenderResult<void>
        updateLights(DirectLightingStorage& resource, std::span<const DirectLightUpdate> input) noexcept
        {
            auto result = resource.update(input);
            return result ? RenderResult<void>{} : cxx::unexpected(result.error());
        }
        void updateLightsCommand(
            DirectLightingStorage& resource,
            const LightingCommands::Update::Request& input,
            AckResponse& response
        ) noexcept
        {
            const auto result = updateLights(resource, input.values);
            response.error = result ? RenderError{} : result.error();
        }

    } // namespace

    namespace
    {
        constexpr std::array<std::size_t, 4> strides{32, 48, 64, 16};

        std::span<const std::byte> bytes(const VDirectLight& value) noexcept
        {
            return std::visit([](const auto& record) { return std::as_bytes(std::span(&record, 1)); }, value);
        }

        bool validShadow(const LightShadowSettings& shadow, std::size_t kind) noexcept
        {
            if (!shadow.enabled)
            {
                return true;
            }
            const bool invalid_bias = !std::isfinite(shadow.bias) || shadow.bias < 0 ||
                                      !std::isfinite(shadow.normal_bias) || shadow.normal_bias < 0 ||
                                      !std::isfinite(shadow.depth_bias_constant) ||
                                      !std::isfinite(shadow.depth_bias_slope);
            if (invalid_bias)
            {
                return false;
            }
            if (kind == 0)
            {
                if (shadow.cascade_count == 0 || shadow.cascade_count > shadow.cascade_splits.size())
                {
                    return false;
                }
                float previous = 0;
                for (auto split : std::span(shadow.cascade_splits).first(shadow.cascade_count))
                {
                    if (!std::isfinite(split) || split <= previous || split > 1)
                    {
                        return false;
                    }
                    previous = split;
                }
            }
            return true;
        }

        bool validLight(const VDirectLight& value) noexcept
        {
            return std::visit(
                [](const auto& light) noexcept
                {
                    const bool valid_radiance =
                        std::ranges::all_of(light.radiance, [](float v) { return std::isfinite(v) && v >= 0; });
                    if (!valid_radiance)
                    {
                        return false;
                    }
                    const auto direction = [](const auto& d) noexcept
                    {
                        const auto square = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                        return std::isfinite(square) && std::abs(square - 1.0F) < 0.001F;
                    };
                    if constexpr (std::same_as<std::decay_t<decltype(light)>, DirectionalLightGpu>)
                    {
                        return direction(light.direction);
                    }
                    else
                    {
                        const bool valid_position =
                            std::ranges::all_of(light.position_range, [](float v) { return std::isfinite(v); }) &&
                            light.position_range[3] > 0;
                        const auto& attenuation = [&]() -> const std::array<float, 4>&
                        {
                            if constexpr (std::same_as<std::decay_t<decltype(light)>, PointLightGpu>)
                            {
                                return light.attenuation;
                            }
                            else
                            {
                                return light.attenuation_inner;
                            }
                        }();
                        const bool valid_attenuation =
                            std::ranges::all_of(attenuation, [](float v) { return std::isfinite(v); }) &&
                            attenuation[0] > 0 && attenuation[1] >= 0 && attenuation[2] >= 0;
                        if constexpr (std::same_as<std::decay_t<decltype(light)>, PointLightGpu>)
                        {
                            return valid_position && valid_attenuation;
                        }
                        else
                        {
                            return valid_position && valid_attenuation && direction(light.direction_outer) &&
                                   light.direction_outer[3] >= -1 && attenuation[3] <= 1 &&
                                   attenuation[3] > light.direction_outer[3];
                        }
                    }
                },
                value
            );
        }
    } // namespace

    DirectLightingStorage::DirectLightingStorage(ResourceCreateContext& dependencies, ResourceInstanceId id) noexcept
        : device_(dependencies.device), resources_(dependencies.resources),
          frame_count_(dependencies.frame_count), id_(id)
    {
        for (std::size_t kind = 0; kind < arrays_.size(); ++kind)
        {
            // One initialized element makes empty arrays valid descriptor inputs; the separate counts remain zero.
            arrays_[kind].records.resize(strides[kind]);
            for (auto& frame : arrays_[kind].frames)
            {
                frame.dirty.grow(1);
                frame.dirty.mark(0);
            }
        }
    }

    DirectLightingStorage::~DirectLightingStorage() noexcept
    {
        for (auto& array : arrays_)
        {
            for (auto& frame : array.frames)
            {
                releaseAfterGpu(device_, frame.buffer);
                releaseAfterGpu(device_, frame.candidate);
            }
        }
    }

    RenderResult<std::vector<LightId>> DirectLightingStorage::create(const LightData& input) noexcept
    {
        if (!input.shadows.empty() && input.shadows.size() != input.values.size())
            return cxx::unexpected(RenderError{kStorageContract});
        if (input.values.size() > UINT32_MAX - entries_.size()) return cxx::unexpected(RenderError{kStorageCapacity});
        if (input.values.empty()) return std::vector<LightId>{};

        auto required = counts();
        for (std::size_t i = 0; i < input.values.size(); ++i)
        {
            const auto& light = input.values[i];
            if (!validLight(light)) return cxx::unexpected(RenderError{kStorageContract});
            const auto kind = light.index();
            if (!input.shadows.empty() && !validShadow(input.shadows[i], kind))
                return cxx::unexpected(RenderError{kStorageContract});
            ++required[kind];
        }
        const auto limit = device_.description().memory.storageBufferMaxRange;
        for (std::size_t k = 0; k < required.size(); ++k)
            if (std::uint64_t{required[k]} * strides[k] > limit) return cxx::unexpected(RenderError{kStorageCapacity});

        for (std::size_t k = 0; k < required.size(); ++k)
        {
            auto& array = arrays_[k];
            const auto count = required[k];
            const auto first = array.identities.size();
            if (count == first) continue;
            array.records.resize(std::max(array.records.size(), std::size_t{count} * strides[k]));
            for (std::uint32_t f = 0; f < frame_count_; ++f)
            {
                auto& dirty = array.frames[f].dirty;
                dirty.grow(count);
                dirty.markRange(first, count - first);
            }
        }
        std::vector<LightId> result;
        result.reserve(input.values.size());
        for (std::size_t i = 0; i < input.values.size(); ++i)
        {
            const auto& light = input.values[i];
            const auto kind = static_cast<std::uint32_t>(light.index());
            auto& array = arrays_[kind];
            const auto position = static_cast<std::uint32_t>(array.identities.size());
            const auto key = entries_.emplace(Entry{kind, position, input.shadows.empty() ? LightShadowSettings{} : input.shadows[i]});
            const auto source = bytes(light);
            std::memcpy(array.records.data() + std::size_t{position} * strides[kind], source.data(), source.size());
            array.identities.push_back(key);
            result.push_back(extension_detail::ResourceIdentity::make<LightId>(id_, key));
        }
        updateCounts();
        return result;
    }

    RenderResult<std::uint32_t> DirectLightingStorage::update(std::span<const DirectLightUpdate> input) noexcept
    {
        if (input.size() > UINT32_MAX)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        for (const auto& value : input)
        {
            const auto* entry = extension_detail::ResourceIdentity::parent(value.light) == id_
                ? entries_.find(extension_detail::ResourceIdentity::local(value.light)) : nullptr;
            if (!entry)
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            const bool invalid_light = entry->kind != value.value.index() || !validLight(value.value);
            const bool invalid_shadow = !validShadow(value.shadows, entry->kind);
            if (invalid_light || invalid_shadow)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
        }
        bool shadow_changed = false;
        for (const auto& value : input)
        {
            auto* entry = entries_.find(extension_detail::ResourceIdentity::local(value.light));
            const bool active_shadow = entry->shadows.enabled || value.shadows.enabled;
            if (active_shadow)
            {
                shadow_changed |= entry->shadows != value.shadows;
                const auto* previous = arrays_[entry->kind].records.data() + entry->position * strides[entry->kind];
                shadow_changed |= std::visit([&](const auto& light) noexcept
                {
                    using Light = std::decay_t<decltype(light)>;
                    Light before;
                    std::memcpy(&before, previous, sizeof(before));
                    if constexpr (std::same_as<Light, DirectionalLightGpu>)
                    {
                        return before.direction != light.direction;
                    }
                    else if constexpr (std::same_as<Light, PointLightGpu>)
                    {
                        return before.position_range != light.position_range;
                    }
                    else
                    {
                        return before.position_range != light.position_range ||
                            before.direction_outer != light.direction_outer;
                    }
                }, value.value);
            }
            auto& array = arrays_[entry->kind];
            auto* destination = array.records.data() + std::size_t(entry->position) * strides[entry->kind];
            const auto source = bytes(value.value);
            if (std::memcmp(destination, source.data(), source.size()) != 0)
            {
                std::memcpy(destination, source.data(), source.size());
                for (std::uint32_t frame = 0; frame < frame_count_; ++frame)
                {
                    array.frames[frame].dirty.mark(entry->position);
                }
            }
            entry->shadows = value.shadows;
        }
        if (shadow_changed && ++shadow_revision_ == 0) std::terminate();
        return static_cast<std::uint32_t>(input.size());
    }

    RenderResult<void> DirectLightingStorage::remove(LightId light) noexcept
    {
        const auto key = extension_detail::ResourceIdentity::local(light);
        const auto* entry = extension_detail::ResourceIdentity::parent(light) == id_ ? entries_.find(key) : nullptr;
        if (!entry) return cxx::unexpected(RenderError{kStorageIdentity});
        auto& array = arrays_[entry->kind];
        const auto last = array.identities.size() - 1;
        const auto position = entry->position;
        if (position != last)
        {
            const auto stride = strides[entry->kind];
            std::memcpy(array.records.data() + position * stride, array.records.data() + last * stride, stride);
            array.identities[position] = array.identities.back();
            entries_.find(array.identities.back())->position = position;
            for (std::uint32_t f = 0; f < frame_count_; ++f) array.frames[f].dirty.mark(position);
        }
        array.identities.pop_back();
        entries_.erase(key);
        updateCounts();
        return {};
    }

    void DirectLightingStorage::updateCounts() noexcept
    {
        const auto sizes = counts();
        const std::array<std::uint32_t, 4> header{sizes[0], sizes[1], sizes[2], 0};
        auto& array = arrays_[3];
        std::memcpy(array.records.data(), header.data(), sizeof(header));
        for (std::uint32_t f = 0; f < frame_count_; ++f) array.frames[f].dirty.mark(0);
        if (++shadow_revision_ == 0) std::terminate();
    }

    std::array<std::uint32_t, 3> DirectLightingStorage::counts() const noexcept
    {
        return {
            static_cast<std::uint32_t>(arrays_[0].identities.size()),
            static_cast<std::uint32_t>(arrays_[1].identities.size()),
            static_cast<std::uint32_t>(arrays_[2].identities.size())
        };
    }

    RenderResult<ResourceViewValue<ShadowLightSnapshot>> DirectLightingStorage::selectShadows() noexcept
    {
        auto& snapshot = shadow_selection_.value;
        if (shadow_selection_.revision == shadow_revision_) return ResourceViewValue<ShadowLightSnapshot>{snapshot, shadow_revision_};

        snapshot.counts = counts();
        snapshot.casters.clear();
        const auto capture = [&]<class Light>(EDirectLightingBuffer projection) noexcept
        {
            const auto& array = arrays_[static_cast<std::size_t>(projection)];
            for (const auto key : array.identities)
            {
                const auto& entry = *entries_.find(key);
                if (!entry.shadows.enabled) continue;
                Light light;
                const auto offset = std::size_t(entry.position) * sizeof(Light);
                std::memcpy(&light, array.records.data() + offset, sizeof(Light));
                // The shadow revision tracks spatial properties only.
                light.radiance = Light{}.radiance;
                if constexpr (std::same_as<Light, PointLightGpu>) light.attenuation = Light{}.attenuation;
                if constexpr (std::same_as<Light, SpotLightGpu>) light.attenuation_inner = Light{}.attenuation_inner;
                snapshot.casters.push_back({
                    extension_detail::ResourceIdentity::make<LightId>(id_, key), light, entry.shadows, entry.position
                });
            }
        };
        capture.template operator()<DirectionalLightGpu>(EDirectLightingBuffer::DIRECTIONAL);
        capture.template operator()<PointLightGpu>(EDirectLightingBuffer::POINT);
        capture.template operator()<SpotLightGpu>(EDirectLightingBuffer::SPOT);
        shadow_selection_.revision = shadow_revision_;
        return ResourceViewValue<ShadowLightSnapshot>{snapshot, shadow_revision_};
    }

    void DirectLightingStorage::publish(std::uint32_t kind, std::uint32_t frame) noexcept
    {
        const auto& slice = arrays_[kind].frames[frame];
        if (!resources_.publishGraphInput(id_, kind, frame, {slice.buffer, 0, slice.capacity * strides[kind]}))
            std::terminate();
    }

    RenderResult<void> DirectLightingStorage::resolve(
        const VGraphResourceDesc& resource,
        DirectLightingBinding binding,
        std::span<ResourceInput> output
    ) noexcept
    {
        const auto field = static_cast<std::size_t>(binding.field);
        const auto* buffer = std::get_if<GraphBufferDesc>(&resource);
        const bool is_invalid_binding = field >= arrays_.size() || !buffer || buffer->size != strides[field];
        if (is_invalid_binding)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        if (output.size() > frame_count_) return cxx::unexpected(RenderError{kDeviceArgument});
        auto& array = arrays_[field];
        for (std::uint32_t f = 0; f < output.size(); ++f)
        {
            const auto& slice = array.frames[f];
            output[f] = {slice.buffer, 0, slice.buffer ? slice.capacity * strides[field] : strides[field]};
        }
        return {};
    }

    RenderResult<bool>
    DirectLightingStorage::prepareFrame(
        std::uint32_t frame,
        ResourceUploader& uploads
    ) noexcept
    {
        if (frame >= frame_count_) return cxx::unexpected(RenderError{kDeviceArgument});
        bool ready = true;
        for (std::uint32_t k = 0; k < arrays_.size(); ++k)
        {
            auto& array = arrays_[k];
            auto& slice = array.frames[frame];
            if (slice.dirty.empty() || !resources_.hasGraphInputUsers(id_, k))
            {
                continue;
            }
            const auto records = array.records.size() / strides[k];
            if (std::max(slice.capacity, slice.candidate_capacity) < records)
            {
                const auto limit = device_.description().memory.storageBufferMaxRange / strides[k];
                const auto capacity = std::min<std::uint64_t>(std::bit_ceil(records), limit);
                nri::Buffer* candidate = nullptr;
                const nri::BufferDesc description{
                    capacity * strides[k],
                    4,
                    (nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE)
                };
                const auto status =
                    device_.nriCore().CreatePlacedBuffer(device_.device(), NriDeviceHeap, description, candidate);
                if (status != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(status));
                }
                releaseAfterGpu(device_, slice.candidate);
                slice.candidate = candidate;
                slice.candidate_capacity = capacity;
                slice.dirty.markRange(0, records);
            }
            slice.accepted.clear();
            slice.dirty.select(
                uploads.windowByteLimit() / strides[k] / 64 + uploads.windowRegionLimit() + 1,
                slice.selected
            );
            RenderResult<bool> copied = true;
            DirtyRanges::visitRanges(
                slice.selected,
                UINT32_MAX,
                [&](std::uint32_t first, std::uint32_t count) noexcept
                {
                    auto source = std::span<const std::byte>(array.records)
                                      .subspan(std::size_t(first) * strides[k], std::size_t(count) * strides[k]);
                    auto result = uploads.stageBufferPrefix(
                        {slice.candidate ? slice.candidate : slice.buffer, std::size_t(first) * strides[k], source},
                        strides[k]
                    );
                    if (!result)
                    {
                        copied = cxx::unexpected(result.error());
                        return false;
                    }
                    const auto accepted = static_cast<std::uint32_t>(*result / strides[k]);
                    slice.dirty.detachRange(slice.accepted, first, accepted);
                    return accepted == count;
                }
            );
            if (!copied)
            {
                return cxx::unexpected(copied.error());
            }
            ready &= slice.dirty.empty() && !slice.candidate;
        }
        return ready;
    }

    void DirectLightingStorage::finishPreparation(const ReplicaSettlement& settlement) noexcept
    {
        if (!settlement.copies.count) return;
        const auto frame = settlement.frame.slot;
        const bool accepted = !settlement.error.type;
        for (std::uint32_t k = 0; k < arrays_.size(); ++k)
        {
            auto& slice = arrays_[k].frames[frame];
            slice.dirty.finishSelection(slice.accepted, bool(accepted));
            if (accepted && slice.dirty.empty() && slice.candidate)
            {
                releaseAfterGpu(device_, slice.buffer);
                slice.buffer = std::exchange(slice.candidate, nullptr);
                slice.capacity = std::exchange(slice.candidate_capacity, 0);
                publish(k, frame);
            }
        }
    }

    std::span<const CommandBinding> directLightingStorageCommands() noexcept
    {
        static const std::array commands{
            CommandBinding::declare<LightingCommands::CreateLights, &createLights>(),
            CommandBinding::declare<LightingCommands::ReleaseLight, &removeLight>(),
            CommandBinding::declare<LightingCommands::Update, &updateLightsCommand>()
        };
        return commands;
    }

    ResourceRegistration DirectLightingStorage::factory() noexcept
    {

        static constexpr auto cpu_view =
            ResourceViewOps::make<DirectLightingStorage, ShadowLightSnapshot, &DirectLightingStorage::selectShadows>();
        static const StorageBindingOps binding{
            .prepareFrame = [](void* value, ReplicaFrame frame, ResourceUploader& uploads) noexcept
            { return static_cast<DirectLightingStorage*>(value)->prepareFrame(frame.slot, uploads); },
            .describeGraphImport = [](const void*, std::uint32_t projection) noexcept -> RenderResult<ResourceProjection>
            {
                if (projection >= strides.size()) return cxx::unexpected(RenderError{kStorageContract});
                const auto bytes = strides[projection];
                return ResourceProjection{GraphBufferDesc{bytes}, {GraphBufferRange{0, bytes}}, EGraphUse::STORAGE_READ};
            },
            .resolveGraphImport = [](void* value, std::uint32_t projection, const VGraphResourceDesc& resource,
               std::span<ResourceInput> output) noexcept -> RenderResult<void>
            {
                if (projection >= strides.size()) return cxx::unexpected(RenderError{kStorageContract});
                return static_cast<DirectLightingStorage*>(value)->resolve(
                    resource, DirectLightingBinding{static_cast<EDirectLightingBuffer>(projection)}, output
                );
            },
            .cpu_view = cpu_view,
            .settleFrame = [](void* value, const ReplicaSettlement& settlement) noexcept
            { static_cast<DirectLightingStorage*>(value)->finishPreparation(settlement); }
        };
        using Created = RenderResult<std::unique_ptr<DirectLightingStorage>>;
        static constexpr auto create = +[](ResourceCreateContext& dependencies, ResourceInstanceId id) noexcept -> Created
        {
            const bool invalid_count = dependencies.frame_count == 0 || dependencies.frame_count > 3;
            if (invalid_count || !dependencies.uploads)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            return std::make_unique<DirectLightingStorage>(dependencies, id);
        };
        return makeResourceRegistration<LightResource, DirectLightingStorage, create, &updateLights>(&binding);
    }
} // namespace lux::render::detail
