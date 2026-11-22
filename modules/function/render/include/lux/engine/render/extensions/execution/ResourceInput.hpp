#pragma once

#include <NRI.h>
#include <cstdint>
#include <span>

namespace lux::render::detail
{
    class SubmissionFrontiers;

    // Both belong to the selected physical image. The owner keeps them stable through replacement or unbind.
    struct ImageQueueUses final
    {
        const SubmissionFrontiers* preparation = nullptr;
        SubmissionFrontiers* reads = nullptr;
        bool operator==(const ImageQueueUses&) const noexcept = default;
    };

    // Borrowed physical bindings. The supplying object retains their lifetime.
    struct ResourceInput final
    {
        nri::Buffer* buffer = nullptr;
        std::uint64_t offset = 0, size = 0;
        nri::Texture* image = nullptr;
        // Buffers reached indirectly through device addresses in this binding. The immutable
        // slice list changes only with its physical topology, independently of content updates.
        std::span<nri::Buffer* const> addressed_buffers;
        std::uint64_t addressed_revision = 0;
        ImageQueueUses image_uses{};
    };
} // namespace lux::render::detail
