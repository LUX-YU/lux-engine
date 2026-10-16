#pragma once

#include <lux/engine/description/TextureInfo.hpp>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>
#include <lux/engine/render/native/vulkan/transfer/Transfer.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>

namespace lux::render::vulkan
{
    class TextureStorage;
    // One cold Native candidate, never a Request owner or an independent completion registry.
    // Source is borrowed from the caller's retained original message until this object is destroyed.
    // advance is nonblocking; destroy only after completion/cancellation or host join + cold Session drain.
    class TextureUpload final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<TextureUpload>> create(
            DeviceSession&, const rdesc::TextureInfo&, std::span<const std::byte>,
            std::optional<TextureRegion> region = {}) noexcept;
        ~TextureUpload() noexcept;
        TextureUpload(const TextureUpload&) = delete;
        TextureUpload& operator=(const TextureUpload&) = delete;
        [[nodiscard]] RenderResult<bool> advance() noexcept;
        void setCompletionPost(CompletionPost post, std::shared_ptr<const void> implementation = {},
            std::shared_ptr<const void> registration = {}) noexcept
        {
            transfer_->setCompletionPost(post, std::move(implementation), std::move(registration));
        }
        void cancel() noexcept { transfer_->cancel(); }
        [[nodiscard]] VkDevice device() const noexcept;
        [[nodiscard]] const rdesc::TextureInfo& info() const noexcept { return info_; }
        [[nodiscard]] std::optional<SubmissionTicket> lastUse() const noexcept { return transfer_->lastUse(); }
        [[nodiscard]] const ImageAllocation& image() const noexcept;
        [[nodiscard]] RenderResult<ImagePermit> takeImage() noexcept;
        [[nodiscard]] RenderResult<void> publish() noexcept;
        [[nodiscard]] std::uint64_t prepareNs() const noexcept { return transfer_->prepareNs(); }
        [[nodiscard]] bool unstartedIn(const DeviceSession& session) const noexcept
        {
            return transfer_->unstartedIn(session);
        }

    private:
        friend class DeviceSession;
        friend class TextureStorage;
        TextureUpload(DeviceSession&, rdesc::TextureInfo, std::unique_ptr<Transfer>) noexcept;
        DeviceSession* session_;
        rdesc::TextureInfo info_;
        std::unique_ptr<Transfer> transfer_;
        TextureStorage* storage_ = nullptr;
        StorageInstanceId storage_id_;
        TextureStorage::Backing* version_ = nullptr;
        TextureId texture_;
        TextureStorage::Backing* base_ = nullptr;
    };
}
