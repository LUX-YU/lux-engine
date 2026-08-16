#include <lux/engine/function/render/client/RenderUploadSession.hpp>

#include <lux/engine/description/Texture.hpp>

#include <vector>

namespace lux::render
{
    RenderUploadSession::RenderUploadSession(
        std::shared_ptr<RenderUploadChannel<>> channel,
        std::shared_ptr<RenderChannelSync> sync)
        : channel_(std::move(channel))
        , sync_(std::move(sync))
        , owner_thread_token_(currentThreadToken())
    {
    }

    void RenderUploadSession::pumpReplies()
    {
        requireOwnerThread();
        while (channel_->responses.tryAcquireRead())
        {
            callbacks_.dispatchAll(channel_->responses.currentRead());
            sync_->notifyReplyConsumed();
        }
    }

    bool RenderUploadSession::waitAndPumpReplies()
    {
        requireOwnerThread();
        if (channel_->responses.tryAcquireRead())
        {
            callbacks_.dispatchAll(channel_->responses.currentRead());
            sync_->notifyReplyConsumed();
            pumpReplies();
            return true;
        }

        const auto observed =
            sync_->reply_epoch.load(std::memory_order_acquire);
        if (channel_->responses.tryAcquireRead())
        {
            callbacks_.dispatchAll(channel_->responses.currentRead());
            sync_->notifyReplyConsumed();
            pumpReplies();
            return true;
        }
        if (sync_->isStopping())
            return false;
        sync_->reply_epoch.wait(observed, std::memory_order_acquire);
        if (sync_->isStopping())
            return false;
        pumpReplies();
        return true;
    }

    UploadSubmitResult<Texture2DCreatedReply>
    RenderUploadSession::tryCreateTexture2D(
        lux::cxx::SharedBytes<> pixels,
        std::int32_t width,
        std::int32_t height,
        std::int32_t channels,
        EPixelFormat format,
        bool generate_mips)
    {
        if (pixels.size() > UINT32_MAX)
            return lux::cxx::unexpected(
                ERenderUploadSubmitError::PAYLOAD_INVALID);
        const auto byte_count = static_cast<std::uint32_t>(pixels.size());
        const TextureUploadMipInput mip{
            static_cast<std::uint32_t>(std::max(0, width)),
            static_cast<std::uint32_t>(std::max(0, height)),
            byte_count};
        if (pixels.empty() || byte_count == 0 ||
            !validateTexture2DUpload(
                format, 1, &mip, UINT32_MAX).ok())
            return lux::cxx::unexpected(
                ERenderUploadSubmitError::PAYLOAD_INVALID);

        return trySubmit<Texture2DCreatedReply>(
            [pixels = std::move(pixels), width, height, channels,
             format, generate_mips, mip](Builder& builder, auto callback)
                mutable
            {
                CreateTexture2DPayload payload{};
                payload.width         = width;
                payload.height        = height;
                payload.channels      = channels;
                payload.format        = format;
                payload.generate_mips = generate_mips;
                payload.mip_count     = 1;
                payload.mips[0].pixels = builder.pushSharedBytes(pixels);
                payload.mips[0].width = mip.width;
                payload.mips[0].height = mip.height;
                builder.pushResource(
                    type_ids::CreateTexture2D,
                    payload,
                    std::move(callback));
            });
    }

    UploadSubmitResult<Texture2DCreatedReply>
    RenderUploadSession::tryCreateTexture2D(
        lux::cxx::SharedBytes<> pixels,
        std::int32_t width,
        std::int32_t height,
        std::int32_t channels,
        lux::rdesc::ETexturePixelFormat format,
        bool generate_mips)
    {
        EPixelFormat translated{};
        if (!toPixelFormat(format, translated))
            return lux::cxx::unexpected(
                ERenderUploadSubmitError::PAYLOAD_INVALID);
        return tryCreateTexture2D(
            std::move(pixels), width, height, channels,
            translated, generate_mips);
    }

    UploadSubmitResult<Texture2DCreatedReply>
    RenderUploadSession::tryCreateTexture2DMips(
        std::vector<OwnedTextureMipLevel> mip_levels,
        std::int32_t channels,
        EPixelFormat format,
        bool generate_mips)
    {
        const auto mip_count = static_cast<std::uint32_t>(mip_levels.size());
        if (mip_count == 0u ||
            mip_count > kTextureUploadMaxMipCount)
            return lux::cxx::unexpected(
                ERenderUploadSubmitError::PAYLOAD_INVALID);

        TextureUploadMipInput inputs[kTextureUploadMaxMipCount]{};
        for (std::uint32_t i = 0; i < mip_count; ++i)
        {
            if (mip_levels[i].pixels.empty() ||
                mip_levels[i].pixels.size() > UINT32_MAX)
                return lux::cxx::unexpected(
                    ERenderUploadSubmitError::PAYLOAD_INVALID);
            inputs[i] = TextureUploadMipInput{
                mip_levels[i].width,
                mip_levels[i].height,
                static_cast<std::uint32_t>(mip_levels[i].pixels.size())};
        }
        if (!validateTexture2DUpload(
                format, mip_count, inputs, UINT32_MAX).ok())
            return lux::cxx::unexpected(
                ERenderUploadSubmitError::PAYLOAD_INVALID);

        return trySubmit<Texture2DCreatedReply>(
            [mip_levels = std::move(mip_levels), mip_count, channels,
             format, generate_mips](Builder& builder, auto callback) mutable
            {
                CreateTexture2DPayload payload{};
                payload.width = static_cast<std::int32_t>(
                    mip_levels[0].width);
                payload.height = static_cast<std::int32_t>(
                    mip_levels[0].height);
                payload.channels      = channels;
                payload.format        = format;
                payload.generate_mips = generate_mips;
                payload.mip_count     = mip_count;
                for (std::uint32_t i = 0; i < mip_count; ++i)
                {
                    payload.mips[i].pixels = builder.pushSharedBytes(
                        mip_levels[i].pixels);
                    payload.mips[i].width  = mip_levels[i].width;
                    payload.mips[i].height = mip_levels[i].height;
                }
                builder.pushResource(
                    type_ids::CreateTexture2D,
                    payload,
                    std::move(callback));
            });
    }

    UploadSubmitResult<Texture2DCreatedReply>
    RenderUploadSession::tryCreateTexture2DMips(
        std::vector<OwnedTextureMipLevel> mip_levels,
        std::int32_t channels,
        lux::rdesc::ETexturePixelFormat format,
        bool generate_mips)
    {
        EPixelFormat translated{};
        if (!toPixelFormat(format, translated))
            return lux::cxx::unexpected(
                ERenderUploadSubmitError::PAYLOAD_INVALID);
        return tryCreateTexture2DMips(
            std::move(mip_levels),
            channels,
            translated,
            generate_mips);
    }

    UploadSubmitResult<TextureMipRangeReplacedReply>
    RenderUploadSession::tryReplaceTexture2DMipRange(
        RTextureHandle handle,
        std::uint32_t base_mip,
        std::vector<OwnedTextureMipLevel> mip_levels,
        EPixelFormat format,
        bool generate_mips)
    {
        const auto mip_count = static_cast<std::uint32_t>(mip_levels.size());
        if (handle.isNull() || mip_count == 0u ||
            mip_count > kTextureUploadMaxMipCount)
        {
            return lux::cxx::unexpected(
                ERenderUploadSubmitError::PAYLOAD_INVALID);
        }

        TextureUploadMipInput inputs[kTextureUploadMaxMipCount]{};
        for (std::uint32_t i = 0u; i < mip_count; ++i)
        {
            if (mip_levels[i].pixels.empty() ||
                mip_levels[i].pixels.size() > UINT32_MAX)
            {
                return lux::cxx::unexpected(
                    ERenderUploadSubmitError::PAYLOAD_INVALID);
            }
            inputs[i] = TextureUploadMipInput{
                mip_levels[i].width,
                mip_levels[i].height,
                static_cast<std::uint32_t>(mip_levels[i].pixels.size())};
        }
        if (!validateTexture2DUpload(
                format, mip_count, inputs, UINT32_MAX).ok())
        {
            return lux::cxx::unexpected(
                ERenderUploadSubmitError::PAYLOAD_INVALID);
        }

        return trySubmit<TextureMipRangeReplacedReply>(
            [handle, base_mip, mip_levels = std::move(mip_levels),
             mip_count, format, generate_mips](Builder& builder, auto callback)
                mutable
            {
                ReplaceTexture2DMipRangePayload payload{};
                payload.handle = handle;
                payload.format = format;
                payload.generate_mips = generate_mips;
                payload.base_mip = base_mip;
                payload.mip_count = mip_count;
                for (std::uint32_t i = 0u; i < mip_count; ++i)
                {
                    payload.mips[i].pixels = builder.pushSharedBytes(
                        mip_levels[i].pixels);
                    payload.mips[i].width = mip_levels[i].width;
                    payload.mips[i].height = mip_levels[i].height;
                }
                builder.pushResource(
                    type_ids::ReplaceTexture2DMipRange,
                    payload,
                    std::move(callback));
            });
    }

    UploadSubmitResult<CubeTextureCreatedReply>
    RenderUploadSession::tryCreateCubeTexture(
        OwnedCubeTextureFaces faces,
        std::int32_t face_size,
        std::int32_t channels,
        EPixelFormat format)
    {
        std::uint64_t sizes[6]{};
        for (std::size_t i = 0; i < 6; ++i)
        {
            if (faces[i].empty())
                return lux::cxx::unexpected(
                    ERenderUploadSubmitError::PAYLOAD_INVALID);
            sizes[i] = faces[i].size();
        }
        if (!validateCubeUpload(
                format, face_size, sizes, UINT32_MAX).ok())
            return lux::cxx::unexpected(
                ERenderUploadSubmitError::PAYLOAD_INVALID);

        return trySubmit<CubeTextureCreatedReply>(
            [faces = std::move(faces), face_size, channels,
             format](Builder& builder, auto callback) mutable
            {
                CreateCubeTexturePayload payload{};
                for (std::size_t i = 0; i < 6; ++i)
                    payload.face_data[i] = builder.pushSharedBytes(faces[i]);
                payload.face_size = face_size;
                payload.channels  = channels;
                payload.format    = format;
                builder.pushResource(
                    type_ids::CreateCubeTexture,
                    payload,
                    std::move(callback));
            });
    }

    UploadSubmitResult<Texture2DCreatedReply>
    RenderUploadSession::tryCreatePersistentTexture2D(
        const PersistentTexture2DDesc& desc)
    {
        if (!validatePersistentTexture2DDesc(desc).ok())
            return lux::cxx::unexpected(
                ERenderUploadSubmitError::PAYLOAD_INVALID);

        return trySubmit<Texture2DCreatedReply>(
            [&desc](Builder& builder, auto callback)
            {
                builder.pushResource(
                    type_ids::CreatePersistentTexture2D,
                    CreatePersistentTexture2DPayload{.desc = desc},
                    std::move(callback)
                );
            }
        );
    }

    UploadSubmitResult<TextureRegionsAppliedReply>
    RenderUploadSession::tryUpdateTextureRegions(
        OwnedTextureUploadBatch batch)
    {
        if (batch.dst.isNull() || batch.regions.empty() ||
            batch.pixels.empty() ||
            batch.regions.size() > UINT32_MAX ||
            batch.pixels.size() > UINT32_MAX)
            return lux::cxx::unexpected(
                ERenderUploadSubmitError::PAYLOAD_INVALID);

        const std::size_t region_bytes =
            batch.regions.size() * sizeof(TextureRegionDesc);
        auto owned_regions = std::make_shared<std::vector<TextureRegionDesc>>(
            std::move(batch.regions));

        return trySubmit<TextureRegionsAppliedReply>(
            [owned_regions = std::move(owned_regions), batch](
                Builder& builder, auto callback) mutable
            {
                UpdateTextureRegionsPayload payload{};
                payload.handle           = batch.dst;
                payload.content_revision = batch.content_revision;
                payload.region_count = static_cast<std::uint32_t>(
                    owned_regions->size());
                payload.regions = builder.pushSharedBytes(
                    std::static_pointer_cast<const void>(owned_regions),
                    reinterpret_cast<const std::byte*>(
                        owned_regions->data()),
                    static_cast<std::uint32_t>(
                        owned_regions->size() * sizeof(TextureRegionDesc))
                );
                payload.pixels = builder.pushSharedBytes(batch.pixels);
                builder.pushResource(
                    type_ids::UpdateTextureRegions,
                    payload,
                    std::move(callback)
                );
            }
        );
    }

    std::uint64_t RenderUploadSession::currentThreadToken() noexcept
    {
        static std::atomic<std::uint64_t> next{1u};
        thread_local const std::uint64_t token =
            next.fetch_add(1u, std::memory_order_relaxed);
        return token;
    }

    bool RenderUploadSession::claimCoordinatorThread() noexcept
    {
        if (!coordinatorOwned())
            return false;

        const auto current = currentThreadToken();
        std::uint64_t unclaimed = 0u;
        if (owner_thread_token_.compare_exchange_strong(
                unclaimed,
                current,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            return true;
        }
        return unclaimed == current;
    }

    void RenderUploadSession::requireOwnerThread() noexcept
    {
        if (owner_thread_token_.load(std::memory_order_acquire) !=
            currentThreadToken())
        {
            renderFatal(
                "RenderUploadSession accessed outside its coordinator owner thread");
        }
    }
} // namespace lux::render
