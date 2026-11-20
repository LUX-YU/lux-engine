#pragma once

#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/extensions/effect/EffectUniformBlock.hpp>
#include <lux/engine/render/extensions/execution/ResourceInput.hpp>
#include <lux/engine/render/extensions/graph/GraphResource.hpp>
#include <memory>

namespace lux::render::detail
{
    class DeviceSession;
    class ResourceUploader;

    class UniformBlockVersions final
    {
    public:
        enum class EPhase : std::uint8_t { EMPTY, DIRTY, UPLOADING, READY, QUARANTINED };

        struct Version final
        {
            SubmissionFrontiers accepted;
            nri::Buffer* buffer = nullptr;
            std::uint64_t revision = 0;
            std::uint64_t upload_serial = 0;
            std::uint32_t prepared_pins = 0;
            std::vector<std::byte> bytes;
            EPhase phase = EPhase::EMPTY;
        };

        UniformBlockVersions(
            DeviceSession&, const void* state, extension_detail::EffectUniformSource, std::uint32_t frame_count
        ) noexcept;
        ~UniformBlockVersions() noexcept;
        UniformBlockVersions(const UniformBlockVersions&) = delete;
        UniformBlockVersions& operator=(const UniformBlockVersions&) = delete;

        [[nodiscard]] RenderResult<Version*> pin(const SubmissionFrontiers& completed) noexcept;
        [[nodiscard]] RenderResult<void> stage(Version&, ResourceUploader&) noexcept;
        void settleUpload(Version&, SubmissionPoint, RenderError) noexcept;
        void unpin(Version&, bool quarantine) noexcept;
        [[nodiscard]] ResourceInput binding(const Version&) const noexcept;
        [[nodiscard]] bool matches(extension_detail::EffectUniformSource) const noexcept;

    private:
        DeviceSession& device_;
        const void* state_;
        extension_detail::EffectUniformSource source_;
        std::vector<Version> versions_;
    };

    struct LinkedUniformBlock final
    {
        UniformBlockVersions* versions;
        GraphResourceId resource;
        std::vector<std::uint32_t> batches;
    };

    struct SelectedUniformBlock final
    {
        UniformBlockVersions* owner;
        UniformBlockVersions::Version* version;
    };
} // namespace lux::render::detail
