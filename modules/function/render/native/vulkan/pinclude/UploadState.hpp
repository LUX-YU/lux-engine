#pragma once
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/transfer/TextureUpload.hpp>

namespace lux::render::vulkan
{
    struct DeviceSession::UploadState final
    {
        struct Page final
        {
            BufferPermit permit;
            BufferAllocation* buffer = nullptr;
            std::byte* mapped = nullptr;
            bool leased = false;
        };
        DeviceSession& session;
        SubmissionQueue* copy_queue = nullptr;
        TextureUploadOptions options;
        std::vector<Page> pages;
        TextureUploadStats stats;
        UploadState(DeviceSession& session, TextureUploadOptions options) noexcept
            : session(session), options(options) {}
        ~UploadState() noexcept;
    };
}
