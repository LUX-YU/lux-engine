#pragma once

#include <lux/engine/render/features/Texture.hpp>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>
#include <lux/cxx/container/StableSlotMap.hpp>

namespace lux::render::vulkan
{
    class TextureUpload;
    class TextureStorage;
    class ExecutionContext;
    struct TextureDescriptorTag;

    // Physical sampled version. Storage owns it; prepared Context leases pin it through real last-use.
    // This identity is independent of the stable business TextureId.
    class TextureVersion final
    {
    public:
        TextureVersion() noexcept = default;
        TextureVersion(const TextureVersion&) = delete;
        TextureVersion& operator=(const TextureVersion&) = delete;
        [[nodiscard]] const rdesc::TextureInfo& info() const noexcept { return info_; }
        [[nodiscard]] std::uint32_t descriptorIndex() const noexcept { return key_.index; }
        [[nodiscard]] const ImageAllocation& image() const noexcept { return *image_; }
        [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    private:
        friend class TextureStorage;
        friend class DeviceSession;
        friend class TextureUpload;
        friend class ExecutionContext;
        cxx::SlotKey<TextureDescriptorTag> key_;
        rdesc::TextureInfo info_{};
        ImagePermit permit_;
        ImageAllocation* image_ = nullptr;
        VkImageView view_ = VK_NULL_HANDLE;
        VkSampler sampler_ = VK_NULL_HANDLE;
        std::optional<SubmissionTicket> last_use_;
        std::uint64_t bytes_ = 0, revision_ = 0;
        std::uint32_t leases_ = 0;
        bool live_ = false, reserved_ = true;
    };

    class TextureStorage final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<TextureStorage>>
        create(DeviceSession& session, const TextureStorageConfig& config) noexcept;
        [[nodiscard]] bool compatibleConfig(const TextureStorageConfig& config) const noexcept
        {
            return capacity_ == config.capacity && byte_limit_ == config.byte_limit;
        }

        ~TextureStorage() noexcept;
        TextureStorage(const TextureStorage&) = delete;
        TextureStorage& operator=(const TextureStorage&) = delete;
        // Reserve exact descriptor/version and business identity BEFORE any GPU submit. Upload owns rollback.
        [[nodiscard]] RenderResult<TexturePublished> reserve(
            TextureUpload& upload, TextureId replace = {}, std::uint64_t expected_revision = 0) noexcept;
        [[nodiscard]] RenderResult<void> release(TextureId, std::uint64_t expected_revision) noexcept;
        [[nodiscard]] RenderResult<std::unique_ptr<TextureUpload>> uploadRegion(
            TextureId, std::uint64_t expected_revision, TextureRegion, std::span<const std::byte>) noexcept;
        [[nodiscard]] RenderResult<TextureVersion*> acquire(TextureId) noexcept;
        void releaseLease(TextureVersion& version) noexcept { --version.leases_; }
        [[nodiscard]] const TextureVersion* find(TextureId) const noexcept;
        [[nodiscard]] VkDescriptorSetLayout layout() const noexcept { return layout_; }
        [[nodiscard]] VkDescriptorSet descriptors() const noexcept { return descriptors_; }
        [[nodiscard]] std::size_t residentVersions() const noexcept { return versions_.size(); }
        [[nodiscard]] std::uint64_t residentBytes() const noexcept { return resident_bytes_; }
        [[nodiscard]] std::uint64_t residentPeak() const noexcept { return resident_peak_; }
    private:
        friend class DeviceSession;
        friend class TextureUpload;
        struct Record final { TextureVersion* current = nullptr; bool updating = false; };
        TextureStorage(DeviceSession& session, std::size_t capacity, std::uint64_t bytes) noexcept;
        [[nodiscard]] RenderResult<void> initialize() noexcept;
        void rollback(TextureUpload&) noexcept;
        void publish(TextureUpload&) noexcept;
        void collect() noexcept;
        void destroy(TextureVersion&) noexcept;
        DeviceSession& session_;
        cxx::SlotMap<Record, TextureTag> textures_;
        cxx::StableSlotMap<TextureVersion, TextureDescriptorTag, cxx::NoAux, 16> versions_;
        VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
        VkDescriptorPool pool_ = VK_NULL_HANDLE;
        VkDescriptorSet descriptors_ = VK_NULL_HANDLE;
        std::size_t capacity_;
        std::uint64_t byte_limit_, resident_bytes_ = 0, resident_peak_ = 0;
    };
}
