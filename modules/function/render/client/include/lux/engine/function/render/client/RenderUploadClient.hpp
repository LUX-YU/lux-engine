#pragma once
/**
 * @file RenderUploadClient.hpp
 * @brief Thread-safe, non-blocking producer facade for persistent GPU uploads.
 *
 * Producers build one fully owning packet and submit an intent.  They never
 * access RenderUploadSession, its SPSC channel or its callback registry.  A
 * process-level coordinator owns those endpoint details and settles the
 * returned RenderRequest at the main-thread adoption point.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/function/render/client/RenderRequest.hpp>
#include <lux/engine/function/render/client/RenderUploadSession.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <atomic>
#include <memory>
#include <span>
#include <utility>

namespace lux::rdesc
{
    enum class ETexturePixelFormat : std::uint32_t;
}

namespace lux::render
{
    struct UploadPayloadAccounting final
    {
        std::uint64_t shared_bytes{0u};
        std::uint64_t copied_bytes{0u};
    };

    struct RenderUploadClientStatistics final
    {
        std::uint64_t submitted_packets{0u};
        std::uint64_t payload_shared_bytes{0u};
        std::uint64_t payload_copied_bytes{0u};
    };

    namespace detail
    {
        struct RenderUploadClientMetrics final
        {
            std::atomic<std::uint64_t> submitted_packets{0u};
            std::atomic<std::uint64_t> payload_shared_bytes{0u};
            std::atomic<std::uint64_t> payload_copied_bytes{0u};
        };

        struct PreparedUpload final
        {
            OperationPacket<> packet;
            TypeId expected_reply_type{kInvalidTypeId};
            ReplyDispatchCallback callback;
        };
    }

    /// Copyable MPMC producer handle. Its erased state is owned through the
    /// control block and dispatched by a static trampoline; no RTTI, virtual
    /// dispatch or std::function task wrapper is involved in admission.
    class LUX_FUNCTION_PUBLIC RenderUploadClient final
    {
    public:
        using Builder = SingleOperationBuilder<>;
        using PreparedUpload = detail::PreparedUpload;
        using SubmitFunction = UploadSubmitNoReplyResult (*)(void*, std::shared_ptr<PreparedUpload>) noexcept;

        RenderUploadClient() noexcept = default;

        [[nodiscard]] static RenderUploadClient bind(
            std::shared_ptr<void> owner,
            SubmitFunction submit,
            std::shared_ptr<detail::RenderUploadClientMetrics> metrics = {}
        ) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return control_ && control_->submit != nullptr;
        }

        template <class Reply, class Record>
        [[nodiscard]] UploadSubmitResult<Reply>
        trySubmit(Record&& record, UploadPayloadAccounting accounting = {}) const
        {
            if (!control_ || control_->submit == nullptr)
                return lux::cxx::unexpected(ERenderUploadSubmitError::STOPPING);

            OperationPacket<> packet;
            Builder builder(packet);
            builder.begin();
            std::invoke(std::forward<Record>(record), builder);
            const bool has_valid_builder = builder.valid();
            const bool has_one_command = has_valid_builder && builder.commandCount() == 1u;
            const auto prepared_reply_type = has_valid_builder ? builder.preparedReplyType() : kInvalidTypeId;
            const bool has_expected_reply = has_one_command && prepared_reply_type != kInvalidTypeId;
            const bool has_valid_accounting = has_expected_reply && packet.sealAccounting();
            const bool is_invalid_payload = !has_expected_reply || !has_valid_accounting;
            if (is_invalid_payload)
            {
                return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
            }

            auto [request, callback] = RenderRequestFactory<Reply>::make();
            auto prepared = std::make_shared<PreparedUpload>();
            prepared->packet = std::move(packet);
            prepared->expected_reply_type = prepared_reply_type;
            prepared->callback = std::move(callback);

            auto submitted = control_->submit(control_->owner.get(), std::move(prepared));
            if (!submitted)
                return lux::cxx::unexpected(submitted.error());
            control_->record(accounting);
            return std::move(request);
        }

        template <class Record>
        [[nodiscard]] UploadSubmitNoReplyResult
        trySubmitNoReply(Record&& record, UploadPayloadAccounting accounting = {}) const
        {
            if (!control_ || control_->submit == nullptr)
                return lux::cxx::unexpected(ERenderUploadSubmitError::STOPPING);

            OperationPacket<> packet;
            Builder builder(packet);
            builder.begin();
            std::invoke(std::forward<Record>(record), builder);
            const bool has_valid_builder = builder.valid();
            const bool has_one_command = has_valid_builder && builder.commandCount() == 1u;
            const auto prepared_reply_type = has_valid_builder ? builder.preparedReplyType() : kInvalidTypeId;
            const bool has_no_reply = has_one_command && prepared_reply_type == kInvalidTypeId;
            const bool has_valid_accounting = has_no_reply && packet.sealAccounting();
            const bool is_invalid_payload = !has_no_reply || !has_valid_accounting;
            if (is_invalid_payload)
            {
                return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
            }

            auto prepared = std::make_shared<PreparedUpload>();
            prepared->packet = std::move(packet);
            auto submitted = control_->submit(control_->owner.get(), std::move(prepared));
            if (submitted)
                control_->record(accounting);
            return submitted;
        }

        [[nodiscard]] UploadSubmitResult<Texture2DCreatedReply> tryCreateTexture2D(
            lux::cxx::SharedBytes<> pixels,
            std::int32_t width,
            std::int32_t height,
            std::int32_t channels = 4,
            EPixelFormat format = EPixelFormat::RGBA8_SRGB,
            bool generate_mips = true
        ) const;

        /// Explicit ownership boundary for temporary caller memory. Unlike
        /// tryCreateTexture2D(SharedBytes), this named path performs one full
        /// CPU copy before admission and records it as such.
        [[nodiscard]] UploadSubmitResult<Texture2DCreatedReply> tryCreateTexture2DCopy(
            std::span<const std::byte> pixels,
            std::int32_t width,
            std::int32_t height,
            std::int32_t channels = 4,
            EPixelFormat format = EPixelFormat::RGBA8_SRGB,
            bool generate_mips = true
        ) const;

        [[nodiscard]] UploadSubmitResult<Texture2DCreatedReply> tryCreateTexture2D(
            lux::cxx::SharedBytes<> pixels,
            std::int32_t width,
            std::int32_t height,
            std::int32_t channels,
            lux::rdesc::ETexturePixelFormat format,
            bool generate_mips = true
        ) const;

        [[nodiscard]] UploadSubmitResult<Texture2DCreatedReply> tryCreateTexture2DMips(
            std::vector<OwnedTextureMipLevel> mip_levels,
            std::int32_t channels,
            EPixelFormat format = EPixelFormat::RGBA8_SRGB,
            bool generate_mips = false
        ) const;

        [[nodiscard]] UploadSubmitResult<Texture2DCreatedReply> tryCreateTexture2DMips(
            std::vector<OwnedTextureMipLevel> mip_levels,
            std::int32_t channels,
            lux::rdesc::ETexturePixelFormat format,
            bool generate_mips = false
        ) const;

        /// Thread-safe immutable-source submission for a stable-slot mip-range
        /// replacement. No source bytes are retained by the render backend.
        [[nodiscard]] UploadSubmitResult<TextureMipRangeReplacedReply> tryReplaceTexture2DMipRange(
            RTextureHandle handle,
            std::uint32_t base_mip,
            std::vector<OwnedTextureMipLevel> mip_levels,
            EPixelFormat format = EPixelFormat::RGBA8_SRGB,
            bool generate_mips = false
        ) const;

        [[nodiscard]] UploadSubmitResult<CubeTextureCreatedReply> tryCreateCubeTexture(
            OwnedCubeTextureFaces faces,
            std::int32_t face_size,
            std::int32_t channels = 4,
            EPixelFormat format = EPixelFormat::RGBA8_SRGB
        ) const;

        [[nodiscard]] UploadSubmitResult<Texture2DCreatedReply>
        tryCreatePersistentTexture2D(const PersistentTexture2DDesc& desc) const;

        [[nodiscard]] UploadSubmitResult<TextureRegionsAppliedReply>
        tryUpdateTextureRegions(OwnedTextureUploadBatch batch) const;

        void reapTextureCreate(
            ScopedRenderRequest<Texture2DCreatedReply>&& request,
            lux::cxx::move_only_function<void(RTextureHandle)> destroy
        ) const;

        [[nodiscard]] RenderUploadClientStatistics statistics() const noexcept
        {
            if (!control_ || !control_->metrics)
                return {};
            return {
                .submitted_packets = control_->metrics->submitted_packets.load(std::memory_order_relaxed),
                .payload_shared_bytes = control_->metrics->payload_shared_bytes.load(std::memory_order_relaxed),
                .payload_copied_bytes = control_->metrics->payload_copied_bytes.load(std::memory_order_relaxed),
            };
        }

    private:
        struct Control final
        {
            std::shared_ptr<void> owner;
            SubmitFunction submit{nullptr};
            std::shared_ptr<detail::RenderUploadClientMetrics> metrics;

            void record(UploadPayloadAccounting accounting) noexcept
            {
                metrics->submitted_packets.fetch_add(1u, std::memory_order_relaxed);
                metrics->payload_shared_bytes.fetch_add(accounting.shared_bytes, std::memory_order_relaxed);
                metrics->payload_copied_bytes.fetch_add(accounting.copied_bytes, std::memory_order_relaxed);
            }
        };

        [[nodiscard]] UploadSubmitResult<Texture2DCreatedReply> tryCreateTexture2DImpl(
            lux::cxx::SharedBytes<> pixels,
            std::int32_t width,
            std::int32_t height,
            std::int32_t channels,
            EPixelFormat format,
            bool generate_mips,
            UploadPayloadAccounting accounting
        ) const;

        explicit RenderUploadClient(std::shared_ptr<Control> control) noexcept : control_(std::move(control))
        {
        }

        std::shared_ptr<Control> control_;
    };
}
