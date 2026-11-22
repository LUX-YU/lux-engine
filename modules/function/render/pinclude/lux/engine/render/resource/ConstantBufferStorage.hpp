#pragma once

#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/extensions/execution/ResourceInput.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/graph/ConstantBlock.hpp>
#include <optional>
#include <vector>

namespace lux::render::detail
{
    class DeviceSession;
    class ResourceUploader;

    struct ConstantBufferVersion final
    {
        std::uint32_t slot = 0;
        std::uint64_t revision = 0;

        [[nodiscard]] explicit operator bool() const noexcept { return revision != 0; }
    };

    // One physical backing per live revision. A single device round borrows this storage at a time.
    class ConstantBufferStorage final
    {
    public:
        ConstantBufferStorage(DeviceSession&, std::uint32_t bytes, std::uint32_t frame_count) noexcept;
        ~ConstantBufferStorage() noexcept;
        ConstantBufferStorage(const ConstantBufferStorage&) = delete;
        ConstantBufferStorage& operator=(const ConstantBufferStorage&) = delete;

    private:
        friend class ConstantBufferSelections;

        [[nodiscard]] RenderResult<ConstantBufferVersion>
        pin(std::uint64_t revision, const SubmissionFrontiers&) noexcept;
        [[nodiscard]] RenderResult<void>
        stage(ConstantBufferVersion, std::span<const std::byte>, ResourceUploader&) noexcept;
        void settleUpload(ConstantBufferVersion, SubmissionPoint, RenderError) noexcept;
        void unpin(ConstantBufferVersion, bool quarantine) noexcept;
        [[nodiscard]] bool ready(ConstantBufferVersion) const noexcept;
        [[nodiscard]] ResourceInput binding(ConstantBufferVersion) noexcept;
        enum class EPhase : std::uint8_t { EMPTY, DIRTY, UPLOADING, READY, QUARANTINED };

        struct Version final
        {
            SubmissionFrontiers preparation, accepted;
            nri::Buffer* buffer = nullptr;
            std::uint64_t revision = 0;
            std::uint32_t prepared_pins = 0;
            EPhase phase = EPhase::EMPTY;
        };

        [[nodiscard]] const Version& resolve(ConstantBufferVersion) const noexcept;
        [[nodiscard]] Version& resolve(ConstantBufferVersion) noexcept;

        DeviceSession& device_;
        std::uint32_t bytes_;
        // Index in the currently active device round. The round clears it before releasing the last borrow.
        std::size_t selection_ = SIZE_MAX;
        std::vector<Version> versions_;
    };

    struct BoundConstantBuffer final
    {
        GraphResourceId resource;
        ConstantBufferStorage* storage;
        extension_detail::BoundConstantValue value;
    };

    // Entries live from the first source read until the device round settles its accepted batches.
    class ConstantBufferSelections final
    {
    public:
        [[nodiscard]] RenderResult<std::optional<ResourceInput>>
        prepare(const BoundConstantBuffer&, const SubmissionFrontiers&, ResourceUploader&) noexcept;
        void settle(SubmissionPoint, RenderError) noexcept;
        void finish(bool unknown) noexcept;

    private:
        struct Entry final
        {
            ConstantBufferStorage* storage;
            ConstantBufferVersion version{};
            std::vector<std::byte> pending;
            std::uint64_t revision = 0;
        };
        std::vector<Entry> entries_;
    };
} // namespace lux::render::detail
