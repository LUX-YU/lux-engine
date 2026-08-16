// 驻留 T7:贴图域子服务实现。配方注释见头文件;这里只记实现层决定。

#include <lux/engine/runtime/render/scene/detail/residency/subservices/TextureSubservice.hpp>
#include <lux/engine/runtime/render/scene/detail/residency/OwnerReplyReaper.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/TextureAsset.hpp>
#include <lux/engine/platform/FormatCompat.h>   // lux::format
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderUploadClient.hpp>

#include <utility>
#include <algorithm>
#include <limits>
#include <vector>

namespace lux::runtime
{
    namespace
    {
        void deferTextureRelease(
            lux::render::RenderControlSession& control,
            lux::render::RTextureHandle handle) noexcept
        {
            if (handle.isNull())
                return;
            control.destroyTexture(handle);
        }

        [[nodiscard]] bool collectMipLevels(
            const lux::rdesc::Texture& texture,
            std::uint32_t base_mip,
            std::vector<lux::render::OwnedTextureMipLevel>& output)
        {
            if (base_mip >= texture.mipCount() ||
                texture.mipCount() > lux::rdesc::kTextureMaxMipCount)
            {
                return false;
            }
            output.clear();
            output.reserve(texture.mipCount() - base_mip);
            for (std::uint32_t mip = base_mip;
                 mip < texture.mipCount();
                 ++mip)
            {
                const auto& range = texture.mipRange(mip);
                if (range.width == 0u || range.height == 0u ||
                    range.size == 0u ||
                    range.offset > texture.size() ||
                    range.size > texture.size() - range.offset ||
                    range.offset > std::numeric_limits<std::size_t>::max() ||
                    range.size > std::numeric_limits<std::size_t>::max())
                {
                    return false;
                }
                auto bytes = texture.pixels().subspan(
                    static_cast<std::size_t>(range.offset),
                    static_cast<std::size_t>(range.size));
                if (bytes.size() != range.size)
                    return false;
                output.push_back(lux::render::OwnedTextureMipLevel{
                    std::move(bytes), range.width, range.height});
            }
            return !output.empty();
        }
    }

    TextureSubservice::TextureSubservice(
        lux::render::RenderControlSession& control,
        lux::render::RenderUploadClient upload,
        lux::asset::AssetManager& assets,
        TryPostToMain post_main,
        TextureStreamingBudget budget) noexcept
        : control_(&control)
        , upload_(std::move(upload))
        , assets_(&assets)
        , post_main_(post_main)
        , budget_(budget)
        , mip_streaming_(std::make_shared<MipStreamingState>())
        , replies_(std::make_unique<detail::OwnerReplyReaper<
              lux::render::Texture2DCreatedReply>>(post_main))
        , replacement_replies_(
              std::make_unique<detail::OwnerReplyReaper<
                  lux::render::TextureMipRangeReplacedReply>>(
                      std::move(post_main)))
    {
    }

    TextureSubservice::~TextureSubservice() = default;

    lux::ecs::EResourceDomain TextureSubservice::domain() const
    {
        return lux::ecs::EResourceDomain::TEXTURE;
    }

    void TextureSubservice::submit(const lux::asset::asset_id_t& id, SubmitDone done)
    {
        trySubmit(id, std::move(done));
    }

    void TextureSubservice::trySubmit(
        const lux::asset::asset_id_t& id, SubmitDone done)
    {
        const auto* tex_asset = assets_->fetchAssetAs<lux::asset::TextureAsset>(id);
        if (tex_asset == nullptr || tex_asset->data() == nullptr)
        {
            // 编排前置被破坏(加载段之后数据就该在账)—— 响亮终败,不静默。
            done(0, "texture data absent at submit (load/submit ordering bug?)");
            return;
        }

        const lux::rdesc::Texture& tex = *tex_asset->data();
        const bool generate_mips =
            !lux::rdesc::isCompressedFormat(tex.pixelFormat())
            && !lux::rdesc::hasTextureFlag(
                tex.flags(),
                lux::rdesc::ETextureAssetFlags::NO_MIPS
            );

        auto submitted = [&]() -> lux::render::UploadSubmitResult<
            lux::render::Texture2DCreatedReply>
        {
            if (tex.mipCount() > 1u)
            {
                std::vector<lux::render::OwnedTextureMipLevel> mip_levels;
                if (!collectMipLevels(tex, 0u, mip_levels))
                {
                    return lux::cxx::unexpected(
                        lux::render::ERenderUploadSubmitError::PAYLOAD_INVALID);
                }
                return upload_.tryCreateTexture2DMips(
                    std::move(mip_levels),
                    tex.channel(),
                    tex.pixelFormat(),
                    false);
            }
            return upload_.tryCreateTexture2D(
                tex.pixels(),
                tex.width(),
                tex.height(),
                tex.channel(),
                tex.pixelFormat(),
                generate_mips);
        }();

        if (!submitted)
        {
            const auto error = submitted.error();
            if (error == lux::render::ERenderUploadSubmitError::QUEUE_FULL ||
                error == lux::render::ERenderUploadSubmitError::BYTE_BUDGET_EXHAUSTED)
            {
                if (post_main_ && post_main_(
                        [this, id, done = std::move(done)]() mutable noexcept
                        { trySubmit(id, std::move(done)); }))
                    return;
            }
            done(0, error == lux::render::ERenderUploadSubmitError::PAYLOAD_INVALID
                        ? "texture upload payload invalid"
                        : "texture upload channel stopping");
            return;
        }

        replies_->track(
            std::move(*submitted),
            [control = control_,
             state = mip_streaming_,
             id,
             done = std::move(done)]
            (const lux::render::Texture2DCreatedReply& r,
             bool compensation_only) mutable noexcept
            {
                if (compensation_only)
                {
                    deferTextureRelease(*control, r.handle);
                    return;
                }
                const bool ok = (r.status == 0 && !r.handle.isNull());
                if (ok)
                {
                    state->asset_by_handle[
                        lux::ecs::packHandleBits(r.handle)] = id;
                    done(lux::ecs::packHandleBits(r.handle), {});
                    return;
                }
                // 畸形失败回执也可能已转移一枚 owner。
                deferTextureRelease(*control, r.handle);
                done(0, lux::format(
                    "texture upload failed (status={})", r.status));
            }
        );
    }

    void TextureSubservice::applyMipDemands(
        const lux::render::TextureMipDemandsReply& demands)
    {
        std::uint32_t submitted_tasks = 0u;
        std::uint64_t submitted_bytes = 0u;
        const std::uint32_t count = std::min(
            demands.count,
            lux::render::kTextureMipDemandBatchCapacity);
        for (std::uint32_t index = 0u; index < count; ++index)
        {
            if (submitted_tasks >= budget_.maximum_replacement_tasks)
                break;
            const auto& demand = demands.entries[index];
            if (demand.handle.isNull() ||
                demand.target_base_mip == demand.resident_base_mip)
            {
                continue;
            }
            const std::uint64_t handle_bits =
                lux::ecs::packHandleBits(demand.handle);
            const auto asset_it =
                mip_streaming_->asset_by_handle.find(handle_bits);
            if (asset_it == mip_streaming_->asset_by_handle.end() ||
                mip_streaming_->replacements_inflight.contains(handle_bits))
            {
                continue;
            }
            const auto* texture_asset =
                assets_->fetchAssetAs<lux::asset::TextureAsset>(
                    asset_it->second);
            if (texture_asset == nullptr || texture_asset->data() == nullptr)
                continue;
            const auto& texture = *texture_asset->data();

            // Runtime-generated-only chains have no immutable source bytes for
            // a coarse logical base. They remain fully resident; Cooked asset
            // textures with explicit mip ranges take the streaming path.
            std::vector<lux::render::OwnedTextureMipLevel> mip_levels;
            if (texture.mipCount() <= 1u ||
                !collectMipLevels(
                    texture, demand.target_base_mip, mip_levels))
            {
                continue;
            }
            std::uint64_t replacement_bytes = 0u;
            for (const auto& mip : mip_levels)
                replacement_bytes += mip.pixels.size();
            if (submitted_tasks != 0u &&
                submitted_bytes + replacement_bytes >
                    budget_.maximum_replacement_bytes)
            {
                break;
            }
            lux::render::EPixelFormat render_format{};
            if (!lux::render::toPixelFormat(
                    texture.pixelFormat(), render_format))
            {
                continue;
            }
            auto submitted = upload_.tryReplaceTexture2DMipRange(
                demand.handle,
                demand.target_base_mip,
                std::move(mip_levels),
                render_format,
                false);
            if (!submitted)
                continue;

            ++submitted_tasks;
            submitted_bytes += replacement_bytes;
            mip_streaming_->replacements_inflight.insert(handle_bits);
            replacement_replies_->track(
                std::move(*submitted),
                [state = mip_streaming_, handle_bits](
                    const lux::render::TextureMipRangeReplacedReply&,
                    bool) noexcept
                {
                    state->replacements_inflight.erase(handle_bits);
                });
        }
    }

    void TextureSubservice::destroy(std::uint64_t handle_bits) noexcept
    {
        const auto handle =
            lux::ecs::unpackHandleBits<lux::render::RTextureHandle>(handle_bits);
        mip_streaming_->asset_by_handle.erase(handle_bits);
        mip_streaming_->replacements_inflight.erase(handle_bits);
        deferTextureRelease(*control_, handle);
    }

    std::size_t TextureSubservice::pendingReplies() const noexcept
    {
        return replies_->pending() + replacement_replies_->pending();
    }

    bool TextureSubservice::ownerControlsQuiescent() const noexcept
    {
        return replies_->pending() == 0 &&
            replacement_replies_->pending() == 0;
    }

    void TextureSubservice::abandonPendingReplies() noexcept
    {
        replies_->abandon();
        replacement_replies_->abandon();
    }

} // namespace lux::runtime
