#include <lux/engine/render/storage/DirectLightingStorage.hpp>

#include <bit>
#include <cmath>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>

namespace lux::render::detail
{
    namespace
    {
        constexpr std::array<std::size_t, 4> strides{32, 48, 64, 16};

        std::span<const std::byte> bytes(const VDirectLight& value) noexcept
        {
            return std::visit([](const auto& record) { return std::as_bytes(std::span(&record, 1)); }, value);
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

    DirectLightingStorage::DirectLightingStorage(StorageCreateContext& dependencies, StorageId id) noexcept
        : device_(dependencies.device), uploads_(*dependencies.uploads), frames_(*dependencies.frames), id_(id)
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
            StorageSubscription::disconnect(array.bindings);
            for (auto& frame : array.frames)
            {
                releaseAfterGpu(device_, frame.buffer);
                releaseAfterGpu(device_, frame.candidate);
            }
        }
    }

    exec::task<RenderResult<std::vector<LightId>>> DirectLightingStorage::create(CreateDirectLights::Request input
    ) noexcept
    {
        if (input.storage != id_)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kDeviceBusy});
        }
        updating_ = true;
        cxx::scope_exit finish([&]() noexcept { updating_ = false; });
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
        if (auto accepted = co_await write(writes, counts); !accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        std::vector<LightId> result;
        result.reserve(writes.size());
        for (const auto& value : writes)
        {
            auto key = entries_.emplace(Entry{value.kind, value.position});
            arrays_[value.kind].identities.push_back(key);
            result.push_back({id_, key});
        }
        ++revision_;
        co_return result;
    }

    exec::task<RenderResult<std::uint32_t>> DirectLightingStorage::update(UpdateDirectLights::Request input) noexcept
    {
        if (input.storage != id_)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kDeviceBusy});
        }
        updating_ = true;
        cxx::scope_exit finish([&]() noexcept { updating_ = false; });
        std::array<std::uint32_t, 3> counts;
        for (std::size_t k = 0; k < counts.size(); ++k)
        {
            counts[k] = static_cast<std::uint32_t>(arrays_[k].identities.size());
        }
        std::vector<Write> writes;
        writes.reserve(input.values.size());
        for (const auto& value : input.values)
        {
            const auto* entry = value.light.storage == id_ ? entries_.find(value.light.local) : nullptr;
            if (!entry)
            {
                co_return cxx::unexpected(RenderError{kStorageIdentity});
            }
            if (entry->kind != value.value.index() || !validLight(value.value))
            {
                co_return cxx::unexpected(RenderError{kStorageContract});
            }
            writes.push_back({entry->kind, entry->position, bytes(value.value)});
        }
        if (auto accepted = co_await write(writes, counts); !accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        ++revision_;
        co_return static_cast<std::uint32_t>(writes.size());
    }

    exec::task<RenderResult<Ack>> DirectLightingStorage::remove(LightId light) noexcept
    {
        auto* entry = light.storage == id_ ? entries_.find(light.local) : nullptr;
        if (!entry)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kDeviceBusy});
        }
        updating_ = true;
        cxx::scope_exit finish([&]() noexcept { updating_ = false; });
        std::array<std::uint32_t, 3> counts;
        for (std::size_t k = 0; k < counts.size(); ++k)
        {
            counts[k] = static_cast<std::uint32_t>(arrays_[k].identities.size());
        }
        auto& array = arrays_[entry->kind];
        const auto last = --counts[entry->kind];
        const auto stride = strides[entry->kind];
        const Write replacement{entry->kind, entry->position, std::span(array.records).subspan(last * stride, stride)};
        const auto writes = entry->position == last ? std::span<const Write>{} : std::span(&replacement, 1);
        if (auto accepted = co_await write(writes, counts); !accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        if (entry->position != last)
        {
            array.identities[entry->position] = array.identities.back();
            entries_.find(array.identities.back())->position = entry->position;
        }
        array.identities.pop_back();
        entries_.erase(light.local);
        ++revision_;
        co_return Ack{};
    }

    std::array<std::uint32_t, 3> DirectLightingStorage::counts() const noexcept
    {
        return {
            static_cast<std::uint32_t>(arrays_[0].identities.size()),
            static_cast<std::uint32_t>(arrays_[1].identities.size()),
            static_cast<std::uint32_t>(arrays_[2].identities.size())
        };
    }

    bool DirectLightingStorage::read(LightId id, VDirectLight& result, std::uint32_t& dense_index) const noexcept
    {
        const auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry)
        {
            return false;
        }
        dense_index = entry->position;
        const auto* bytes = arrays_[entry->kind].records.data() + std::size_t(entry->position) * strides[entry->kind];
        const auto copy = [&]<class T>() noexcept
        {
            T light;
            std::memcpy(&light, bytes, sizeof(light));
            result = light;
        };
        switch (entry->kind)
        {
        case 0:
            copy.template operator()<DirectionalLightGpu>();
            break;
        case 1:
            copy.template operator()<PointLightGpu>();
            break;
        case 2:
            copy.template operator()<SpotLightGpu>();
            break;
        }
        return true;
    }

    exec::task<RenderResult<void>> DirectLightingStorage::write(
        std::span<const Write> changes,
        const std::array<std::uint32_t, 3>& counts
    ) noexcept
    {
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
        std::uint32_t frame = 0;
        while (!frames_.tryBeginWrite(frame))
        {
            if (auto ready = co_await frames_.whenWritable(changes.size() * 64 + sizeof(header)); !ready)
            {
                co_return cxx::unexpected(ready.error());
            }
        }
        SubmissionFrontiers progress;
        cxx::scope_exit finish([&]() noexcept { frames_.finishWrite(frame, progress); });
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
                    nri::BufferUsageBits::SHADER_RESOURCE_STORAGE
                };
                const auto status =
                    device_.nriCore().CreatePlacedBuffer(device_.device(), NriDeviceHeap, description, candidates[k]);
                if (status != nri::Result::SUCCESS)
                {
                    co_return cxx::unexpected(nativeError(status));
                }
                writes.push_back({candidates[k], 0, array.records});
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
        auto accepted = co_await uploads_.writeBuffers(writes, {}, frame, &progress);
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
            for (std::uint32_t f = 0; f < frames_.frameCount(); ++f)
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
            for (std::uint32_t f = 0; f < frames_.frameCount(); ++f)
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
        StorageSubscription::publish(arrays_[kind].bindings, frame, {slice.buffer, 0, slice.capacity * strides[kind]});
    }

    RenderResult<void> DirectLightingStorage::resolve(
        const CompiledResource& resource,
        DirectLightingBinding binding,
        StorageSubscription& subscription,
        std::span<ResourceInput> output
    ) noexcept
    {
        const auto field = static_cast<std::size_t>(binding.field);
        const auto* buffer = std::get_if<GraphBufferDesc>(&resource.description);
        if (field >= arrays_.size() || !buffer || buffer->size != strides[field])
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        auto& array = arrays_[field];
        subscription.attach(array.bindings);
        for (std::uint32_t f = 0; f < output.size(); ++f)
        {
            const auto& slice = array.frames[f];
            output[f] = {slice.buffer, 0, slice.buffer ? slice.capacity * strides[field] : strides[field]};
        }
        return {};
    }

    RenderResult<bool> DirectLightingStorage::prepareFrame(std::uint32_t frame, ResourceUploader& uploads) noexcept
    {
        bool progress = false;
        cxx::scope_exit submit(
            [&]() noexcept
            {
                if (progress)
                {
                    frames_.onPreparationSubmitted(
                        this,
                        [](void* value, std::uint32_t f, RenderResult<void> result) noexcept
                        { static_cast<DirectLightingStorage*>(value)->finishPreparation(f, result); }
                    );
                }
            }
        );
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
                    nri::BufferUsageBits::SHADER_RESOURCE_STORAGE
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
            std::uint64_t covered = 0;
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
                    DirtyRanges::appendRange(slice.accepted, first, accepted);
                    progress |= accepted != 0;
                    covered += accepted;
                    return accepted == count;
                }
            );
            if (!copied)
            {
                return cxx::unexpected(copied.error());
            }
            ready &= covered == slice.dirty.recordCount() && !slice.candidate;
        }
        return ready;
    }

    void DirectLightingStorage::finishPreparation(std::uint32_t frame, RenderResult<void> accepted) noexcept
    {
        for (std::uint32_t k = 0; k < arrays_.size(); ++k)
        {
            auto& slice = arrays_[k].frames[frame];
            if (accepted)
            {
                slice.dirty.acknowledge(slice.accepted);
            }
            slice.accepted.clear();
            if (accepted && slice.dirty.empty() && slice.candidate)
            {
                releaseAfterGpu(device_, slice.buffer);
                slice.buffer = std::exchange(slice.candidate, nullptr);
                slice.capacity = std::exchange(slice.candidate_capacity, 0);
                publish(k, frame);
            }
        }
    }

    StorageFactory DirectLightingStorage::factory() noexcept
    {
        static const StorageBindingOps binding{
            [](void* value,
               const CompiledResource& resource,
               const void* argument,
               StorageSubscription& node,
               std::span<ResourceInput> output) noexcept
            {
                return static_cast<DirectLightingStorage*>(value)
                    ->resolve(resource, *static_cast<const DirectLightingBinding*>(argument), node, output);
            },
            [](void* value, std::uint32_t frame, ResourceUploader& uploads) noexcept
            { return static_cast<DirectLightingStorage*>(value)->prepareFrame(frame, uploads); }
        };
        static const auto create = +[](StorageCreateContext& dependencies,
                                       StorageId id) noexcept -> RenderResult<std::unique_ptr<DirectLightingStorage>>
        {
            if (!dependencies.frames || !dependencies.uploads)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            return std::make_unique<DirectLightingStorage>(dependencies, id);
        };
        return makeStorageFactory<DirectLightingStorageDescriptor, DirectLightingStorage, StorageCreateContext, create>(
            nullptr,
            &binding
        );
    }
} // namespace lux::render::detail
