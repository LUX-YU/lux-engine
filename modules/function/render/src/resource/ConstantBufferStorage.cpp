#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/resource/ConstantBufferStorage.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <utility>

namespace lux::render::detail
{
    ConstantBufferStorage::ConstantBufferStorage(
        DeviceSession& device, std::uint32_t bytes, std::uint32_t frame_count
    ) noexcept : device_(device), bytes_(bytes)
    {
        versions_.reserve(frame_count + 1);
        for (std::uint32_t i = 0; i <= frame_count; ++i)
        {
            versions_.push_back({device_.makeFrontiers(), device_.makeFrontiers()});
        }
    }

    ConstantBufferStorage::~ConstantBufferStorage() noexcept
    {
        if (selection_ != SIZE_MAX) std::terminate();
        for (auto& version : versions_)
        {
            if (version.prepared_pins || version.phase == EPhase::UPLOADING) std::terminate();
            if (!version.buffer) continue;
            device_.deferRelease(
                std::move(version.accepted),
                [buffer = version.buffer, destroy = device_.nriCore().DestroyBuffer]() noexcept { destroy(buffer); }
            );
        }
    }

    RenderResult<ConstantBufferVersion> ConstantBufferStorage::pin(
        std::uint64_t revision, const SubmissionFrontiers& completed
    ) noexcept
    {
        if (!revision) return cxx::unexpected(RenderError{kFrameContract});
        auto match = std::ranges::find(versions_, revision, &Version::revision);
        if (match != versions_.end())
        {
            if (match->phase == EPhase::QUARANTINED) return cxx::unexpected(RenderError{kOutcomeUnknown});
            if (match->prepared_pins == UINT32_MAX) return cxx::unexpected(RenderError{kRuntimeCapacity});
            ++match->prepared_pins;
            return ConstantBufferVersion{static_cast<std::uint32_t>(match - versions_.begin()), revision};
        }

        // Prefer unused native backings. With every slot in use, wait for actual completed frontiers.
        auto reusable = std::ranges::find(versions_, EPhase::EMPTY, &Version::phase);
        if (reusable == versions_.end())
        {
            reusable = std::ranges::find_if(versions_, [&completed](const Version& version) noexcept
            {
                return !version.prepared_pins && version.phase != EPhase::UPLOADING &&
                    version.phase != EPhase::QUARANTINED && version.accepted.reachedBy(completed);
            });
        }
        if (reusable == versions_.end()) return cxx::unexpected(RenderError{kDeviceBusy});
        if (!reusable->buffer)
        {
            const auto& limits = device_.description();
            const auto alignment = limits.memoryAlignment.constantBufferOffset;
            if (!alignment) return cxx::unexpected(RenderError{kDeviceUnsupported});
            const auto bytes = std::uint64_t{bytes_};
            const bool padded = limits.graphicsAPI == nri::GraphicsAPI::D3D12;
            const auto padding = padded ? (alignment - bytes % alignment) % alignment : 0;
            const nri::BufferDesc desc{bytes + padding, 0, nri::BufferUsageBits::CONSTANT};
            const auto created = device_.nriCore().CreatePlacedBuffer(
                device_.device(), NriDeviceHeap, desc, reusable->buffer
            );
            if (created != nri::Result::SUCCESS) return cxx::unexpected(nativeError(created));
        }
        reusable->accepted.clear();
        reusable->preparation.clear();
        reusable->revision = revision;
        reusable->prepared_pins = 1;
        reusable->phase = EPhase::DIRTY;
        return ConstantBufferVersion{static_cast<std::uint32_t>(reusable - versions_.begin()), revision};
    }

    const ConstantBufferStorage::Version& ConstantBufferStorage::resolve(ConstantBufferVersion id) const noexcept
    {
        const bool valid_slot = id && id.slot < versions_.size();
        const bool live = valid_slot && versions_[id.slot].revision == id.revision && versions_[id.slot].prepared_pins;
        if (!live) std::terminate();
        return versions_[id.slot];
    }

    ConstantBufferStorage::Version& ConstantBufferStorage::resolve(ConstantBufferVersion id) noexcept
    {
        return const_cast<Version&>(std::as_const(*this).resolve(id));
    }

    RenderResult<void> ConstantBufferStorage::stage(
        ConstantBufferVersion id, std::span<const std::byte> bytes, ResourceUploader& uploader
    ) noexcept
    {
        auto& version = resolve(id);
        if (version.phase == EPhase::READY || version.phase == EPhase::UPLOADING) return {};
        if (version.phase != EPhase::DIRTY || bytes.size() != bytes_) std::terminate();
        auto staged = uploader.stageBuffer({version.buffer, 0, bytes});
        if (!staged) return cxx::unexpected(staged.error());
        if (*staged != bytes.size()) std::terminate();
        version.phase = EPhase::UPLOADING;
        return {};
    }

    void ConstantBufferStorage::settleUpload(ConstantBufferVersion id, SubmissionPoint point, RenderError error) noexcept
    {
        auto& version = resolve(id);
        if (version.phase != EPhase::UPLOADING) return;
        if (error.type)
        {
            version.phase = error.type == kOutcomeUnknown ? EPhase::QUARANTINED : EPhase::DIRTY;
            return;
        }
        if (!point.serial) std::terminate();
        version.accepted.accept(point);
        version.preparation.accept(point);
        version.phase = EPhase::READY;
    }

    void ConstantBufferStorage::unpin(ConstantBufferVersion id, bool quarantine) noexcept
    {
        auto& version = resolve(id);
        if (!version.prepared_pins || version.phase == EPhase::UPLOADING) std::terminate();
        if (quarantine) version.phase = EPhase::QUARANTINED;
        --version.prepared_pins;
    }

    bool ConstantBufferStorage::ready(ConstantBufferVersion id) const noexcept
    {
        return resolve(id).phase == EPhase::READY;
    }

    ResourceInput ConstantBufferStorage::binding(ConstantBufferVersion id) noexcept
    {
        auto& version = resolve(id);
        if (version.phase != EPhase::READY || !version.prepared_pins) std::terminate();
        return {.buffer = version.buffer, .size = bytes_, .queue_uses = {&version.preparation, &version.accepted}};
    }

    RenderResult<std::optional<ResourceInput>>
    ConstantBufferSelections::prepare(
        const BoundConstantBuffer& input, const SubmissionFrontiers& completed, ResourceUploader& uploads
    ) noexcept
    {
        auto& storage = *input.storage;
        const bool first = storage.selection_ == SIZE_MAX;
        if (first)
        {
            storage.selection_ = entries_.size();
            entries_.emplace_back(&storage);
        }
        auto& entry = entries_[storage.selection_];
        if (!entry.version)
        {
            if (first) entry.revision = *input.value.revision;
            auto pinned = storage.pin(entry.revision, completed);
            if (!pinned && pinned.error().type != kDeviceBusy) return cxx::unexpected(pinned.error());
            if (first && (!pinned || !storage.ready(*pinned)))
                entry.pending.assign(input.value.bytes.begin(), input.value.bytes.end());
            if (!pinned) return std::nullopt;
            entry.version = *pinned;
        }
        if (!storage.ready(entry.version))
        {
            auto staged = storage.stage(entry.version, entry.pending, uploads);
            if (!staged) return cxx::unexpected(staged.error());
            return std::nullopt;
        }
        return storage.binding(entry.version);
    }

    void ConstantBufferSelections::settle(SubmissionPoint point, RenderError error) noexcept
    {
        for (const auto& entry : entries_)
            if (entry.version) entry.storage->settleUpload(entry.version, point, error);
    }

    void ConstantBufferSelections::finish(bool unknown) noexcept
    {
        for (const auto& entry : entries_)
        {
            if (entry.version) entry.storage->unpin(entry.version, unknown);
            entry.storage->selection_ = SIZE_MAX;
        }
        entries_.clear();
    }
} // namespace lux::render::detail
