#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <lux/engine/render/resource/transform/TransformStorage.hpp>
#include <lux/engine/render/resources/transform/Binding.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <bit>
#include <utility>

namespace lux::render::detail
{
    TransformStorage::~TransformStorage() noexcept
    {
        for (auto& replica : replicas_)
        {
            destroy(replica.binding);
            destroy(replica.candidate);
            for (auto* page : replica.pages) destroy(page);
        }
    }

    void TransformStorage::destroy(nri::Buffer* buffer) noexcept
    {
        if (buffer)
        {
            session_.deferRelease([buffer, release = session_.nriCore().DestroyBuffer]() noexcept { release(buffer); });
        }
    }

    RenderResult<nri::Buffer*> TransformStorage::allocate(std::uint64_t bytes) noexcept
    {
        if (bytes > session_.description().memory.storageBufferMaxRange)
            return cxx::unexpected(RenderError{kStorageCapacity});
        const auto usage = nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE;
        const nri::BufferDesc desc{bytes, 4, usage};
        nri::Buffer* buffer = nullptr;
        const auto result = session_.nriCore().CreatePlacedBuffer(session_.device(), NriDeviceHeap, desc, buffer);
        if (result != nri::Result::SUCCESS) return cxx::unexpected(nativeError(result));
        return buffer;
    }

    RenderResult<void> TransformStorage::ensureReplica(Replica& replica) noexcept
    {
        if (paged_)
        {
            const auto pages = (matrices_.size() + kPageSlots - 1) / kPageSlots;
            const auto addresses = std::bit_ceil(std::max<std::size_t>(kTransformInitialPageAddresses, pages));
            auto* table = replica.candidate ? replica.candidate : replica.binding;
            const auto bytes = addresses * sizeof(std::uint64_t);
            if (!table || session_.nriCore().GetBufferDesc(*table).size < bytes)
            {
                auto candidate = allocate(bytes);
                if (!candidate) return cxx::unexpected(candidate.error());
                destroy(std::exchange(replica.candidate, *candidate));
                replica.addresses.resize(addresses);
                replica.table_dirty = true;
            }
            while (replica.pages.size() < pages)
            {
                auto page = allocate(std::uint64_t{kPageSlots} * sizeof(TransformMatrix));
                if (!page) return cxx::unexpected(page.error());
                const auto index = replica.pages.size();
                replica.addresses[index] = session_.nriCore().GetBufferDeviceAddress(**page);
                replica.pages.push_back(*page);
                replica.table_dirty = true;
                const auto first = index * kPageSlots;
                replica.dirty.grow(matrices_.size());
                replica.dirty.markRange(first, std::min<std::size_t>(kPageSlots, matrices_.size() - first));
            }
            return {};
        }

        auto* buffer = replica.candidate ? replica.candidate : replica.binding;
        const auto size = matrices_.size() * sizeof(TransformMatrix);
        if (!buffer || session_.nriCore().GetBufferDesc(*buffer).size < size)
        {
            auto candidate = allocate(std::bit_ceil(size));
            if (!candidate) return cxx::unexpected(candidate.error());
            destroy(std::exchange(replica.candidate, *candidate));
            replica.dirty.grow(matrices_.size());
            replica.dirty.markRange(0, matrices_.size());
        }
        return {};
    }

    RenderResult<bool> TransformStorage::prepareFrame(
        std::uint32_t frame, ResourceUploader& uploader
    ) noexcept
    {
        if (frame >= frame_count_) return cxx::unexpected(RenderError{kDeviceArgument});
        auto& replica = replicas_[frame];
        if (auto ready = ensureReplica(replica); !ready) return cxx::unexpected(ready.error());
        auto* binding = replica.candidate ? replica.candidate : replica.binding;
        if (replica.table_dirty)
        {
            const auto bytes = std::as_bytes(std::span(replica.addresses));
            auto copied = uploader.stageBuffer({binding, 0, bytes}, sizeof(std::uint64_t));
            if (!copied) return cxx::unexpected(copied.error());
            if (*copied != bytes.size()) return cxx::unexpected(RenderError{kDeviceCapacity});
            replica.table_staged = true;
        }
        const auto word_limit = (uploader.windowByteLimit() / sizeof(TransformMatrix) + 63) / 64 +
            uploader.windowRegionLimit();
        replica.dirty.select(word_limit, replica.selected);
        RenderResult<void> status;
        const bool complete = DirtyRanges::visitRanges(replica.selected, kPageSlots,
            [&](std::uint32_t first, std::uint32_t count) noexcept
            {
                const auto local = paged_ ? first % kPageSlots : first;
                auto* target = paged_ ? replica.pages[first / kPageSlots] : binding;
                const auto bytes = std::as_bytes(std::span(matrices_).subspan(first, count));
                const auto offset = std::uint64_t{local} * sizeof(TransformMatrix);
                auto copied = uploader.stageBuffer({target, offset, bytes}, sizeof(TransformMatrix));
                if (!copied)
                {
                    status = cxx::unexpected(copied.error());
                    return false;
                }
                if (*copied != bytes.size()) std::terminate();
                replica.dirty.detachRange(replica.staged, first, count);
                return true;
            }
        );
        if (!status) return cxx::unexpected(status.error());
        if (!complete || !replica.dirty.empty()) return cxx::unexpected(RenderError{kDeviceCapacity});
        // Dynamic draw preparation resolves the current descriptors before recording.
        return !replica.candidate && replica.addressed.size() == replica.pages.size();
    }

    ResourceInput TransformStorage::binding(std::uint32_t frame) const noexcept
    {
        const auto& replica = replicas_[frame];
        return {
            replica.binding,
            0,
            replica.binding ? session_.nriCore().GetBufferDesc(*replica.binding).size : sizeof(TransformMatrix),
            nullptr,
            replica.addressed,
            replica.address_revision
        };
    }

    void TransformStorage::finishPreparation(const ReplicaSettlement& settlement) noexcept
    {
        if (!settlement.copies.count) return;
        const auto frame = settlement.frame.slot;
        const bool result = !settlement.error.type;
        auto& replica = replicas_[frame];
        replica.dirty.finishSelection(replica.staged, bool(result));
        if (std::exchange(replica.table_staged, false) && result)
            replica.table_dirty = false;
        if (!result) return;
        bool changed = replica.candidate != nullptr;
        if (replica.candidate)
            destroy(std::exchange(replica.binding, std::exchange(replica.candidate, nullptr)));
        if (replica.addressed.size() != replica.pages.size())
        {
            replica.addressed = replica.pages;
            ++replica.address_revision;
            changed = true;
        }
        if (changed)
        {
            const auto projection = paged_ ? ETransformGraphProjection::PAGE_ADDRESSES
                                           : ETransformGraphProjection::RECORDS;
            if (!resources_.publishGraphInput(id_, static_cast<std::uint32_t>(projection), frame, binding(frame)))
                std::terminate();
        }
    }

    RenderResult<void> TransformStorage::resolve(
        const VGraphResourceDesc& plan, std::span<ResourceInput> output
    ) noexcept
    {
        const auto* desc = std::get_if<GraphBufferDesc>(&plan);
        if (!desc || desc->size > sizeof(TransformMatrix))
            return cxx::unexpected(RenderError{kStorageMismatch});
        if (output.size() > frame_count_) return cxx::unexpected(RenderError{kDeviceArgument});
        for (std::uint32_t frame = 0; frame < output.size(); ++frame) output[frame] = binding(frame);
        return {};
    }
} // namespace lux::render::detail
