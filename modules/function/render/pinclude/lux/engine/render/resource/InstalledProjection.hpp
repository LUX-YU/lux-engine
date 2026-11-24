#pragma once

#include <lux/engine/render/core/FinalBufferVersion.hpp>
#include <lux/engine/render/graph/ImportedResourceSlot.hpp>
#include <lux/engine/render/resource/ResourceId.hpp>

namespace lux::render::detail { struct GraphBindingTag; struct SubmissionPoint; }

namespace lux::render::runtime_detail
{
    struct ProductAccess;

    struct ProjectionSubscription final
    {
        ResourceInstanceId storage;
        std::uint32_t projection;
        cxx::SlotKey<detail::GraphBindingTag> subscription{};
    };

    struct BufferProductBinding final
    {
        ResourceInstanceId storage;
        std::uint32_t projection;
        const detail::ImportedResourceSlot* slot;
        std::optional<FinalBufferVersion> publication;
        // The physical buffer row stays in its Resource while prepared or in use by any queue.
        ProductAccess* pin = nullptr;
        void accept(detail::SubmissionPoint) const noexcept;
        void releasePin() noexcept;
    };
} // namespace lux::render::runtime_detail
