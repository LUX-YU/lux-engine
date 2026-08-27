#include <lux/engine/function/render/client/RenderUploadClient.hpp>

#include <lux/engine/description/Texture.hpp>

#include <algorithm>
#include <vector>

namespace lux::render
{
    RenderUploadClient RenderUploadClient::bind(
        std::shared_ptr<void> owner,
        SubmitFunction submit,
        std::shared_ptr<detail::RenderUploadClientMetrics> metrics
    ) noexcept
    {
        if (!owner || submit == nullptr)
            return {};
        auto control = std::make_shared<Control>();
        control->owner = std::move(owner);
        control->submit = submit;
        control->metrics = metrics ? std::move(metrics) : std::make_shared<detail::RenderUploadClientMetrics>();
        return RenderUploadClient{std::move(control)};
    }

    UploadSubmitResult<Texture2DCreatedReply> RenderUploadClient::tryCreateTexture2D(
        lux::cxx::SharedBytes<> pixels,
        std::int32_t width,
        std::int32_t height,
        std::int32_t channels,
        EPixelFormat format,
        bool generate_mips
    ) const
    {
        const auto bytes = pixels.size();
        return tryCreateTexture2DImpl(
            std::move(pixels),
            width,
            height,
            channels,
            format,
            generate_mips,
            UploadPayloadAccounting{.shared_bytes = bytes}
        );
    }

    UploadSubmitResult<Texture2DCreatedReply> RenderUploadClient::tryCreateTexture2DCopy(
        std::span<const std::byte> pixels,
        std::int32_t width,
        std::int32_t height,
        std::int32_t channels,
        EPixelFormat format,
        bool generate_mips
    ) const
    {
        const auto bytes = pixels.size();
        return tryCreateTexture2DImpl(
            lux::cxx::SharedBytes<>::copyOf(pixels),
            width,
            height,
            channels,
            format,
            generate_mips,
            UploadPayloadAccounting{.copied_bytes = bytes}
        );
    }

    UploadSubmitResult<Texture2DCreatedReply> RenderUploadClient::tryCreateTexture2DImpl(
        lux::cxx::SharedBytes<> pixels,
        std::int32_t width,
        std::int32_t height,
        std::int32_t channels,
        EPixelFormat format,
        bool generate_mips,
        UploadPayloadAccounting accounting
    ) const
    {
        if (pixels.size() > UINT32_MAX)
            return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
        const auto byte_count = static_cast<std::uint32_t>(pixels.size());
        const TextureUploadMipInput mip{
            static_cast<std::uint32_t>(std::max(0, width)),
            static_cast<std::uint32_t>(std::max(0, height)),
            byte_count};
        if (pixels.empty() || byte_count == 0u || !validateTexture2DUpload(format, 1u, &mip, UINT32_MAX).ok())
        {
            return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
        }

        return trySubmit<Texture2DCreatedReply>(
            [pixels = std::move(pixels), width, height, channels, format, generate_mips, mip](
                Builder& builder) mutable {
                CreateTexture2DPayload payload{};
                payload.width = width;
                payload.height = height;
                payload.channels = channels;
                payload.format = format;
                payload.generate_mips = generate_mips;
                payload.mip_count = 1u;
                payload.mips[0].pixels = builder.pushSharedBytes(pixels);
                payload.mips[0].width = mip.width;
                payload.mips[0].height = mip.height;
                builder.pushPreparedResource(type_ids::CreateTexture2D, payload);
            },
            accounting
        );
    }

    UploadSubmitResult<Texture2DCreatedReply> RenderUploadClient::tryCreateTexture2D(
        lux::cxx::SharedBytes<> pixels,
        std::int32_t width,
        std::int32_t height,
        std::int32_t channels,
        lux::rdesc::ETexturePixelFormat format,
        bool generate_mips
    ) const
    {
        EPixelFormat translated{};
        if (!toPixelFormat(format, translated))
            return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
        return tryCreateTexture2D(std::move(pixels), width, height, channels, translated, generate_mips);
    }

    UploadSubmitResult<Texture2DCreatedReply> RenderUploadClient::tryCreateTexture2DMips(
        std::vector<OwnedTextureMipLevel> mip_levels,
        std::int32_t channels,
        EPixelFormat format,
        bool generate_mips
    ) const
    {
        const auto mip_count = static_cast<std::uint32_t>(mip_levels.size());
        if (mip_count == 0u || mip_count > kTextureUploadMaxMipCount)
        {
            return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
        }

        TextureUploadMipInput inputs[kTextureUploadMaxMipCount]{};
        std::uint64_t shared_bytes = 0u;
        for (std::uint32_t i = 0u; i < mip_count; ++i)
        {
            if (mip_levels[i].pixels.empty() || mip_levels[i].pixels.size() > UINT32_MAX)
            {
                return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
            }
            inputs[i] = TextureUploadMipInput{
                mip_levels[i].width,
                mip_levels[i].height,
                static_cast<std::uint32_t>(mip_levels[i].pixels.size())};
            shared_bytes += mip_levels[i].pixels.size();
        }
        if (!validateTexture2DUpload(format, mip_count, inputs, UINT32_MAX).ok())
        {
            return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
        }

        return trySubmit<Texture2DCreatedReply>(
            [mip_levels = std::move(mip_levels), mip_count, channels, format, generate_mips](Builder& builder) mutable {
                CreateTexture2DPayload payload{};
                payload.width = static_cast<std::int32_t>(mip_levels[0].width);
                payload.height = static_cast<std::int32_t>(mip_levels[0].height);
                payload.channels = channels;
                payload.format = format;
                payload.generate_mips = generate_mips;
                payload.mip_count = mip_count;
                for (std::uint32_t i = 0u; i < mip_count; ++i)
                {
                    payload.mips[i].pixels = builder.pushSharedBytes(mip_levels[i].pixels);
                    payload.mips[i].width = mip_levels[i].width;
                    payload.mips[i].height = mip_levels[i].height;
                }
                builder.pushPreparedResource(type_ids::CreateTexture2D, payload);
            },
            UploadPayloadAccounting{.shared_bytes = shared_bytes}
        );
    }

    UploadSubmitResult<Texture2DCreatedReply> RenderUploadClient::tryCreateTexture2DMips(
        std::vector<OwnedTextureMipLevel> mip_levels,
        std::int32_t channels,
        lux::rdesc::ETexturePixelFormat format,
        bool generate_mips
    ) const
    {
        EPixelFormat translated{};
        if (!toPixelFormat(format, translated))
        {
            return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
        }
        return tryCreateTexture2DMips(std::move(mip_levels), channels, translated, generate_mips);
    }

    UploadSubmitResult<TextureMipRangeReplacedReply> RenderUploadClient::tryReplaceTexture2DMipRange(
        RTextureHandle handle,
        std::uint32_t base_mip,
        std::vector<OwnedTextureMipLevel> mip_levels,
        EPixelFormat format,
        bool generate_mips
    ) const
    {
        const auto mip_count = static_cast<std::uint32_t>(mip_levels.size());
        if (handle.isNull() || mip_count == 0u || mip_count > kTextureUploadMaxMipCount)
        {
            return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
        }

        TextureUploadMipInput inputs[kTextureUploadMaxMipCount]{};
        std::uint64_t shared_bytes = 0u;
        for (std::uint32_t i = 0u; i < mip_count; ++i)
        {
            if (mip_levels[i].pixels.empty() || mip_levels[i].pixels.size() > UINT32_MAX)
            {
                return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
            }
            inputs[i] = TextureUploadMipInput{
                mip_levels[i].width,
                mip_levels[i].height,
                static_cast<std::uint32_t>(mip_levels[i].pixels.size())};
            shared_bytes += mip_levels[i].pixels.size();
        }
        if (!validateTexture2DUpload(format, mip_count, inputs, UINT32_MAX).ok())
        {
            return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
        }

        return trySubmit<TextureMipRangeReplacedReply>(
            [handle, base_mip, mip_levels = std::move(mip_levels), mip_count, format, generate_mips](
                Builder& builder) mutable {
                ReplaceTexture2DMipRangePayload payload{};
                payload.handle = handle;
                payload.format = format;
                payload.generate_mips = generate_mips;
                payload.base_mip = base_mip;
                payload.mip_count = mip_count;
                for (std::uint32_t i = 0u; i < mip_count; ++i)
                {
                    payload.mips[i].pixels = builder.pushSharedBytes(mip_levels[i].pixels);
                    payload.mips[i].width = mip_levels[i].width;
                    payload.mips[i].height = mip_levels[i].height;
                }
                builder.pushPreparedResource(type_ids::ReplaceTexture2DMipRange, payload);
            },
            UploadPayloadAccounting{.shared_bytes = shared_bytes}
        );
    }

    UploadSubmitResult<CubeTextureCreatedReply> RenderUploadClient::tryCreateCubeTexture(
        OwnedCubeTextureFaces faces,
        std::int32_t face_size,
        std::int32_t channels,
        EPixelFormat format
    ) const
    {
        std::uint64_t sizes[6]{};
        std::uint64_t shared_bytes = 0u;
        for (std::size_t i = 0u; i < 6u; ++i)
        {
            if (faces[i].empty())
            {
                return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
            }
            sizes[i] = faces[i].size();
            shared_bytes += sizes[i];
        }
        if (!validateCubeUpload(format, face_size, sizes, UINT32_MAX).ok())
        {
            return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
        }

        return trySubmit<CubeTextureCreatedReply>(
            [faces = std::move(faces), face_size, channels, format](Builder& builder) mutable {
                CreateCubeTexturePayload payload{};
                for (std::size_t i = 0u; i < 6u; ++i)
                {
                    payload.face_data[i] = builder.pushSharedBytes(faces[i]);
                }
                payload.face_size = face_size;
                payload.channels = channels;
                payload.format = format;
                builder.pushPreparedResource(type_ids::CreateCubeTexture, payload);
            },
            UploadPayloadAccounting{.shared_bytes = shared_bytes}
        );
    }

    UploadSubmitResult<Texture2DCreatedReply>
    RenderUploadClient::tryCreatePersistentTexture2D(const PersistentTexture2DDesc& desc) const
    {
        if (!validatePersistentTexture2DDesc(desc).ok())
            return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);

        return trySubmit<Texture2DCreatedReply>([&desc](Builder& builder) {
            builder.pushPreparedResource(
                type_ids::CreatePersistentTexture2D,
                CreatePersistentTexture2DPayload{.desc = desc}
            );
        }
        );
    }

    UploadSubmitResult<TextureRegionsAppliedReply>
    RenderUploadClient::tryUpdateTextureRegions(OwnedTextureUploadBatch batch) const
    {
        const bool is_invalid_destination = batch.dst.isNull();
        const bool is_empty_payload = batch.regions.empty() || batch.pixels.empty();
        const bool is_oversized_payload = batch.regions.size() > UINT32_MAX || batch.pixels.size() > UINT32_MAX;
        const bool is_invalid_payload = is_invalid_destination || is_empty_payload || is_oversized_payload;
        if (is_invalid_payload)
        {
            return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
        }

        auto regions = std::make_shared<std::vector<TextureRegionDesc>>(std::move(batch.regions));
        const auto shared_bytes = batch.pixels.size() + regions->size() * sizeof(TextureRegionDesc);
        return trySubmit<TextureRegionsAppliedReply>(
            [regions = std::move(regions), batch = std::move(batch)](Builder& builder) mutable {
                UpdateTextureRegionsPayload payload{};
                payload.handle = batch.dst;
                payload.content_revision = batch.content_revision;
                payload.region_count = static_cast<std::uint32_t>(regions->size());
                payload.regions = builder.pushSharedBytes(
                    std::static_pointer_cast<const void>(regions),
                    reinterpret_cast<const std::byte*>(regions->data()),
                    static_cast<std::uint32_t>(regions->size() * sizeof(TextureRegionDesc))
                );
                payload.pixels = builder.pushSharedBytes(batch.pixels);
                builder.pushPreparedResource(type_ids::UpdateTextureRegions, payload);
            },
            UploadPayloadAccounting{.shared_bytes = shared_bytes}
        );
    }

    void RenderUploadClient::reapTextureCreate(
        ScopedRenderRequest<Texture2DCreatedReply>&& request,
        lux::cxx::move_only_function<void(RTextureHandle)> destroy
    ) const
    {
        if (!request.valid())
            return;
        auto observation = request.release();
        observation.then([destroy = std::move(destroy)](const Texture2DCreatedReply& reply) mutable noexcept {
            if (!reply.handle.isNull() && destroy)
                destroy(reply.handle);
        }
        );
    }
}
