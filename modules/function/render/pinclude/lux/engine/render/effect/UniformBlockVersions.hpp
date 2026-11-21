#pragma once

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/extensions/effect/EffectUniformBlock.hpp>
#include <lux/engine/render/extensions/execution/ResourceInput.hpp>
#include <lux/engine/render/extensions/graph/GraphResource.hpp>
#include <memory>

namespace lux::render::detail
{
    class DeviceSession;
    class ResourceUploader;

    using UniformPoolId = cxx::SlotKey<struct UniformPoolTag>;

    struct UniformVersionId final
    {
        std::uint32_t slot = 0;
        std::uint64_t revision = 0;

        [[nodiscard]] explicit operator bool() const noexcept { return revision != 0; }
    };

    class UniformBlockVersions final
    {
    public:
        UniformBlockVersions(
            DeviceSession&, extension_detail::EffectUniformSource, std::uint32_t frame_count
        ) noexcept;
        ~UniformBlockVersions() noexcept;
        UniformBlockVersions(const UniformBlockVersions&) = delete;
        UniformBlockVersions& operator=(const UniformBlockVersions&) = delete;

        [[nodiscard]] RenderResult<UniformVersionId> pin(const void*, const SubmissionFrontiers&) noexcept;
        [[nodiscard]] RenderResult<void> stage(UniformVersionId, ResourceUploader&) noexcept;
        void settleUpload(UniformVersionId, SubmissionPoint, RenderError) noexcept;
        void accept(UniformVersionId, SubmissionPoint) noexcept;
        void unpin(UniformVersionId, bool quarantine) noexcept;
        [[nodiscard]] bool ready(UniformVersionId) const noexcept;
        [[nodiscard]] std::uint64_t uploadSerial(UniformVersionId) const noexcept;
        [[nodiscard]] ResourceInput binding(UniformVersionId) const noexcept;
        [[nodiscard]] bool matches(extension_detail::EffectUniformSource) const noexcept;
        [[nodiscard]] cxx::basic_type_info stateType() const noexcept { return source_.state_type; }

    private:
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

        [[nodiscard]] const Version& resolve(UniformVersionId) const noexcept;
        [[nodiscard]] Version& resolve(UniformVersionId) noexcept;

        DeviceSession& device_;
        extension_detail::EffectUniformSource source_;
        std::vector<Version> versions_;
    };

    struct LinkedUniformBlock final
    {
        UniformPoolId pool;
        GraphResourceId resource;
        std::vector<std::uint32_t> batches;
    };

    struct SelectedUniformVersion final
    {
        UniformPoolId pool;
        UniformVersionId version;
    };
} // namespace lux::render::detail
