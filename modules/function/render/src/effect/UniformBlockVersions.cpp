#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/effect/UniformBlockVersions.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <utility>

namespace lux::render::detail
{
    void RenderGraphInstance::linkUniformBlocks(std::span<const LinkedUniformBlock> blocks) noexcept
    {
        uniform_blocks_ = blocks;
    }

    UniformBlockVersions::UniformBlockVersions(
        DeviceSession& device, extension_detail::EffectUniformSource source,
        std::uint32_t frame_count
    ) noexcept : device_(device), source_(source)
    {
        versions_.reserve(frame_count + 1);
        for (std::uint32_t i = 0; i <= frame_count; ++i)
        {
            versions_.push_back({device_.makeFrontiers()});
        }
    }

    UniformBlockVersions::~UniformBlockVersions() noexcept
    {
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

    bool UniformBlockVersions::matches(extension_detail::EffectUniformSource source) const noexcept
    {
        return source_.state_type == source.state_type && source_.select == source.select &&
            source_.block.type == source.block.type && source_.block.layout == source.block.layout &&
            source_.block.bytes == source.block.bytes && source_.block.alignment == source.block.alignment;
    }

    RenderResult<UniformVersionId> UniformBlockVersions::pin(
        const void* state, const SubmissionFrontiers& completed
    ) noexcept
    {
        const auto snapshot = source_.select(state);
        if (snapshot.bytes.size() != source_.block.bytes || !snapshot.revision) std::terminate();
        auto match = std::ranges::find(versions_, snapshot.revision, &Version::revision);
        if (match != versions_.end())
        {
            if (match->phase == EPhase::QUARANTINED) return cxx::unexpected(RenderError{kOutcomeUnknown});
            if (match->prepared_pins == UINT32_MAX) return cxx::unexpected(RenderError{kRuntimeCapacity});
            ++match->prepared_pins;
            return UniformVersionId{static_cast<std::uint32_t>(match - versions_.begin()), snapshot.revision};
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
            const auto bytes = std::uint64_t{source_.block.bytes};
            const auto padding = limits.graphicsAPI == nri::GraphicsAPI::D3D12 ? (alignment - bytes % alignment) % alignment : 0;
            const nri::BufferDesc desc{bytes + padding, 0, nri::BufferUsageBits::CONSTANT};
            const auto created = device_.nriCore().CreatePlacedBuffer(device_.device(), NriDeviceHeap, desc, reusable->buffer);
            if (created != nri::Result::SUCCESS) return cxx::unexpected(nativeError(created));
        }
        reusable->bytes.assign(snapshot.bytes.begin(), snapshot.bytes.end());
        reusable->accepted.clear();
        reusable->upload_serial = 0;
        reusable->revision = snapshot.revision;
        reusable->prepared_pins = 1;
        reusable->phase = EPhase::DIRTY;
        return UniformVersionId{static_cast<std::uint32_t>(reusable - versions_.begin()), snapshot.revision};
    }

    const UniformBlockVersions::Version& UniformBlockVersions::resolve(UniformVersionId id) const noexcept
    {
        const bool valid_slot = id && id.slot < versions_.size();
        const bool live = valid_slot && versions_[id.slot].revision == id.revision && versions_[id.slot].prepared_pins;
        if (!live) std::terminate();
        return versions_[id.slot];
    }

    UniformBlockVersions::Version& UniformBlockVersions::resolve(UniformVersionId id) noexcept
    {
        return const_cast<Version&>(std::as_const(*this).resolve(id));
    }

    RenderResult<void> UniformBlockVersions::stage(UniformVersionId id, ResourceUploader& uploader) noexcept
    {
        auto& version = resolve(id);
        if (version.phase == EPhase::READY || version.phase == EPhase::UPLOADING) return {};
        if (version.phase != EPhase::DIRTY || !version.prepared_pins) std::terminate();
        auto staged = uploader.stageBuffer({version.buffer, 0, version.bytes});
        if (!staged) return cxx::unexpected(staged.error());
        if (*staged != version.bytes.size()) std::terminate();
        version.phase = EPhase::UPLOADING;
        return {};
    }

    void UniformBlockVersions::settleUpload(UniformVersionId id, SubmissionPoint point, RenderError error) noexcept
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
        version.upload_serial = point.serial;
        version.phase = EPhase::READY;
    }

    void UniformBlockVersions::accept(UniformVersionId id, SubmissionPoint point) noexcept
    {
        resolve(id).accepted.accept(point);
    }

    void UniformBlockVersions::unpin(UniformVersionId id, bool quarantine) noexcept
    {
        auto& version = resolve(id);
        if (!version.prepared_pins || version.phase == EPhase::UPLOADING) std::terminate();
        if (quarantine) version.phase = EPhase::QUARANTINED;
        --version.prepared_pins;
    }

    bool UniformBlockVersions::ready(UniformVersionId id) const noexcept
    {
        return resolve(id).phase == EPhase::READY;
    }

    std::uint64_t UniformBlockVersions::uploadSerial(UniformVersionId id) const noexcept
    {
        return resolve(id).upload_serial;
    }

    ResourceInput UniformBlockVersions::binding(UniformVersionId id) const noexcept
    {
        const auto& version = resolve(id);
        if (version.phase != EPhase::READY || !version.prepared_pins) std::terminate();
        return {version.buffer, 0, source_.block.bytes};
    }
} // namespace lux::render::detail
