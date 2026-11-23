#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <lux/engine/render/runtime/commands/LightingCommands.hpp>
#include <lux/engine/render/resource/ReplicaMutation.hpp>
#include <lux/engine/render/resource/FrameDataCall.hpp>
#include <lux/engine/render/resource/lighting/DirectLightingStorage.hpp>

#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <bit>
#include <cmath>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/graph/GraphResource.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>

namespace lux::render::detail
{
    namespace
    {
        exec::task<LightingCommands::CreateLights::Response>
        createLights(
            DirectLightingStorage& resource,
            ReplicaMutation& mutation,
            LightingCommands::CreateLights::Request&& input
        ) noexcept
        {
            const auto sequence = input.data.sequence;
            auto result = co_await resource.create(mutation, std::move(input.data));
            co_return result ? LightingCommands::CreateLights::Response{{}, sequence, std::move(*result)}
                             : LightingCommands::CreateLights::Response{result.error(), sequence};
        }

        exec::task<LightingCommands::ReleaseLight::Response>
        removeLight(
            DirectLightingStorage& resource,
            ReplicaMutation& mutation,
            LightingCommands::ReleaseLight::Request&& input
        ) noexcept
        {
            auto result = co_await resource.remove(mutation, input.light);
            co_return LightingCommands::ReleaseLight::Response{
                result ? RenderError{} : result.error(), input.sequence
            };
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

    exec::task<RenderResult<std::vector<LightId>>>
    DirectLightingStorage::create(ReplicaMutation& mutation, LightData input) noexcept
    {
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kDeviceBusy});
        }
        updating_ = true;
        ++preparation_revision_;
        cxx::scope_exit finish([&]() noexcept { updating_ = false; });
        if (!input.shadows.empty() && input.shadows.size() != input.values.size())
        {
            co_return cxx::unexpected(RenderError{kStorageContract});
        }
        for (std::size_t i = 0; i < input.shadows.size(); ++i)
        {
            if (!validShadow(input.shadows[i], input.values[i].index()))
            {
                co_return cxx::unexpected(RenderError{kStorageContract});
            }
        }
        if (input.values.size() > UINT32_MAX - entries_.size())
        {
            co_return cxx::unexpected(RenderError{kStorageCapacity});
        }
        std::array<std::uint32_t, 3> counts;
        for (std::size_t k = 0; k < counts.size(); ++k)
        {
            counts[k] = static_cast<std::uint32_t>(arrays_[k].identities.size());
        }
        std::vector<Write> writes;
        writes.reserve(input.values.size());
        for (const auto& value : input.values)
        {
            if (!validLight(value))
            {
                co_return cxx::unexpected(RenderError{kStorageContract});
            }
            const auto kind = static_cast<std::uint32_t>(value.index());
            writes.push_back({kind, counts[kind]++, bytes(value)});
        }
        entries_.reserve(std::bit_ceil(entries_.size() + input.values.size() + 1));
        for (std::size_t k = 0; k < counts.size(); ++k)
        {
            arrays_[k].identities.reserve(counts[k]);
        }
        if (auto accepted = co_await write(mutation, writes, counts); !accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        std::vector<LightId> result;
        result.reserve(writes.size());
        for (const auto& value : writes)
        {
            const auto index = result.size();
            auto key = entries_.emplace(
                Entry{value.kind, value.position, input.shadows.empty() ? LightShadowSettings{} : input.shadows[index]}
            );
            arrays_[value.kind].identities.push_back(key);
            result.push_back(extension_detail::ResourceIdentity::make<LightId>(id_, key));
        }
        ++shadow_revision_;
        co_return result;
    }

    RenderResult<std::uint32_t> DirectLightingStorage::update(std::span<const DirectLightUpdate> input) noexcept
    {
        if (updating_)
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
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
        bool changed = false;
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
                changed = true;
            }
            entry->shadows = value.shadows;
        }
        preparation_revision_ += changed;
        shadow_revision_ += shadow_changed;
        return static_cast<std::uint32_t>(input.size());
    }

    exec::task<RenderResult<void>> DirectLightingStorage::remove(ReplicaMutation& mutation, LightId light) noexcept
    {
        auto* entry = extension_detail::ResourceIdentity::parent(light) == id_
                          ? entries_.find(extension_detail::ResourceIdentity::local(light))
                          : nullptr;
        if (!entry)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kDeviceBusy});
        }
        updating_ = true;
        ++preparation_revision_;
        cxx::scope_exit finish([&]() noexcept { updating_ = false; });
        std::array<std::uint32_t, 3> counts;
        for (std::size_t k = 0; k < counts.size(); ++k)
        {
            counts[k] = static_cast<std::uint32_t>(arrays_[k].identities.size());
        }
        auto& array = arrays_[entry->kind];
        const auto last = --counts[entry->kind];
        const auto stride = strides[entry->kind];
        std::array<std::byte, sizeof(SpotLightGpu)> replacement_bytes;
        std::array<Write, 1> replacement;
        std::span<const Write> writes;
        if (entry->position != last)
        {
            std::memcpy(replacement_bytes.data(), array.records.data() + last * stride, stride);
            replacement[0] = {entry->kind, entry->position, std::span(replacement_bytes).first(stride)};
            writes = replacement;
        }
        if (auto accepted = co_await write(mutation, writes, counts); !accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        if (entry->position != last)
        {
            array.identities[entry->position] = array.identities.back();
            entries_.find(array.identities.back())->position = entry->position;
        }
        array.identities.pop_back();
        entries_.erase(extension_detail::ResourceIdentity::local(light));
        ++shadow_revision_;
        co_return RenderResult<void>{};
    }

    std::array<std::uint32_t, 3> DirectLightingStorage::counts() const noexcept
    {
        return {
            static_cast<std::uint32_t>(arrays_[0].identities.size()),
            static_cast<std::uint32_t>(arrays_[1].identities.size()),
            static_cast<std::uint32_t>(arrays_[2].identities.size())
        };
    }

    RenderResult<SnapshotValue<ShadowLightSnapshot>> DirectLightingStorage::selectShadows() noexcept
    {
        if (updating_) return cxx::unexpected(RenderError{kDeviceBusy});
        auto& snapshot = shadow_selection_;
        if (snapshot.revision == shadow_revision_) return SnapshotValue<ShadowLightSnapshot>{snapshot, shadow_revision_};

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
        snapshot.revision = shadow_revision_;
        return SnapshotValue<ShadowLightSnapshot>{snapshot, shadow_revision_};
    }

    exec::task<RenderResult<void>>
    DirectLightingStorage::write(
        ReplicaMutation& mutation,
        std::span<const Write> changes,
        const std::array<std::uint32_t,
        3>& counts
    ) noexcept
    {
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested()) co_return cxx::unexpected(RenderError{kUploadStopped});
        const std::array<std::uint32_t, 4> header{counts[0], counts[1], counts[2], 0};
        const bool count_changed = std::memcmp(header.data(), arrays_[3].records.data(), sizeof(header)) != 0;
        if (changes.empty() && !count_changed)
        {
            co_return RenderResult<void>{};
        }
        std::array<bool, 4> changed{};
        for (const auto& write : changes)
        {
            changed[write.kind] = true;
        }
        changed[3] = count_changed;
        for (std::size_t k = 0; k < 3; ++k)
        {
            const auto size = std::max<std::uint64_t>(counts[k], 1) * strides[k];
            if (size > device_.description().memory.storageBufferMaxRange)
            {
                co_return cxx::unexpected(RenderError{kStorageCapacity});
            }
            arrays_[k].records.reserve(std::max(size, arrays_[k].records.size()));
        }
        auto replica = co_await mutation.select();
        if (!replica) co_return cxx::unexpected(replica.error());
        const auto frame = replica->slot;
        std::array<nri::Buffer*, 4> candidates{};
        std::array<std::size_t, 4> capacities{};
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                for (auto*& candidate : candidates)
                {
                    releaseAfterGpu(device_, candidate);
                }
            }
        );
        std::array<std::vector<std::byte>, 4> existing_records;
        std::vector<BufferWrite> writes;
        writes.reserve(changes.size() + 5);
        for (std::size_t k = 0; k < arrays_.size(); ++k)
        {
            if (!changed[k])
            {
                continue;
            }
            auto& array = arrays_[k];
            auto& slice = array.frames[frame];
            const auto count =
                k == 3 ? 1u : std::max(counts[k], static_cast<std::uint32_t>(array.records.size() / strides[k]));
            capacities[k] = std::max<std::size_t>(slice.capacity, std::bit_ceil(std::size_t(count)));
            for (auto& other : array.frames)
            {
                other.dirty.grow(std::max(count, std::uint32_t(array.records.size() / strides[k])));
            }
            if (!slice.buffer || slice.capacity < count)
            {
                const nri::BufferDesc description{
                    capacities[k] * strides[k],
                    4,
                    (nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE)
                };
                const auto status =
                    device_.nriCore().CreatePlacedBuffer(device_.device(), NriDeviceHeap, description, candidates[k]);
                if (status != nri::Result::SUCCESS)
                {
                    co_return cxx::unexpected(nativeError(status));
                }
                existing_records[k] = array.records;
                writes.push_back({candidates[k], 0, existing_records[k]});
            }
        }
        for (const auto& change : changes)
        {
            const auto k = change.kind;
            writes.push_back(
                {candidates[k] ? candidates[k] : arrays_[k].frames[frame].buffer,
                 std::uint64_t(change.position) * strides[k],
                 change.bytes}
            );
        }
        if (count_changed)
        {
            writes.push_back(
                {candidates[3] ? candidates[3] : arrays_[3].frames[frame].buffer, 0, std::as_bytes(std::span(header))}
            );
        }
        auto accepted = co_await mutation.writeBuffers(writes);
        if (accepted && stop.stop_requested()) accepted = cxx::unexpected(RenderError{kUploadStopped});
        if (!accepted)
        {
            // In-place copies may have been accepted. Keep CPU values and repair those records before a Draw.
            for (const auto& change : changes)
            {
                auto& array = arrays_[change.kind];
                if (!candidates[change.kind] && change.position < array.identities.size())
                {
                    array.frames[frame].dirty.mark(change.position);
                }
            }
            if (count_changed && !candidates[3])
            {
                arrays_[3].frames[frame].dirty.mark(0);
            }
            co_return cxx::unexpected(accepted.error());
        }
        // No suspension after acceptance: counts, records and resource identities publish together.
        for (std::size_t k = 0; k < 3; ++k)
        {
            arrays_[k].records.resize(
                std::max<std::size_t>(arrays_[k].records.size(), std::size_t(counts[k]) * strides[k])
            );
        }
        for (const auto& change : changes)
        {
            auto& array = arrays_[change.kind];
            std::memmove(
                array.records.data() + std::size_t(change.position) * strides[change.kind],
                change.bytes.data(),
                change.bytes.size()
            );
            for (std::uint32_t f = 0; f < frame_count_; ++f)
            {
                if (f == frame)
                {
                    array.frames[f].dirty.acknowledgeRange(change.position, 1);
                }
                else
                {
                    array.frames[f].dirty.mark(change.position);
                }
            }
        }
        if (count_changed)
        {
            std::memcpy(arrays_[3].records.data(), header.data(), sizeof(header));
            for (std::uint32_t f = 0; f < frame_count_; ++f)
            {
                if (f == frame)
                {
                    arrays_[3].frames[f].dirty.acknowledgeRange(0, 1);
                }
                else
                {
                    arrays_[3].frames[f].dirty.mark(0);
                }
            }
        }
        for (std::size_t k = 0; k < arrays_.size(); ++k)
        {
            if (!candidates[k])
            {
                continue;
            }
            auto& slice = arrays_[k].frames[frame];
            releaseAfterGpu(device_, slice.candidate);
            releaseAfterGpu(device_, slice.buffer);
            slice.buffer = std::exchange(candidates[k], nullptr);
            slice.capacity = capacities[k];
            slice.candidate_capacity = 0;
            slice.dirty.clear();
            publish(static_cast<std::uint32_t>(k), frame);
        }
        co_return RenderResult<void>{};
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
            if (slice.dirty.empty())
            {
                continue;
            }
            const auto records = array.records.size() / strides[k];
            if (std::max(slice.capacity, slice.candidate_capacity) < records)
            {
                const auto capacity = std::bit_ceil(records);
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
                    auto result = uploads.stageBuffer(
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
            if (!slice.dirty.empty())
            {
                return cxx::unexpected(RenderError{kDeviceCapacity});
            }
            ready &= !slice.candidate;
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
        static const auto frame_call =
            FrameDataCall::declare<LightResource, DirectLightingStorage, &updateLights>();

        static constexpr auto snapshot =
            StorageSnapshotOps::make<DirectLightingStorage, ShadowLightSnapshot, &DirectLightingStorage::selectShadows>();
        static const StorageBindingOps binding{
            [](void* value, ReplicaFrame frame, ResourceUploader& uploads) noexcept
            { return static_cast<DirectLightingStorage*>(value)->prepareFrame(frame.slot, uploads); },
            [](const void* value) noexcept
            { return &static_cast<const DirectLightingStorage*>(value)->preparation_revision_; },
            [](const void*, std::uint32_t projection) noexcept -> RenderResult<StorageGraphImport>
            {
                if (projection >= strides.size()) return cxx::unexpected(RenderError{kStorageContract});
                const auto bytes = strides[projection];
                return StorageGraphImport{GraphBufferDesc{bytes}, {GraphBufferRange{0, bytes}}, EGraphUse::STORAGE_READ};
            },
            [](void* value, std::uint32_t projection, const VGraphResourceDesc& resource,
               std::span<ResourceInput> output) noexcept -> RenderResult<void>
            {
                if (projection >= strides.size()) return cxx::unexpected(RenderError{kStorageContract});
                return static_cast<DirectLightingStorage*>(value)->resolve(
                    resource, DirectLightingBinding{static_cast<EDirectLightingBuffer>(projection)}, output
                );
            },
            &snapshot,
            [](void* value, const ReplicaSettlement& settlement) noexcept
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
        return makeResourceRegistration<LightResource, DirectLightingStorage, create>(
            &binding,
            frame_call
        );
    }
} // namespace lux::render::detail
