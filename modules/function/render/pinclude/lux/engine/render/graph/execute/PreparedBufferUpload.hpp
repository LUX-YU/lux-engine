#pragma once

#include <lux/engine/render/resource/StorageBinding.hpp>
#include <NRI.h>

namespace lux::render::compiled { class RenderGraph; }

namespace lux::render::detail
{
    class DeviceSession;
    struct FrameResources;
    class ResourceUploader;

    struct PreparedBufferInput final
    {
        enum class ESource { MISSING, SIZE_ONLY, RESIDENT, OWNED_BYTES };

        ESource source = ESource::MISSING;
        std::vector<std::byte> bytes;
        std::uint64_t revision = 0, size = 0, resident_generation = 0;
        std::size_t staged_bytes = 0;
    };

    struct PreparedBufferCopy final
    {
        std::uint32_t buffer;
        std::uint64_t generation;
        ReplicaCopyRange ranges;
        std::uint64_t begin, end, size, revision;
    };

    // Each successful staging call records the native generation and the exact copy ranges selected in this window.
    class PreparedUploadWindow final
    {
    public:
        PreparedUploadWindow(
            DeviceSession& device, ResourceUploader* uploader, std::vector<PreparedBufferCopy>& copies
        ) noexcept
            : device_(device), uploader_(uploader), copies_(copies) {}

        [[nodiscard]] RenderResult<bool>
        stageFrame(FrameResources&, const compiled::RenderGraph&, std::span<PreparedBufferInput>) noexcept;

    private:
        [[nodiscard]] RenderResult<bool>
        stage(nri::Buffer&, std::uint32_t slot, std::uint64_t generation, PreparedBufferInput&) noexcept;

        DeviceSession& device_;
        ResourceUploader* uploader_;
        std::vector<PreparedBufferCopy>& copies_;
    };
} // namespace lux::render::detail
