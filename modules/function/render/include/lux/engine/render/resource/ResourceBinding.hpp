#pragma once

#include <NRI.h>
#include <cstdint>
#include <span>

namespace lux::render::detail
{
    class SubmissionFrontiers;

    // The owner keeps these addresses alive through the last frozen submission and its receipt.
    // Preparation orders the first use; accepted includes each physical queue that used the selected backing.
    struct ResourceQueueUses final
    {
        const SubmissionFrontiers* preparation = nullptr;
        SubmissionFrontiers* accepted = nullptr;
        bool operator==(const ResourceQueueUses&) const noexcept = default;
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
        ResourceQueueUses queue_uses{};
    };
} // namespace lux::render::detail
